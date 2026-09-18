#include "ShadowMap.h"
#include "Utils.h"
#include <cmath>
#include <algorithm>

// ============================================================
//  Нелинейное распределение каскадов (practical split scheme)
//
//  Чисто логарифмическое разбиение n*(f/n)^(i/N) оптимально по
//  плотности текселей, но при малом near первый каскад получается
//  исчезающе тонким. Равномерное — наоборот, тратит почти всё
//  разрешение на далёкую геометрию. Смесь с весом lambda берёт
//  лучшее от обоих; 0.85 — типичное значение с уклоном в лог.
// ============================================================
void ComputeCascadeSplits(float nearZ, float farZ, int count, float lambda,
    float* outSplits)
{
    if (count <= 0 || !outSplits) return;
    if (nearZ < 1e-4f) nearZ = 1e-4f;          // log расходится в нуле
    if (farZ <= nearZ) farZ = nearZ + 1e-3f;
    if (lambda < 0.f) lambda = 0.f;
    if (lambda > 1.f) lambda = 1.f;

    const float ratio = farZ / nearZ;
    for (int i = 0; i < count; ++i)
    {
        const float p = static_cast<float>(i + 1) / static_cast<float>(count);
        const float logSplit = nearZ * powf(ratio, p);
        const float uniSplit = nearZ + (farZ - nearZ) * p;
        outSplits[i] = lambda * logSplit + (1.0f - lambda) * uniSplit;
    }
    outSplits[count - 1] = farZ;               // последний точно до дальней плоскости
}

// ============================================================
void CascadedShadowMap::Create(ID3D12Device* device,
    ID3D12DescriptorHeap* srvHeap, UINT srvSlot, UINT srvDescriptorSize,
    UINT resolution, int cascadeCount)
{
    resolution_ = resolution;
    cascadeCount_ = (cascadeCount < 1) ? 1 :
        (cascadeCount > MAX_CASCADES ? MAX_CASCADES : cascadeCount);

    // ---- текстура: массив слоёв глубины ----
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = resolution_;
    rd.Height = resolution_;
    rd.DepthOrArraySize = static_cast<UINT16>(cascadeCount_);
    rd.MipLevels = 1;
    // TYPELESS: один и тот же ресурс читается как R32_FLOAT и пишется как D32_FLOAT
    rd.Format = DXGI_FORMAT_R32_TYPELESS;
    rd.SampleDesc = { 1, 0 };
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clear = {};
    clear.Format = DXGI_FORMAT_D32_FLOAT;
    clear.DepthStencil.Depth = 1.0f;

    ThrowIfFailed(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear, IID_PPV_ARGS(&texture_)),
        "shadow map array");

    // ---- по DSV на каждый слой ----
    D3D12_DESCRIPTOR_HEAP_DESC dhd = {};
    dhd.NumDescriptors = static_cast<UINT>(cascadeCount_);
    dhd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    ThrowIfFailed(device->CreateDescriptorHeap(&dhd, IID_PPV_ARGS(&dsvHeap_)),
        "shadow dsv heap");

    const UINT dsvStep = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    D3D12_CPU_DESCRIPTOR_HANDLE dsvStart =
        dsvHeap_->GetCPUDescriptorHandleForHeapStart();

    for (int i = 0; i < cascadeCount_; ++i)
    {
        D3D12_DEPTH_STENCIL_VIEW_DESC dv = {};
        dv.Format = DXGI_FORMAT_D32_FLOAT;
        dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dv.Texture2DArray.MipSlice = 0;
        dv.Texture2DArray.FirstArraySlice = static_cast<UINT>(i);
        dv.Texture2DArray.ArraySize = 1;

        dsv_[i] = dsvStart;
        dsv_[i].ptr += static_cast<SIZE_T>(i) * dsvStep;
        device->CreateDepthStencilView(texture_.Get(), &dv, dsv_[i]);
    }

    // ---- SRV на весь массив ----
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = DXGI_FORMAT_R32_FLOAT;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2DArray.MipLevels = 1;
    sv.Texture2DArray.ArraySize = static_cast<UINT>(cascadeCount_);

    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = srvHeap->GetCPUDescriptorHandleForHeapStart();
    srvCpu.ptr += static_cast<SIZE_T>(srvSlot) * srvDescriptorSize;
    device->CreateShaderResourceView(texture_.Get(), &sv, srvCpu);

    srvGpu_ = srvHeap->GetGPUDescriptorHandleForHeapStart();
    srvGpu_.ptr += static_cast<UINT64>(srvSlot) * srvDescriptorSize;
}

