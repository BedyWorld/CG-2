#include "Game.h"
#include "Utils.h"
#include <cmath>
#include <algorithm>
#include <limits>
#include <unordered_map>

static constexpr float PI = 3.14159265f;

static inline XMFLOAT3 F3Add(XMFLOAT3 a, XMFLOAT3 b) { return { a.x + b.x,a.y + b.y,a.z + b.z }; }
static inline XMFLOAT3 F3Sub(XMFLOAT3 a, XMFLOAT3 b) { return { a.x - b.x,a.y - b.y,a.z - b.z }; }
static inline XMFLOAT3 F3Scale(XMFLOAT3 a, float s) { return { a.x * s,a.y * s,a.z * s }; }
static inline float    F3Dot(XMFLOAT3 a, XMFLOAT3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline XMFLOAT3 F3Cross(XMFLOAT3 a, XMFLOAT3 b) { return { a.y * b.z - a.z * b.y,a.z * b.x - a.x * b.z,a.x * b.y - a.y * b.x }; }
static inline float    F3Len(XMFLOAT3 a) { return sqrtf(F3Dot(a, a)); }
static inline XMFLOAT3 F3Norm(XMFLOAT3 a) { float l = F3Len(a); return l < 1e-6f ? XMFLOAT3{ 0,0,1 } : F3Scale(a, 1.f / l); }
static inline float    saturate01(float x) { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }

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
        LoadSceneGeometry();   // Sponza: меш + per-material текстуры
        LoadSceneGeometry2();  // вторая модель
        LoadSceneTextures2();  // текстуры второй модели
        rs_->EndResourceUpload();
        rs_->ReleaseTextureUploadBuffers();
        rs_->CreateConstantBuffers();
        return true;
    }
    catch (const std::exception& e)
    {
        MessageBoxA(hwnd_, e.what(), "Initialize failed", MB_OK | MB_ICONERROR);
        return false;
    }
    catch (...)
    {
        MessageBoxW(hwnd_, L"Initialize failed", L"Error", MB_OK | MB_ICONERROR);
        return false;
    }
}

// ============================================================
//  Мышь / ввод
// ============================================================
void Game::CaptureMouse()
{
    if (mouseCaptured_) return;
    mouseCaptured_ = true;
    ShowCursor(FALSE);
    SetCapture(hwnd_);
    RECT rc; GetClientRect(hwnd_, &rc);
    POINT center = { (rc.right - rc.left) / 2,(rc.bottom - rc.top) / 2 };
    ClientToScreen(hwnd_, &center);
    centerX_ = center.x; centerY_ = center.y;
    SetCursorPos(centerX_, centerY_);
}

void Game::ReleaseMouse()
{
    if (!mouseCaptured_) return;
    mouseCaptured_ = false; ShowCursor(TRUE); ReleaseCapture();
}

void Game::OnMouseMove(int screenX, int screenY)
{
    if (!mouseCaptured_) return;
    int dx = screenX - centerX_, dy = screenY - centerY_;
    if (dx == 0 && dy == 0) return;
    const float sensitivity = 0.0015f;
    yaw_ += dx * sensitivity; pitch_ += dy * sensitivity;
    const float pitchLimit = PI * 0.499f;
    pitch_ = max(-pitchLimit, min(pitchLimit, pitch_));
    SetCursorPos(centerX_, centerY_);
}

void Game::OnMouseDown(int button)
{
    if (button == 0) { if (!mouseCaptured_) CaptureMouse(); else OnShoot(); }
}

void Game::OnKeyDown(int vkey)
{
    if (vkey == VK_ESCAPE) { ReleaseMouse(); return; }
    if (vkey == 'R') { ClearStuckLights(); return; }
    if (vkey == VK_OEM_PLUS || vkey == VK_ADD)       dispScale_ = min(dispScale_ + 0.05f, 2.0f);
    if (vkey == VK_OEM_MINUS || vkey == VK_SUBTRACT) dispScale_ = max(dispScale_ - 0.05f, 0.0f);
}

void Game::ClearStuckLights()
{
    stuckLights_.clear();
    for (auto& b : bullets_) b.Active = false;
    bullets_.erase(std::remove_if(bullets_.begin(), bullets_.end(),
        [](const LightBullet& b) {return !b.Active && !b.Stuck;}), bullets_.end());
}

