#include "Game.h"
#include "Utils.h"
#include <cmath>
#include <algorithm>
#include <limits>

static constexpr float PI = 3.14159265f;

// ============================================================
//  Вспомогательные inline-функции
// ============================================================
static inline XMFLOAT3 F3Add(XMFLOAT3 a, XMFLOAT3 b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
static inline XMFLOAT3 F3Sub(XMFLOAT3 a, XMFLOAT3 b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
static inline XMFLOAT3 F3Scale(XMFLOAT3 a, float s)  { return {a.x*s, a.y*s, a.z*s}; }
static inline float    F3Dot(XMFLOAT3 a, XMFLOAT3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static inline XMFLOAT3 F3Cross(XMFLOAT3 a, XMFLOAT3 b)
{
    return { a.y*b.z - a.z*b.y,
             a.z*b.x - a.x*b.z,
             a.x*b.y - a.y*b.x };
}
static inline float F3Len(XMFLOAT3 a) { return sqrtf(F3Dot(a,a)); }
static inline XMFLOAT3 F3Norm(XMFLOAT3 a)
{
    float l = F3Len(a);
    if (l < 1e-6f) return {0,0,1};
    return F3Scale(a, 1.f/l);
}

// Применяет матрицу World к точке (float3 → float3)
static inline XMFLOAT3 TransformPoint(XMFLOAT3 p, XMMATRIX m)
{
    XMVECTOR v = XMVector3TransformCoord(XMLoadFloat3(&p), m);
    XMFLOAT3 out;
    XMStoreFloat3(&out, v);
    return out;
}

// ============================================================

Game::Game(HWND hwnd, int width, int height)
    : hwnd_(hwnd), width_(width), height_(height)
{
    rs_ = std::make_unique<RenderingSystem>(hwnd, width, height);
    worldMatrix_ = XMMatrixIdentity();
}

bool Game::Initialize()
{
    try
    {
        rs_->Initialize();
        rs_->BeginResourceUpload();
        LoadSceneGeometry();
        LoadSceneTextures();
        rs_->EndResourceUpload();
        rs_->ReleaseTextureUploadBuffers();
        rs_->CreateConstantBuffers();
        return true;
    }
    catch (...)
    {
        MessageBoxW(hwnd_, L"Initialize failed", L"Error", MB_OK | MB_ICONERROR);
        return false;
    }
}
void Game::ClearStuckLights()
{
    stuckLights_.clear();

    // Также деактивируем все летящие снаряды
    for (auto& b : bullets_)
    {
        if (b.Active)
            b.Active = false;
    }

    // Удаляем неактивные снаряды
    bullets_.erase(
        std::remove_if(bullets_.begin(), bullets_.end(),
            [](const LightBullet& b) { return !b.Active && !b.Stuck; }),
        bullets_.end());
}
void Game::SetMeshForRaycast(const std::vector<Vertex>& verts, const std::vector<UINT>& idxs)
{
    meshVerts_ = verts;
    meshIdxs_  = idxs;
}

void Game::LoadSceneGeometry()
{
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dir(exePath);
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);

    ObjResult obj = LoadObj(dir + L"sponza.obj");
    if (obj.valid)
    {
        rs_->BuildBuffers(obj.vertices, obj.indices);
        SetMeshForRaycast(obj.vertices, obj.indices);
        return;
    }

    // Fallback: куб
    std::vector<Vertex> verts;
    std::vector<UINT>   idxs;
    auto addFace = [&](XMFLOAT3 p0, XMFLOAT3 p1, XMFLOAT3 p2, XMFLOAT3 p3, XMFLOAT3 n)
    {
        UINT base = static_cast<UINT>(verts.size());
        verts.push_back({ p0, n, {1,1,1,1}, {0,0} });
        verts.push_back({ p1, n, {1,1,1,1}, {1,0} });
        verts.push_back({ p2, n, {1,1,1,1}, {1,1} });
        verts.push_back({ p3, n, {1,1,1,1}, {0,1} });
        idxs.insert(idxs.end(), { base,base+1,base+2, base,base+2,base+3 });
    };
    addFace({ -1,-1,-1 }, { 1,-1,-1 }, { 1,1,-1 }, { -1,1,-1 }, { 0,0,-1 });
    addFace({ -1,-1, 1 }, { 1,-1, 1 }, { 1,1, 1 }, { -1,1, 1 }, { 0,0, 1 });
    addFace({ -1,-1,-1 }, { -1,-1,1 }, { -1,1,1 }, { -1,1,-1 }, { -1,0,0 });
    addFace({  1,-1,-1 }, {  1,-1,1 }, {  1,1,1 }, {  1,1,-1 }, {  1,0,0 });
    addFace({ -1,-1,-1 }, {  1,-1,-1 }, { 1,-1,1 }, { -1,-1,1 }, { 0,-1,0 });
    addFace({ -1, 1,-1 }, {  1, 1,-1 }, { 1, 1,1 }, { -1, 1,1 }, { 0, 1,0 });
    rs_->BuildBuffers(verts, idxs);
    SetMeshForRaycast(verts, idxs);
}

void Game::LoadSceneTextures()
{
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dir(exePath);
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);

    TextureData td1 = LoadTextureWIC(dir + L"texture1.png");
    if (!td1.valid) td1 = LoadTextureWIC(dir + L"texture1.jpg");
    if (!td1.valid)
    {
        MessageBoxW(hwnd_,
            (L"Не найден файл текстуры!\nОжидается:\n" + dir + L"texture1.png").c_str(),
            L"Texture Not Found", MB_OK | MB_ICONERROR);
        ThrowIfFailed(E_FAIL, "texture1 not found");
    }
    rs_->UploadTexture(td1, 0);

    TextureData td2 = LoadTextureWIC(dir + L"texture2.png");
    if (!td2.valid) td2 = LoadTextureWIC(dir + L"texture2.jpg");
    if (!td2.valid)
    {
        MessageBoxW(hwnd_,
            (L"Не найден файл текстуры!\nОжидается:\n" + dir + L"texture2.png").c_str(),
            L"Texture Not Found", MB_OK | MB_ICONERROR);
        ThrowIfFailed(E_FAIL, "texture2 not found");
    }
    rs_->UploadTexture(td2, 1);
}

// ============================================================
//  Рейкаст Мёллера–Трумбора (против одного треугольника)
// ============================================================
bool Game::RayTriangle(XMFLOAT3 orig, XMFLOAT3 dir,
                        XMFLOAT3 v0, XMFLOAT3 v1, XMFLOAT3 v2,
                        float& t)
{
    const float EPS = 1e-7f;
    XMFLOAT3 e1 = F3Sub(v1, v0);
    XMFLOAT3 e2 = F3Sub(v2, v0);
    XMFLOAT3 h  = F3Cross(dir, e2);
    float    a  = F3Dot(e1, h);
    if (a > -EPS && a < EPS) return false; // луч параллелен
    float    f  = 1.f / a;
    XMFLOAT3 s  = F3Sub(orig, v0);
    float    u  = f * F3Dot(s, h);
    if (u < 0.f || u > 1.f) return false;
    XMFLOAT3 q  = F3Cross(s, e1);
    float    v  = f * F3Dot(dir, q);
    if (v < 0.f || u + v > 1.f) return false;
    t = f * F3Dot(e2, q);
    return (t > EPS);
}

// ============================================================
//  Рейкаст по всему мешу (в world-space через worldMatrix_)
// ============================================================
bool Game::RaycastMesh(XMFLOAT3 origin, XMFLOAT3 dir,
                        float maxDist, float& outT) const
{
    if (meshIdxs_.size() < 3) return false;

    outT = maxDist;
    bool hit = false;

    // Приводим луч в object-space (инвертируем мировую матрицу)
    XMMATRIX invWorld = XMMatrixInverse(nullptr, worldMatrix_);
    XMVECTOR origV = XMVector3TransformCoord(XMLoadFloat3(&origin), invWorld);
    XMVECTOR dirV  = XMVector3TransformNormal(XMLoadFloat3(&dir),   invWorld);
    dirV = XMVector3Normalize(dirV);

    XMFLOAT3 origOS, dirOS;
    XMStoreFloat3(&origOS, origV);
    XMStoreFloat3(&dirOS,  dirV);

    const size_t triCount = meshIdxs_.size() / 3;
    for (size_t i = 0; i < triCount; ++i)
    {
        UINT i0 = meshIdxs_[i*3+0];
        UINT i1 = meshIdxs_[i*3+1];
        UINT i2 = meshIdxs_[i*3+2];
        float t = 0.f;
        if (RayTriangle(origOS, dirOS,
                        meshVerts_[i0].Position,
                        meshVerts_[i1].Position,
                        meshVerts_[i2].Position, t))
        {
            if (t < outT)
            {
                outT = t;
                hit  = true;
            }
        }
    }
    return hit;
}

// ============================================================
//  Выстрел: создаём снаряд из позиции камеры
// ============================================================
void Game::OnShoot()
{
    // Лимит снарядов в воздухе
    int activeCount = 0;
    for (auto& b : bullets_)
        if (b.Active) ++activeCount;
    if (activeCount >= MAX_BULLETS) return;

    // Случайный красивый цвет: выбираем один из 6 насыщенных оттенков
    static int colorIdx = 0;
    static const XMFLOAT3 kColors[] = {
        {1.0f, 0.3f, 0.05f},   // оранжево-красный
        {0.1f, 0.5f, 1.0f},    // синий
        {0.05f, 1.0f, 0.3f},   // зелёный
        {1.0f, 0.9f, 0.1f},    // жёлтый
        {0.8f, 0.1f, 1.0f},    // фиолетовый
        {0.05f, 1.0f, 0.9f},   // циан
    };
    XMFLOAT3 col = kColors[colorIdx % 6];
    ++colorIdx;

    LightBullet b = {};
    b.Position  = camPos_;
    b.Direction = camForward_;
    b.Color     = col;
    b.Intensity = 8.0f;
    b.Range     = 7.0f;
    b.Speed     = BULLET_SPEED;
    b.Active    = true;
    b.Stuck     = false;
    bullets_.push_back(b);
}

// ============================================================
//  Обновление снарядов
// ============================================================
void Game::UpdateBullets(float dt)
{
    for (auto& b : bullets_)
    {
        if (!b.Active || b.Stuck) continue;

        // Шаг движения
        float stepLen = b.Speed * dt;
        XMFLOAT3 newPos = F3Add(b.Position, F3Scale(b.Direction, stepLen));

        // CPU рейкаст вдоль шага (origin = старая позиция, длина = stepLen)
        float hitT = 0.f;
        bool  hit  = RaycastMesh(b.Position, b.Direction, stepLen + 0.05f, hitT);

        if (hit && hitT <= stepLen + 0.05f)
        {
            // Прилипаем: финальная позиция = точка удара
            XMFLOAT3 hitPos = F3Add(b.Position, F3Scale(b.Direction, hitT));

            b.Active = false;
            b.Stuck  = true;
            b.StuckPosition[0] = hitPos.x;
            b.StuckPosition[1] = hitPos.y;
            b.StuckPosition[2] = hitPos.z;

            // Если достигнут лимит прилипших — убираем самый старый
            if ((int)stuckLights_.size() >= MAX_STUCK_LIGHTS)
                stuckLights_.erase(stuckLights_.begin());

            StuckLight sl;
            sl.Position  = hitPos;
            sl.Color     = b.Color;
            sl.Intensity = b.Intensity;
            sl.Range     = b.Range;
            stuckLights_.push_back(sl);
        }
        else
        {
            b.Position = newPos;

            // Убиваем снаряд, если улетел слишком далеко
            float dx = b.Position.x - camPos_.x;
            float dy = b.Position.y - camPos_.y;
            float dz = b.Position.z - camPos_.z;
            float dist2 = dx*dx + dy*dy + dz*dz;
            if (dist2 > BULLET_MAX_DIST * BULLET_MAX_DIST)
                b.Active = false;
        }
    }

    // Удаляем мёртвые (не летящие и не прилипшие) снаряды
    bullets_.erase(
        std::remove_if(bullets_.begin(), bullets_.end(),
            [](const LightBullet& b){ return !b.Active && !b.Stuck; }),
        bullets_.end());
}

// ============================================================
//  Update
// ============================================================
void Game::Update(float deltaTime)
{
    rotationAngle_ += 0.5f * deltaTime;
    time_ += deltaTime;
    uvOffsetX_ += 0.4f * deltaTime;
    if (uvOffsetX_ > 1.0f) uvOffsetX_ -= 1.0f;

    cameraAngle_ += 0.18f * deltaTime;
    float camH    = 2.5f + sinf(time_ * 0.3f) * 1.0f;
    float camDist = 5.0f;
    XMVECTOR eye    = XMVectorSet(sinf(cameraAngle_) * camDist, camH, cosf(cameraAngle_) * camDist, 1.f);
    XMVECTOR target = XMVectorSet(0.f, 0.f, 0.f, 1.f);
    XMVECTOR up     = XMVectorSet(0.f, 1.f, 0.f, 0.f);

    XMMATRIX world  = XMMatrixRotationY(rotationAngle_);
    worldMatrix_    = world;   // сохраняем для рейкаста

    XMMATRIX view   = XMMatrixLookAtLH(eye, target, up);
    float    aspect = (height_ > 0) ? float(width_) / float(height_) : 1.f;
    XMMATRIX proj   = XMMatrixPerspectiveFovLH(XMConvertToRadians(55.f), aspect, 0.1f, 200.f);

    // Сохраняем позицию и направление камеры для стрельбы
    XMStoreFloat3(&camPos_, eye);
    XMVECTOR fwd = XMVector3Normalize(XMVectorSubtract(target, eye));
    XMStoreFloat3(&camForward_, fwd);

    float blend = 0.5f + 0.5f * sinf(time_ * 0.6f);

    // Geometry Pass CB
    ConstantBufferData geomCB = {};
    geomCB.World       = XMMatrixTranspose(world);
    geomCB.View        = XMMatrixTranspose(view);
    geomCB.Proj        = XMMatrixTranspose(proj);
    geomCB.CameraPos   = { camPos_.x, camPos_.y, camPos_.z, 1.f };
    geomCB.Tiling      = { 3.f, 3.f };
    geomCB.UVOffset    = { uvOffsetX_, 1.9f };
    geomCB.BlendFactor = blend;
    rs_->UpdateGeometryPassCB(geomCB);

    // ---- Обновление снарядов ----
    UpdateBullets(deltaTime);

    rs_->ClearLights();

    // ---- Прилипшие световые источники (приоритет) ----
    for (auto& sl : stuckLights_)
        rs_->AddPointLight(sl.Position, sl.Color, sl.Intensity, sl.Range);

    // ---- Летящие снаряды — тоже светят (небольшой радиус) ----
    for (auto& b : bullets_)
    {
        if (b.Active && !b.Stuck)
            rs_->AddPointLight(b.Position, b.Color, b.Intensity * 0.7f, 2.5f);
    }

    // ---- Динамические источники сцены ----
    {
        float sunAngle = time_ * 0.15f;
        float sx = sinf(sunAngle);
        float sy = -0.6f - 0.3f * cosf(time_ * 0.07f);
        float sz = cosf(sunAngle);
        rs_->AddDirectionalLight({ sx, sy, sz }, { 1.f, 0.92f, 0.75f }, 1.4f);
    }
    {
        float moonAngle = time_ * 0.15f + PI;
        rs_->AddDirectionalLight(
            { sinf(moonAngle), -0.4f, cosf(moonAngle) },
            { 0.35f, 0.4f, 0.65f }, 0.3f);
    }
    {
        float a = time_ * 0.9f, r = 4.5f;
        rs_->AddPointLight({ cosf(a)*r, 1.f, sinf(a)*r }, { 1.f, 0.15f, 0.05f }, 6.f, 9.f);
    }
    {
        float a = time_ * 0.7f + 2.f*PI/3.f, r = 3.8f;
        rs_->AddPointLight({ cosf(a)*r, 3.f+sinf(time_*0.5f)*1.5f, sinf(a)*r },
                           { 0.1f, 0.4f, 1.f }, 5.f, 10.f);
    }
    {
        float a = time_ * 1.1f + 4.f*PI/3.f, r = 3.2f;
        float y = 1.5f + cosf(a*0.8f)*2.f;
        rs_->AddPointLight({ cosf(a)*r, y, sinf(a)*r }, { 0.05f, 1.f, 0.3f }, 4.5f, 8.f);
    }
    {
        float sweepAngle = time_ * 0.5f, sweepR = 0.6f;
        XMFLOAT3 spotPos = { -5.f, 6.f, -5.f };
        XMFLOAT3 spotDir = { sinf(sweepAngle)*sweepR, -1.f, cosf(sweepAngle)*sweepR };
        float dlen = sqrtf(spotDir.x*spotDir.x + spotDir.y*spotDir.y + spotDir.z*spotDir.z);
        spotDir = { spotDir.x/dlen, spotDir.y/dlen, spotDir.z/dlen };
        rs_->AddSpotLight(spotPos, spotDir, { 1.f, 0.85f, 0.5f }, 8.f, 14.f, 12.f, 25.f);
    }
    {
        float sweepAngle = -time_*0.7f + PI, sweepR = 0.5f;
        XMFLOAT3 spotPos = { 5.f, 5.f, 5.f };
        XMFLOAT3 spotDir = { sinf(sweepAngle)*sweepR-0.4f, -1.f, cosf(sweepAngle)*sweepR-0.4f };
        float dlen = sqrtf(spotDir.x*spotDir.x + spotDir.y*spotDir.y + spotDir.z*spotDir.z);
        spotDir = { spotDir.x/dlen, spotDir.y/dlen, spotDir.z/dlen };
        rs_->AddSpotLight(spotPos, spotDir, { 0.8f, 0.1f, 1.f }, 7.f, 12.f, 10.f, 22.f);
    }
    {
        float pulse = 0.6f + 0.4f*fabsf(sinf(time_*2.5f));
        XMFLOAT3 spotPos = { 0.f, -3.f, 6.f };
        XMFLOAT3 spotDir = { 0.f, 0.6f, -1.f };
        float dlen = sqrtf(spotDir.x*spotDir.x + spotDir.y*spotDir.y + spotDir.z*spotDir.z);
        spotDir = { spotDir.x/dlen, spotDir.y/dlen, spotDir.z/dlen };
        rs_->AddSpotLight(spotPos, spotDir, { 1.f, 1.f, 1.f }, 5.f*pulse, 11.f, 8.f, 20.f);
    }

    LightingPassCB lCB = rs_->GetLightingCB();
    lCB.CameraPos = { camPos_.x, camPos_.y, camPos_.z, 1.f };
    rs_->UpdateLightingPassCB(lCB);


}

void Game::Render()
{
    rs_->BeginFrame();
    rs_->BindGeometryPass();
    rs_->DrawMesh();

    rs_->BeginLightingPass();
    rs_->DrawLightingQuad();

    rs_->EndFrame();
}

void Game::Resize(int width, int height)
{
    if (width <= 0 || height <= 0) return;
    width_  = width;
    height_ = height;
    rs_->Resize(width, height);
}