// ============================================================
//  Матрицы света для каждого каскада
//
//  Объём каскада описывается ограничивающей СФЕРОЙ подфрустума,
//  а не его углами. Сфера не меняет радиус при вращении камеры,
//  поэтому масштаб ортопроекции остаётся постоянным и тени не
//  мерцают при повороте. Плюс привязка центра к сетке текселей —
//  без неё тени дрожат при перемещении.
// ============================================================
void CascadedShadowMap::Update(const XMMATRIX& camView, float fovY, float aspect,
    float nearZ, float farZ, const XMFLOAT3& lightDirIn, float lambda)
{
    float splits[MAX_CASCADES] = {};
    ComputeCascadeSplits(nearZ, farZ, cascadeCount_, lambda, splits);

    splitDepths_ = { 0,0,0,0 };
    float* sd = &splitDepths_.x;
    for (int i = 0; i < cascadeCount_; ++i) sd[i] = splits[i];
    // Незаполненные слоты уводим за дальнюю плоскость, чтобы выбор
    // каскада в шейдере не проваливался в пустой слой.
    for (int i = cascadeCount_; i < MAX_CASCADES; ++i) sd[i] = farZ;

    XMVECTOR lightDir = XMVector3Normalize(XMLoadFloat3(&lightDirIn));
    // Вырожденное направление сломало бы LookAt
    if (XMVectorGetX(XMVector3LengthSq(lightDir)) < 1e-6f)
        lightDir = XMVectorSet(0.0f, -1.0f, 0.0f, 0.0f);

    const XMMATRIX invView = XMMatrixInverse(nullptr, camView);
    const float tanHalfV = tanf(fovY * 0.5f);
    const float tanHalfH = tanHalfV * aspect;

    float prevSplit = nearZ;
    for (int c = 0; c < cascadeCount_; ++c)
    {
        const float curSplit = splits[c];

        // Восемь углов подфрустума во view-space, затем в мир
        XMVECTOR corners[8];
        int k = 0;
        for (int zi = 0; zi < 2; ++zi)
        {
            const float z = (zi == 0) ? prevSplit : curSplit;
            const float x = tanHalfH * z;
            const float y = tanHalfV * z;
            corners[k++] = XMVectorSet(-x, -y, z, 1.0f);
            corners[k++] = XMVectorSet(x, -y, z, 1.0f);
            corners[k++] = XMVectorSet(-x, y, z, 1.0f);
            corners[k++] = XMVectorSet(x, y, z, 1.0f);
        }
        for (int i = 0; i < 8; ++i)
            corners[i] = XMVector3TransformCoord(corners[i], invView);

        // Центр и радиус описанной сферы
        XMVECTOR center = XMVectorZero();
        for (int i = 0; i < 8; ++i) center = XMVectorAdd(center, corners[i]);
        center = XMVectorScale(center, 1.0f / 8.0f);

        float radius = 0.0f;
        for (int i = 0; i < 8; ++i)
        {
            const float d = XMVectorGetX(XMVector3Length(XMVectorSubtract(corners[i], center)));
            if (d > radius) radius = d;
        }
        // Округление вверх убирает дрожание радиуса от накопления ошибок
        radius = ceilf(radius * 16.0f) / 16.0f;
        if (radius < 1e-3f) radius = 1e-3f;

        // Камеру света нужно отодвинуть так, чтобы в объём попали ВСЕ
        // потенциальные загораживающие объекты, а не только сам каскад.
        // Раньше здесь стояло radius*2: для ближнего каскада это 4-6 единиц
        // над центром, а колонны и стены Sponza уходят вверх на 15-20.
        // Всё, что выше камеры света, отсекалось ближней плоскостью
        // (DepthClipEnable), и в карту глубины попадали случайные внутренние
        // поверхности — отсюда размазанные тени во весь экран.
        const float backOff = radius + casterDistance_;
        const XMVECTOR eye = XMVectorSubtract(center, XMVectorScale(lightDir, backOff));

        // up не должен быть коллинеарен направлению света
        const float dotUp = fabsf(XMVectorGetY(lightDir));
        const XMVECTOR up = (dotUp > 0.99f)
            ? XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f)
            : XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

        XMMATRIX lightView = XMMatrixLookAtLH(eye, center, up);
        XMMATRIX lightProj = XMMatrixOrthographicOffCenterLH(
            -radius, radius, -radius, radius, 0.0f, backOff + radius);

        // ---- привязка к сетке текселей ----
        // Без неё центр каскада плавно едет вместе с камерой, тени
        // переливаются по краям. Смещаем начало координат света так,
        // чтобы центр попадал ровно в тексель.
        XMMATRIX lightVP = XMMatrixMultiply(lightView, lightProj);
        XMVECTOR originShadow = XMVector3TransformCoord(XMVectorZero(), lightVP);
        const float texelScale = static_cast<float>(resolution_) * 0.5f;
        originShadow = XMVectorScale(originShadow, texelScale);

        XMVECTOR rounded = XMVectorRound(originShadow);
        XMVECTOR offset = XMVectorSubtract(rounded, originShadow);
        offset = XMVectorScale(offset, 1.0f / texelScale);

        XMMATRIX roundMat = XMMatrixTranslation(
            XMVectorGetX(offset), XMVectorGetY(offset), 0.0f);
        lightVP = XMMatrixMultiply(lightVP, roundMat);

        XMStoreFloat4x4(&viewProj_[c], lightVP);
        prevSplit = curSplit;
    }

    // Оставшиеся слоты заполняем последней матрицей, чтобы в них
    // не оказалось нулей при выборе каскада из шейдера
    for (int c = cascadeCount_; c < MAX_CASCADES; ++c)
        viewProj_[c] = viewProj_[cascadeCount_ - 1];
}

// ============================================================
void CascadedShadowMap::BeginCascade(ID3D12GraphicsCommandList* cl, int cascade)
{
    if (!texture_ || cascade < 0 || cascade >= cascadeCount_) return;

    if (!inDepthWrite_)
    {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = texture_.Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        cl->ResourceBarrier(1, &b);
        inDepthWrite_ = true;
    }

    D3D12_VIEWPORT vp = { 0.0f, 0.0f,
        static_cast<float>(resolution_), static_cast<float>(resolution_), 0.0f, 1.0f };
    D3D12_RECT sc = { 0, 0,
        static_cast<LONG>(resolution_), static_cast<LONG>(resolution_) };
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sc);

    cl->ClearDepthStencilView(dsv_[cascade], D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    // Цветовых целей нет: пишем только глубину
    cl->OMSetRenderTargets(0, nullptr, FALSE, &dsv_[cascade]);
}

void CascadedShadowMap::EndPass(ID3D12GraphicsCommandList* cl)
{
    if (!texture_ || !inDepthWrite_) return;
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = texture_.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    cl->ResourceBarrier(1, &b);
    inDepthWrite_ = false;
}