void Game::SetMeshForRaycast(const std::vector<Vertex>& verts, const std::vector<UINT>& idxs)
{
    meshVerts_ = verts; meshIdxs_ = idxs;
}

// ============================================================
//  LoadSceneGeometry — загружает Sponza с per-material текстурами
//  MTL Blender-экспорта не содержит map_Kd, поэтому используем
//  hardcoded таблицу соответствий имён материалов -> TGA файлам.
// ============================================================
void Game::LoadSceneGeometry()
{
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dir(exePath);
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);

    ObjResult obj = LoadObj(dir + L"sponza.obj");

    if (obj.valid && !obj.subMeshes.empty())
    {
        // ---- Таблица: basename материала -> { diffuse TGA, normal DDN TGA } ----
        struct MatEntry { const wchar_t* diff; const wchar_t* norm; };
        static const std::unordered_map<std::wstring, MatEntry> kMatMap = {
            { L"arch",          { L"sponza_arch_diff.tga",          L"sponza_arch_ddn.tga"        } },
            { L"bricks",        { L"spnza_bricks_a_diff.tga",       L"spnza_bricks_a_ddn.tga"     } },
            { L"ceiling",       { L"sponza_ceiling_a_diff.tga",     L"sponza_ceiling_a_ddn.tga"   } },
            { L"chain",         { L"chain_texture.tga",             L"chain_texture_ddn.tga"       } },
            { L"column_a",      { L"sponza_column_a_diff.tga",      L"sponza_column_a_ddn.tga"    } },
            { L"column_b",      { L"sponza_column_c_diff.tga",      L"sponza_column_b_ddn.tga"    } },
            { L"column_c",      { L"sponza_column_c_diff.tga",      L"sponza_column_c_ddn.tga"    } },
            { L"details",       { L"sponza_details_diff.tga",       L"sponza_details_ddn.tga"     } },
            { L"fabric_a",      { L"sponza_curtain_diff.tga",       L"sponza_curtain_ddn.tga"     } },
            { L"fabric_c",      { L"sponza_curtain_blue_diff.tga",  L"sponza_curtain_ddn.tga"     } },
            { L"fabric_d",      { L"sponza_curtain_diff.tga",       L"sponza_curtain_ddn.tga"     } },
            { L"fabric_e",      { L"sponza_curtain_green_diff.tga", L"sponza_curtain_ddn.tga"     } },
            { L"fabric_f",      { L"sponza_fabric_diff.tga",        L"sponza_fabric_ddn.tga"      } },
            { L"fabric_g",      { L"sponza_fabric_green_diff.tga",  L"sponza_fabric_ddn.tga"      } },
            { L"flagpole",      { L"sponza_flagpole_diff.tga",      L"sponza_flagpole_ddn.tga"    } },
            { L"floor",         { L"sponza_floor_a_diff.tga",       L"sponza_floor_a_ddn.tga"     } },
            { L"leaf",          { L"sponza_thorn_diff.tga",         L"sponza_thorn_ddn.tga"       } },
            { L"roof",          { L"sponza_roof_diff.tga",          L"sponza_roof_ddn.tga"        } },
            { L"vase",          { L"vase_dif.tga",                  L"vase_ddn.tga"               } },
            { L"vase_hanging",  { L"vase_hanging.tga",              L"vase_hanging_ddn.tga"       } },
            { L"vase_round",    { L"vase_round.tga",                L"vase_round_ddn.tga"         } },
            { L"Material__25",  { L"background.tga",                L"background_ddn.tga"         } },
            { L"Material__298", { L"background.tga",                L"background_ddn.tga"         } },
            { L"Material__47",  { L"lion.tga",                      L"lion_ddn.tga"               } },
            { L"Material__57",  { L"lion.tga",                      L"lion2_ddn.tga"              } },
        };

        // Убирает суффикс ".001", ".002" и т.п. из имени материала
        auto StripSuffix = [](const std::string& name) -> std::wstring
            {
                std::wstring w(name.begin(), name.end());
                auto dot = w.rfind(L'.');
                if (dot != std::wstring::npos)
                {
                    std::wstring suffix = w.substr(dot + 1);
                    bool allDigits = !suffix.empty();
                    for (auto c : suffix) if (!iswdigit(c)) { allDigits = false; break; }
                    if (allDigits) w = w.substr(0, dot);
                }
                return w;
            };

        std::vector<UINT>         starts, counts;
        std::vector<std::wstring> diffPaths, normPaths;

        for (auto& sm : obj.subMeshes)
        {
            starts.push_back(sm.indexStart);
            counts.push_back(sm.indexCount);

            std::wstring key = StripSuffix(sm.materialName);
            auto it = kMatMap.find(key);
            if (it != kMatMap.end())
            {
                diffPaths.push_back(dir + it->second.diff);
                normPaths.push_back(dir + it->second.norm);
            }
            else
            {
                diffPaths.push_back(L"");
                normPaths.push_back(L"");
            }
        }

        rs_->BuildMesh1(obj.vertices, obj.indices, starts, counts, diffPaths, normPaths);
        SetMeshForRaycast(obj.vertices, obj.indices);
        return;
    }

    // ---- Fallback: куб ----
    std::vector<Vertex> verts;
    std::vector<UINT>   idxs;
    auto addFace = [&](XMFLOAT3 p0, XMFLOAT3 p1, XMFLOAT3 p2, XMFLOAT3 p3, XMFLOAT3 n)
        {
            UINT base = static_cast<UINT>(verts.size());
            verts.push_back({ p0,n,{1,1,1,1},{0,0} });
            verts.push_back({ p1,n,{1,1,1,1},{1,0} });
            verts.push_back({ p2,n,{1,1,1,1},{1,1} });
            verts.push_back({ p3,n,{1,1,1,1},{0,1} });
            idxs.insert(idxs.end(), { base,base + 1,base + 2,base,base + 2,base + 3 });
        };
    addFace({ -1,-1,-1 }, { 1,-1,-1 }, { 1,1,-1 }, { -1,1,-1 }, { 0,0,-1 });
    addFace({ -1,-1, 1 }, { 1,-1, 1 }, { 1,1, 1 }, { -1,1, 1 }, { 0,0, 1 });
    addFace({ -1,-1,-1 }, { -1,-1,1 }, { -1,1,1 }, { -1,1,-1 }, { -1,0,0 });
    addFace({ 1,-1,-1 }, { 1,-1,1 }, { 1,1,1 }, { 1,1,-1 }, { 1,0,0 });
    addFace({ -1,-1,-1 }, { 1,-1,-1 }, { 1,-1,1 }, { -1,-1,1 }, { 0,-1,0 });
    addFace({ -1, 1,-1 }, { 1, 1,-1 }, { 1, 1,1 }, { -1, 1,1 }, { 0, 1,0 });

    std::vector<UINT> s = { 0 }, c = { static_cast<UINT>(idxs.size()) };
    std::vector<std::wstring> d = { L"" }, nm = { L"" };
    rs_->BuildMesh1(verts, idxs, s, c, d, nm);
    SetMeshForRaycast(verts, idxs);
}

