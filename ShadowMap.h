#pragma once
#include <d3d12.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <vector>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

// ============================================================
//  Каскадные карты теней (ДЗ №5)
//
//  Одна текстура Texture2DArray формата D32_FLOAT, по слою на каскад.
//  На каждый слой свой DSV, на весь массив — один SRV.
//
//  Разбиение на каскады нелинейное (practical split scheme из PSSM):
//      d_i = lambda * n*(f/n)^(i/N) + (1-lambda) * (n + (f-n)*i/N)
//  Логарифмическая часть даёт нужную плотность вблизи камеры,
//  равномерная не даёт первому каскаду выродиться в почти нулевой
//  диапазон. lambda задаёт баланс.
// ============================================================
class CascadedShadowMap
{
public:
    static const int MAX_CASCADES = 4;

    void Create(ID3D12Device* device,
        ID3D12DescriptorHeap* srvHeap, UINT srvSlot, UINT srvDescriptorSize,
        UINT resolution = 2048, int cascadeCount = MAX_CASCADES);

    // Пересчитать матрицы света под текущее положение камеры
    void Update(const XMMATRIX& camView, float fovY, float aspect,
        float nearZ, float farZ, const XMFLOAT3& lightDir, float lambda = 0.85f);

    // Начать запись в каскад: барьер, viewport, очистка, привязка DSV
    void BeginCascade(ID3D12GraphicsCommandList* cl, int cascade);
    // Перевести массив в состояние чтения шейдером
    void EndPass(ID3D12GraphicsCommandList* cl);

    int   CascadeCount()  const { return cascadeCount_; }
    UINT  Resolution()    const { return resolution_; }
    const XMFLOAT4X4& ViewProj(int i) const { return viewProj_[i]; }
    // Границы каскадов в view-space глубине, для выбора каскада в шейдере
    const XMFLOAT4& SplitDepths() const { return splitDepths_; }
    D3D12_GPU_DESCRIPTOR_HANDLE SrvGpu() const { return srvGpu_; }
    bool  Ready() const { return texture_ != nullptr; }

    // На сколько отодвигать камеру света за пределы каскада. Должно
    // перекрывать высоту сцены, иначе высокие объекты не попадут в объём
    // и перестанут отбрасывать тень. Для Sponza хватает 60 единиц.
    void  SetCasterDistance(float d) { casterDistance_ = (d > 1.0f) ? d : 1.0f; }

private:
    ComPtr<ID3D12Resource>       texture_;
    ComPtr<ID3D12DescriptorHeap> dsvHeap_;      // свой DSV-хип: по одному на слой
    D3D12_CPU_DESCRIPTOR_HANDLE  dsv_[MAX_CASCADES] = {};
    D3D12_GPU_DESCRIPTOR_HANDLE  srvGpu_ = {};

    XMFLOAT4X4 viewProj_[MAX_CASCADES] = {};
    XMFLOAT4   splitDepths_ = { 0,0,0,0 };

    UINT resolution_ = 2048;
    int  cascadeCount_ = MAX_CASCADES;
    bool inDepthWrite_ = false;
    float casterDistance_ = 60.0f;
};

// Выделено отдельно, чтобы можно было проверить без D3D.
// Заполняет outSplits[0..count-1] дальними границами каскадов.
void ComputeCascadeSplits(float nearZ, float farZ, int count, float lambda,
    float* outSplits);