// ============================================================
//  LoadSceneGeometry2 — вторая модель (model2.obj / сфера)
// ============================================================
void Game::LoadSceneGeometry2()
{
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dir(exePath);
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);

    ObjResult obj = LoadObj(dir + L"model2.obj");
    if (obj.valid) { rs_->BuildBuffers2(obj.vertices, obj.indices); return; }

    // Fallback: UV-сфера
    const int SLICES = 32, STACKS = 16;
    std::vector<Vertex> verts;
    std::vector<UINT>   idxs;
    for (int st = 0;st <= STACKS;++st)
    {
        float phi = PI * float(st) / float(STACKS);
        for (int sl = 0;sl <= SLICES;++sl)
        {
            float theta = 2.0f * PI * float(sl) / float(SLICES);
            float x = sinf(phi) * cosf(theta), y = cosf(phi), z = sinf(phi) * sinf(theta);
            verts.push_back({ {x,y,z},{x,y,z},{1,1,1,1},{float(sl) / SLICES,float(st) / STACKS} });
        }
    }
    for (int st = 0;st < STACKS;++st)
        for (int sl = 0;sl < SLICES;++sl)
        {
            UINT a = st * (SLICES + 1) + sl, b = (st + 1) * (SLICES + 1) + sl;
            UINT c = (st + 1) * (SLICES + 1) + sl + 1, d = st * (SLICES + 1) + sl + 1;
            idxs.insert(idxs.end(), { a,b,c,a,c,d });
        }
    rs_->BuildBuffers2(verts, idxs);
}

// ============================================================
//  LoadSceneTextures2 — текстуры второй модели
// ============================================================
void Game::LoadSceneTextures2()
{
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dir(exePath);
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);

    // albedo1
    TextureData td1 = LoadTextureAuto(dir + L"model2_albedo.png");
    if (!td1.valid) td1 = LoadTextureAuto(dir + L"model2_albedo.jpg");
    if (!td1.valid) td1 = CreateSolidColor(200, 200, 200);
    rs_->UploadTextureMesh2(td1, 0);

    // albedo2
    TextureData td2 = LoadTextureAuto(dir + L"model2_albedo2.png");
    if (!td2.valid) td2 = LoadTextureAuto(dir + L"model2_albedo2.jpg");
    if (!td2.valid) td2 = CreateSolidColor(210, 190, 170);
    rs_->UploadTextureMesh2(td2, 1);

    // normal
    TextureData tdN = LoadTextureAuto(dir + L"model2_normal.png");
    if (!tdN.valid) tdN = LoadTextureAuto(dir + L"model2_normal.jpg");
    if (!tdN.valid) tdN = CreateFlatNormal();
    rs_->UploadTextureMesh2(tdN, 2);

    // displacement — процедурный синусоидальный узор
    TextureData tdD = LoadTextureAuto(dir + L"model2_displacement.png");
    if (!tdD.valid) tdD = LoadTextureAuto(dir + L"model2_displacement.jpg");
    if (!tdD.valid)
    {
        const UINT SZ = 128;
        tdD.width = tdD.height = SZ;
        tdD.pixels.resize(SZ * SZ * 4); tdD.valid = true;
        for (UINT y = 0;y < SZ;++y)
            for (UINT x = 0;x < SZ;++x)
            {
                float fx = float(x) / SZ, fy = float(y) / SZ;
                float v = 0.5f + 0.5f * sinf(fx * 8.f * PI) * sinf(fy * 8.f * PI);
                uint8_t b = static_cast<uint8_t>(v * 255.f);
                UINT idx = (y * SZ + x) * 4;
                tdD.pixels[idx] = tdD.pixels[idx + 1] = tdD.pixels[idx + 2] = b;
                tdD.pixels[idx + 3] = 255;
            }
    }
    rs_->UploadTextureMesh2(tdD, 3);
}

// ============================================================
//  Raycast
// ============================================================
bool Game::RayTriangle(XMFLOAT3 orig, XMFLOAT3 dir, XMFLOAT3 v0, XMFLOAT3 v1, XMFLOAT3 v2, float& t)
{
    const float EPS = 1e-7f;
    XMFLOAT3 e1 = F3Sub(v1, v0), e2 = F3Sub(v2, v0);
    XMFLOAT3 h = F3Cross(dir, e2); float a = F3Dot(e1, h);
    if (a > -EPS && a < EPS) return false;
    float f = 1.f / a;
    XMFLOAT3 s = F3Sub(orig, v0); float u = f * F3Dot(s, h);
    if (u < 0.f || u>1.f) return false;
    XMFLOAT3 q = F3Cross(s, e1); float v = f * F3Dot(dir, q);
    if (v < 0.f || u + v>1.f) return false;
    t = f * F3Dot(e2, q); return (t > EPS);
}

bool Game::RaycastMesh(XMFLOAT3 origin, XMFLOAT3 dir, float maxDist, float& outT) const
{
    if (meshIdxs_.size() < 3) return false;
    outT = maxDist; bool hit = false;
    XMMATRIX invWorld = XMMatrixInverse(nullptr, worldMatrix_);
    XMVECTOR origV = XMVector3TransformCoord(XMLoadFloat3(&origin), invWorld);
    XMVECTOR dirV = XMVector3Normalize(XMVector3TransformNormal(XMLoadFloat3(&dir), invWorld));
    XMFLOAT3 origOS, dirOS;
    XMStoreFloat3(&origOS, origV); XMStoreFloat3(&dirOS, dirV);
    for (size_t i = 0;i < meshIdxs_.size() / 3;++i)
    {
        float t = 0.f;
        if (RayTriangle(origOS, dirOS,
            meshVerts_[meshIdxs_[i * 3]].Position,
            meshVerts_[meshIdxs_[i * 3 + 1]].Position,
            meshVerts_[meshIdxs_[i * 3 + 2]].Position, t))
            if (t < outT) { outT = t;hit = true; }
    }
    return hit;
}

// ============================================================
//  Выстрел
// ============================================================
void Game::OnShoot()
{
    int activeCount = 0; for (auto& b : bullets_) if (b.Active) ++activeCount;
    if (activeCount >= MAX_BULLETS) return;
    float cosPitch = cosf(pitch_), sinPitch = sinf(pitch_);
    float cosYaw = cosf(yaw_), sinYaw = sinf(yaw_);
    XMFLOAT3 dir = F3Norm({ sinYaw * cosPitch,-sinPitch,cosYaw * cosPitch });
    static int colorIdx = 0;
    static const XMFLOAT3 kColors[] = { {1.f,.3f,.05f},{.1f,.5f,1.f},{.05f,1.f,.3f},{1.f,.9f,.1f},{.8f,.1f,1.f},{.05f,1.f,.9f} };
    LightBullet b = {};
    b.Position = camPos_; b.Direction = dir;
    b.Color = kColors[colorIdx++ % 6];
    b.Intensity = 8.f; b.Range = 7.f; b.Speed = BULLET_SPEED; b.Active = true;
    bullets_.push_back(b);
}

// ============================================================
//  UpdateBullets
// ============================================================
void Game::UpdateBullets(float dt)
{
    for (auto& b : bullets_)
    {
        if (!b.Active || b.Stuck) continue;
        float stepLen = b.Speed * dt;
        XMFLOAT3 newPos = F3Add(b.Position, F3Scale(b.Direction, stepLen));
        float hitT = 0.f;
        if (RaycastMesh(b.Position, b.Direction, stepLen + 0.05f, hitT) && hitT <= stepLen + 0.05f)
        {
            XMFLOAT3 hitPos = F3Add(b.Position, F3Scale(b.Direction, hitT));
            b.Active = false; b.Stuck = true;
            b.StuckPosition[0] = hitPos.x; b.StuckPosition[1] = hitPos.y; b.StuckPosition[2] = hitPos.z;
            if ((int)stuckLights_.size() >= MAX_STUCK_LIGHTS) stuckLights_.erase(stuckLights_.begin());
            stuckLights_.push_back({ hitPos,b.Color,b.Intensity,b.Range });
        }
        else
        {
            b.Position = newPos;
            float dx = b.Position.x - camPos_.x, dy = b.Position.y - camPos_.y, dz = b.Position.z - camPos_.z;
            if (dx * dx + dy * dy + dz * dz > BULLET_MAX_DIST * BULLET_MAX_DIST) b.Active = false;
        }
    }
    bullets_.erase(std::remove_if(bullets_.begin(), bullets_.end(),
        [](const LightBullet& b) {return !b.Active && !b.Stuck;}), bullets_.end());
}

// ============================================================
//  Update
// ============================================================
void Game::Update(float deltaTime)
{
    time_ += deltaTime;
    uvOffsetX_ += 0.05f * deltaTime; if (uvOffsetX_ > 1.f) uvOffsetX_ -= 1.f;

    if (mouseCaptured_)
    {
        RECT rc; GetClientRect(hwnd_, &rc);
        POINT center = { (rc.right - rc.left) / 2,(rc.bottom - rc.top) / 2 };
        ClientToScreen(hwnd_, &center);
        centerX_ = center.x; centerY_ = center.y;
    }

    float cosPitch = cosf(pitch_), sinPitch = sinf(pitch_);
    float cosYaw = cosf(yaw_), sinYaw = sinf(yaw_);
    XMFLOAT3 forward = F3Norm({ sinYaw * cosPitch,-sinPitch,cosYaw * cosPitch });
    XMFLOAT3 right = F3Norm(F3Cross(forward, { 0,1,0 }));
    XMFLOAT3 up = F3Norm(F3Cross(right, forward));

    const float speed = 5.0f;
    if (GetAsyncKeyState('W') & 0x8000) camPos_ = F3Add(camPos_, F3Scale(forward, speed * deltaTime));
    if (GetAsyncKeyState('S') & 0x8000) camPos_ = F3Sub(camPos_, F3Scale(forward, speed * deltaTime));
    if (GetAsyncKeyState('A') & 0x8000) camPos_ = F3Sub(camPos_, F3Scale(right, speed * deltaTime));
    if (GetAsyncKeyState('D') & 0x8000) camPos_ = F3Add(camPos_, F3Scale(right, speed * deltaTime));
    if (GetAsyncKeyState(VK_SPACE) & 0x8000)   camPos_.y += speed * deltaTime;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) camPos_.y -= speed * deltaTime;
    camForward_ = forward;

    XMMATRIX world = XMMatrixIdentity(); worldMatrix_ = world;
    XMVECTOR eyeV = XMLoadFloat3(&camPos_);
    XMFLOAT3 targetF = F3Add(camPos_, forward);
    XMMATRIX view = XMMatrixLookAtLH(eyeV, XMLoadFloat3(&targetF), XMLoadFloat3(&up));
    float aspect = (height_ > 0) ? float(width_) / float(height_) : 1.f;
    XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(75.f), aspect, 0.05f, 500.f);

    const float DIST_NEAR = 1.0f, DIST_FAR = 20.0f;
    float distToCenter = sqrtf(camPos_.x * camPos_.x + camPos_.y * camPos_.y + camPos_.z * camPos_.z);
    float t = saturate01((distToCenter - DIST_NEAR) / (DIST_FAR - DIST_NEAR));
    // BlendFactor=0: всегда показываем t0 (diffuse материала), t1 не используется
    float blend = 0.0f;
    float tessMax = 1.0f + (16.0f - 1.0f) * (1.0f - t);
    float dispScaleEffective = dispScale_ * (1.0f - t);

    ConstantBufferData geomCB = {};
    geomCB.World = XMMatrixTranspose(world);
    geomCB.View = XMMatrixTranspose(view);
    geomCB.Proj = XMMatrixTranspose(proj);
    geomCB.CameraPos = { camPos_.x,camPos_.y,camPos_.z,1.f };
    geomCB.Tiling = { 1.f,1.f };
    geomCB.UVOffset = { 0.f,0.f };
    geomCB.BlendFactor = blend;
    geomCB.TessNear = DIST_NEAR; geomCB.TessFar = DIST_FAR;
    geomCB.TessMinLevel = 1.0f; geomCB.TessMaxLevel = tessMax;
    geomCB.DisplacementScale = dispScaleEffective;
    rs_->UpdateGeometryPassCB(geomCB);

    UpdateBullets(deltaTime);
    rs_->ClearLights();

    for (auto& sl : stuckLights_)
        rs_->AddPointLight(sl.Position, sl.Color, sl.Intensity, sl.Range);
    for (auto& b : bullets_)
        if (b.Active && !b.Stuck)
            rs_->AddPointLight(b.Position, b.Color, b.Intensity * 0.7f, 2.5f);

    // Динамические источники
    { float sa = time_ * 0.15f; rs_->AddDirectionalLight({ sinf(sa),-0.6f - 0.3f * cosf(time_ * 0.07f),cosf(sa) }, { 1.f,.92f,.75f }, 1.4f); }
    { float ma = time_ * 0.15f + PI; rs_->AddDirectionalLight({ sinf(ma),-.4f,cosf(ma) }, { .35f,.4f,.65f }, .3f); }
    { float a = time_ * .9f, r = 4.5f; rs_->AddPointLight({ cosf(a) * r,1.f,sinf(a) * r }, { 1.f,.15f,.05f }, 6.f, 9.f); }
    { float a = time_ * .7f + 2.f * PI / 3.f, r = 3.8f; rs_->AddPointLight({ cosf(a) * r,3.f + sinf(time_ * .5f) * 1.5f,sinf(a) * r }, { .1f,.4f,1.f }, 5.f, 10.f); }
    { float a = time_ * 1.1f + 4.f * PI / 3.f, r = 3.2f; rs_->AddPointLight({ cosf(a) * r,1.5f + cosf(a * .8f) * 2.f,sinf(a) * r }, { .05f,1.f,.3f }, 4.5f, 8.f); }
    { float sa = time_ * .5f, sr = .6f; XMFLOAT3 sd = F3Norm({ sinf(sa) * sr,-1.f,cosf(sa) * sr }); rs_->AddSpotLight({ -5.f,6.f,-5.f }, sd, { 1.f,.85f,.5f }, 8.f, 14.f, 12.f, 25.f); }
    { float sa = -time_ * .7f + PI, sr = .5f; XMFLOAT3 sd = F3Norm({ sinf(sa) * sr - .4f,-1.f,cosf(sa) * sr - .4f }); rs_->AddSpotLight({ 5.f,5.f,5.f }, sd, { .8f,.1f,1.f }, 7.f, 12.f, 10.f, 22.f); }
    { float pulse = .6f + .4f * fabsf(sinf(time_ * 2.5f)); XMFLOAT3 sd = F3Norm({ 0.f,.6f,-1.f }); rs_->AddSpotLight({ 0.f,-3.f,6.f }, sd, { 1.f,1.f,1.f }, 5.f * pulse, 11.f, 8.f, 20.f); }

    LightingPassCB lCB = rs_->GetLightingCB();
    lCB.CameraPos = { camPos_.x,camPos_.y,camPos_.z,1.f };
    rs_->UpdateLightingPassCB(lCB);

    // CB второй модели
    // BlendFactor: плавный переход albedo1->albedo2 с расстоянием до модели.
    // Mesh2 всегда стоит в (0,0,0), поэтому distToCenter подходит напрямую.
    // Вблизи (< MESH2_NEAR): blend=0 -> albedo1
    // Вдали  (> MESH2_FAR):  blend=1 -> albedo2
    const float MESH2_NEAR = 1.5f;
    const float MESH2_FAR = 2.0f;
    float blendMesh2 = saturate01((distToCenter - MESH2_NEAR) / (MESH2_FAR - MESH2_NEAR));

    mesh2Rotation_ += 0.3f * deltaTime;
    XMMATRIX world2 = XMMatrixRotationY(mesh2Rotation_); worldMatrix2_ = world2;
    ConstantBufferData geomCB2 = {};
    geomCB2.World = XMMatrixTranspose(world2);
    geomCB2.View = XMMatrixTranspose(view);
    geomCB2.Proj = XMMatrixTranspose(proj);
    geomCB2.CameraPos = { camPos_.x,camPos_.y,camPos_.z,1.f };
    geomCB2.Tiling = { 1.f,1.f }; geomCB2.UVOffset = { 0.f,0.f };
    geomCB2.BlendFactor = blendMesh2;
    geomCB2.TessNear = DIST_NEAR; geomCB2.TessFar = DIST_FAR;
    geomCB2.TessMinLevel = 1.0f; geomCB2.TessMaxLevel = tessMax;
    geomCB2.DisplacementScale = dispScaleEffective * 0.4f;
    rs_->UpdateGeometryPassCB2(geomCB2);
}

// ============================================================
//  Render
// ============================================================
void Game::Render()
{
    rs_->BeginFrame();

    // Geometry Pass — Mesh1 (Sponza, per-material субмеши)
    rs_->BindGeometryPass();
    rs_->DrawMesh1SubMeshes();

    // Geometry Pass — Mesh2 (вторая модель)
    if (rs_->HasMesh2())
    {
        rs_->BindGeometryPassMesh2();
        rs_->DrawMesh2();
    }

    rs_->BeginLightingPass();
    rs_->DrawLightingQuad();
    rs_->EndFrame();
}

void Game::Resize(int width, int height)
{
    if (width <= 0 || height <= 0) return;
    width_ = width; height_ = height;
    rs_->Resize(width, height);
}