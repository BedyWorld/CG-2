#include "Game.h"
#include "Utils.h"
#include <cmath>
#include <algorithm>
#include <limits>
#include <cfloat>
#include <random>
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
        { LoadTimer t("Initialize (устройства, PSO)"); rs_->Initialize(); }
        rs_->BeginResourceUpload();
        { LoadTimer t("Sponza: меш + текстуры"); LoadSceneGeometry(); }
        { LoadTimer t("вторая модель"); LoadSceneGeometry2(); }
        { LoadTimer t("текстуры второй модели"); LoadSceneTextures2(); }
        { LoadTimer t("EndResourceUpload (ожидание GPU)"); rs_->EndResourceUpload(); }
        rs_->ReleaseTextureUploadBuffers();
        { LoadTimer t("CB + тени + частицы"); rs_->CreateConstantBuffers(); }
        { LoadTimer t("октодерево (300 объектов)"); BuildSceneObjects(); }
        { LoadTimer t("плоскость воды"); BuildWaterPlane(); }
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
    if (vkey == 'B') { waterEnabled_ = !waterEnabled_; return; }
    if (vkey == 'F') { ToggleFrustumCulling(); return; }
    if (vkey == 'O') { ToggleOctreeCulling();  return; }
    if (vkey == VK_OEM_PLUS || vkey == VK_ADD)       waterAmpScale_ = min(waterAmpScale_ + 0.1f, 3.0f);
    if (vkey == VK_OEM_MINUS || vkey == VK_SUBTRACT) waterAmpScale_ = max(waterAmpScale_ - 0.1f, 0.0f);
    if (vkey == 'T') { tessEnabled_ = !tessEnabled_; return; }
    if (vkey == 'C') { shadowsEnabled_ = !shadowsEnabled_; return; }
    if (vkey == 'P') { particlesEnabled_ = !particlesEnabled_; return; }
    if (vkey == 'G') {
        wireframe_ = !wireframe_;
        rs_->SetWireframe(wireframe_);
        return;
    }
    if (vkey == 'V') { debugMode_ = (debugMode_ + 1) % 3; return; }
    if (vkey == 'I') { instancesEnabled_ = !instancesEnabled_; return; }
    if (vkey == VK_OEM_PLUS || vkey == VK_ADD)       dispScale_ = min(dispScale_ + 0.05f, 2.0f);
    if (vkey == VK_OEM_MINUS || vkey == VK_SUBTRACT) dispScale_ = max(dispScale_ - 0.05f, 0.0f);
}

void Game::ClearStuckLights()
{
    stuckLights_.clear();
    bullets_.clear();
}

void Game::SetMeshForRaycast(const std::vector<Vertex>& verts, const std::vector<UINT>& idxs)
{
    meshVerts_ = verts; meshIdxs_ = idxs;
    { LoadTimer _t("BuildBVH (трассировка)"); BuildBVH(); }
}

void Game::LoadSceneGeometry()
{
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dir(exePath);
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);

    ObjResult obj = LoadObj(dir + L"sponza.obj");

    if (obj.valid && !obj.subMeshes.empty())
    {
        struct MatEntry { const wchar_t* diff; const wchar_t* norm; const wchar_t* disp; };
        static const std::unordered_map<std::wstring, MatEntry> kMatMap = {
            { L"arch", { L"sponza_arch_diff.tga", L"sponza_arch_ddn.tga", L"sponza_arch_disp.tga" } },
            { L"bricks", { L"spnza_bricks_a_diff.tga", L"spnza_bricks_a_ddn.tga", L"spnza_bricks_a_disp.tga" } },
            { L"ceiling", { L"sponza_ceiling_a_diff.tga", L"sponza_ceiling_a_ddn.tga", L"sponza_ceiling_a_disp.tga" } },
            { L"chain", { L"chain_texture.tga", L"chain_texture_ddn.tga", L"chain_texture_disp.tga" } },
            { L"column_a", { L"sponza_column_a_diff.tga", L"sponza_column_a_ddn.tga", L"sponza_column_a_disp.tga" } },
            { L"column_b", { L"sponza_column_c_diff.tga", L"sponza_column_b_ddn.tga", L"sponza_column_b_disp.tga" } },
            { L"column_c", { L"sponza_column_c_diff.tga", L"sponza_column_c_ddn.tga", L"sponza_column_c_disp.tga" } },
            { L"details", { L"sponza_details_diff.tga", L"sponza_details_ddn.tga", L"sponza_details_disp.tga" } },
            { L"fabric_a", { L"sponza_curtain_diff.tga", L"sponza_curtain_ddn.tga", L"sponza_curtain_disp.tga" } },
            { L"fabric_c", { L"sponza_curtain_blue_diff.tga", L"sponza_curtain_ddn.tga", L"sponza_curtain_disp.tga" } },
            { L"fabric_d", { L"sponza_curtain_diff.tga", L"sponza_curtain_ddn.tga", L"sponza_curtain_disp.tga" } },
            { L"fabric_e", { L"sponza_curtain_green_diff.tga", L"sponza_curtain_ddn.tga", L"sponza_curtain_disp.tga" } },
            { L"fabric_f", { L"sponza_fabric_diff.tga", L"sponza_fabric_ddn.tga", L"sponza_fabric_disp.tga" } },
            { L"fabric_g", { L"sponza_fabric_green_diff.tga", L"sponza_fabric_ddn.tga", L"sponza_fabric_disp.tga" } },
            { L"flagpole", { L"sponza_flagpole_diff.tga", L"sponza_flagpole_ddn.tga", L"sponza_flagpole_disp.tga" } },
            { L"floor", { L"sponza_floor_a_diff.tga", L"sponza_floor_a_ddn.tga", L"sponza_floor_a_disp.tga" } },
            { L"leaf", { L"sponza_thorn_diff.tga", L"sponza_thorn_ddn.tga", L"sponza_thorn_disp.tga" } },
            { L"roof", { L"sponza_roof_diff.tga", L"sponza_roof_ddn.tga", L"sponza_roof_disp.tga" } },
            { L"vase", { L"vase_dif.tga", L"vase_ddn.tga", L"vase_disp.tga" } },
            { L"vase_hanging", { L"vase_hanging.tga", L"vase_hanging_ddn.tga", L"vase_hanging_disp.tga" } },
            { L"vase_round", { L"vase_round.tga", L"vase_round_ddn.tga", L"vase_round_disp.tga" } },
            { L"Material__25", { L"background.tga", L"background_ddn.tga", L"background_disp.tga" } },
            { L"Material__298", { L"background.tga", L"background_ddn.tga", L"background_disp.tga" } },
            { L"Material__47", { L"lion.tga", L"lion_ddn.tga", L"lion_disp.tga" } },
            { L"Material__57", { L"lion.tga", L"lion2_ddn.tga", L"lion2_disp.tga" } },
        };

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
        std::vector<std::wstring> diffPaths, normPaths, dispPaths;

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
                dispPaths.push_back(dir + it->second.disp);
            }
            else
            {
                diffPaths.push_back(L"");
                normPaths.push_back(L"");
                dispPaths.push_back(L"");
            }
        }

        sponzaBounds_.clear();
        sponzaBounds_.reserve(starts.size());
        for (size_t sm = 0; sm < starts.size(); ++sm)
        {
            AABB b;
            b.Min = { FLT_MAX,  FLT_MAX,  FLT_MAX };
            b.Max = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
            const UINT from = starts[sm], to = starts[sm] + counts[sm];
            for (UINT k = from; k < to && k < obj.indices.size(); ++k)
            {
                const XMFLOAT3& p = obj.vertices[obj.indices[k]].Position;
                b.Min.x = fminf(b.Min.x, p.x); b.Min.y = fminf(b.Min.y, p.y); b.Min.z = fminf(b.Min.z, p.z);
                b.Max.x = fmaxf(b.Max.x, p.x); b.Max.y = fmaxf(b.Max.y, p.y); b.Max.z = fmaxf(b.Max.z, p.z);
            }
            if (b.Min.x > b.Max.x || b.Min.y > b.Max.y || b.Min.z > b.Max.z)
            {
                b.Min = { -FLT_MAX / 4, -FLT_MAX / 4, -FLT_MAX / 4 };
                b.Max = { FLT_MAX / 4,  FLT_MAX / 4,  FLT_MAX / 4 };
            }
            else
            {
                const float pad = 0.5f;
                b.Min.x -= pad; b.Min.y -= pad; b.Min.z -= pad;
                b.Max.x += pad; b.Max.y += pad; b.Max.z += pad;
            }
            sponzaBounds_.push_back(b);
        }

        rs_->BuildMesh1(obj.vertices, obj.indices, starts, counts,
            diffPaths, normPaths, dispPaths);
        SetMeshForRaycast(obj.vertices, obj.indices);
        return;
    }

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
    std::vector<std::wstring> d = { L"" }, nm = { L"" }, dp = { L"" };
    rs_->BuildMesh1(verts, idxs, s, c, d, nm, dp);
    SetMeshForRaycast(verts, idxs);
}

void Game::LoadSceneGeometry2()
{
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dir(exePath);
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);

    ObjResult obj = LoadObj(dir + L"model2.obj");
    if (obj.valid) { rs_->BuildBuffers2(obj.vertices, obj.indices); return; }

    const int SLICES = 16, STACKS = 8;
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

void Game::BuildWaterPlane()
{
    const int   N = 48;
    const float HALF = 34.0f;
    const float Y = waterHeight_;

    std::vector<Vertex> verts;
    std::vector<UINT>   idxs;
    verts.reserve((N + 1) * (N + 1));

    for (int z = 0; z <= N; ++z)
        for (int x = 0; x <= N; ++x)
        {
            float fx = -HALF + 2.0f * HALF * (float(x) / N);
            float fz = -HALF + 2.0f * HALF * (float(z) / N);
            Vertex v = {};
            v.Position = { fx, Y, fz };
            v.Normal = { 0,1,0 };
            v.Color = { 1,1,1,1 };
            v.TexCoord = { float(x) / N, float(z) / N };
            verts.push_back(v);
        }

    auto IDX = [&](int x, int z) { return (UINT)(z * (N + 1) + x); };
    for (int z = 0; z < N; ++z)
        for (int x = 0; x < N; ++x)
        {
            UINT a = IDX(x, z), b = IDX(x + 1, z), c = IDX(x + 1, z + 1), d = IDX(x, z + 1);
            idxs.insert(idxs.end(), { a,b,c, a,c,d }); // каждый треугольник = patch
        }

    rs_->BuildWater(verts, idxs);
}

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

    // displacement
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

static inline float FMin(float a, float b) { return a < b ? a : b; }
static inline float FMax(float a, float b) { return a > b ? a : b; }

void Game::BuildBVH()
{
    bvhNodes_.clear();
    triIndices_.clear();
    triCentroids_.clear();

    const size_t triCount = meshIdxs_.size() / 3;
    if (triCount == 0) return;

    triIndices_.resize(triCount);
    triCentroids_.resize(triCount);
    for (size_t i = 0; i < triCount; ++i)
    {
        triIndices_[i] = static_cast<int>(i);
        const XMFLOAT3& a = meshVerts_[meshIdxs_[i * 3 + 0]].Position;
        const XMFLOAT3& b = meshVerts_[meshIdxs_[i * 3 + 1]].Position;
        const XMFLOAT3& c = meshVerts_[meshIdxs_[i * 3 + 2]].Position;
        triCentroids_[i] = { (a.x + b.x + c.x) / 3.f,
                             (a.y + b.y + c.y) / 3.f,
                             (a.z + b.z + c.z) / 3.f };
    }

    bvhNodes_.reserve(triCount * 2);
    BuildBVHRecursive(0, static_cast<int>(triCount), 0);
}

int Game::BuildBVHRecursive(int first, int count, int depth)
{
    const int nodeIdx = static_cast<int>(bvhNodes_.size());
    bvhNodes_.push_back(BVHNode{});

    XMFLOAT3 bmin = { FLT_MAX,  FLT_MAX,  FLT_MAX };
    XMFLOAT3 bmax = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (int i = 0; i < count; ++i)
    {
        const int tri = triIndices_[first + i];
        for (int k = 0; k < 3; ++k)
        {
            const XMFLOAT3& p = meshVerts_[meshIdxs_[tri * 3 + k]].Position;
            bmin.x = FMin(bmin.x, p.x); bmin.y = FMin(bmin.y, p.y); bmin.z = FMin(bmin.z, p.z);
            bmax.x = FMax(bmax.x, p.x); bmax.y = FMax(bmax.y, p.y); bmax.z = FMax(bmax.z, p.z);
        }
    }
    bvhNodes_[nodeIdx].bmin = bmin;
    bvhNodes_[nodeIdx].bmax = bmax;

    const int kLeafSize = 8;
    const int kMaxDepth = 40;
    if (count <= kLeafSize || depth >= kMaxDepth)
    {
        bvhNodes_[nodeIdx].left = -1;
        bvhNodes_[nodeIdx].first = first;
        bvhNodes_[nodeIdx].count = count;
        return nodeIdx;
    }

    const float ex = bmax.x - bmin.x, ey = bmax.y - bmin.y, ez = bmax.z - bmin.z;
    const int axis = (ex > ey && ex > ez) ? 0 : (ey > ez ? 1 : 2);

    auto centroidAxis = [&](int tri) {
        const XMFLOAT3& c = triCentroids_[tri];
        return axis == 0 ? c.x : (axis == 1 ? c.y : c.z);
        };

    const int mid = count / 2;
    std::nth_element(triIndices_.begin() + first,
        triIndices_.begin() + first + mid,
        triIndices_.begin() + first + count,
        [&](int a, int b) { return centroidAxis(a) < centroidAxis(b); });

    if (mid == 0 || mid == count)
    {
        bvhNodes_[nodeIdx].left = -1;
        bvhNodes_[nodeIdx].first = first;
        bvhNodes_[nodeIdx].count = count;
        return nodeIdx;
    }

    const int leftChild = BuildBVHRecursive(first, mid, depth + 1);
    const int rightChild = BuildBVHRecursive(first + mid, count - mid, depth + 1);

    bvhNodes_[nodeIdx].left = leftChild;
    bvhNodes_[nodeIdx].right = rightChild;
    bvhNodes_[nodeIdx].count = 0;
    return nodeIdx;
}

static bool RayAABB(const XMFLOAT3& orig, const XMFLOAT3& invDir,
    const XMFLOAT3& bmin, const XMFLOAT3& bmax, float tMax, float& tNear)
{
    float t0 = (bmin.x - orig.x) * invDir.x, t1 = (bmax.x - orig.x) * invDir.x;
    if (t0 > t1) { float tmp = t0; t0 = t1; t1 = tmp; }
    float lo = t0, hi = t1;

    t0 = (bmin.y - orig.y) * invDir.y; t1 = (bmax.y - orig.y) * invDir.y;
    if (t0 > t1) { float tmp = t0; t0 = t1; t1 = tmp; }
    lo = FMax(lo, t0); hi = FMin(hi, t1);

    t0 = (bmin.z - orig.z) * invDir.z; t1 = (bmax.z - orig.z) * invDir.z;
    if (t0 > t1) { float tmp = t0; t0 = t1; t1 = tmp; }
    lo = FMax(lo, t0); hi = FMin(hi, t1);

    tNear = lo;
    return hi >= FMax(lo, 0.0f) && lo <= tMax;
}

bool Game::TraverseBVH(const XMFLOAT3& orig, const XMFLOAT3& invDir,
    const XMFLOAT3& dir, float& outT) const
{
    if (bvhNodes_.empty()) return false;

    bool hit = false;
    int stack[64];
    int sp = 0;
    stack[sp++] = 0;

    while (sp > 0)
    {
        const int nodeIdx = stack[--sp];
        const BVHNode& node = bvhNodes_[nodeIdx];

        float tNear = 0.f;
        if (!RayAABB(orig, invDir, node.bmin, node.bmax, outT, tNear))
            continue;
        if (tNear > outT) continue;

        if (node.count > 0)
        {
            for (int i = 0; i < node.count; ++i)
            {
                const int tri = triIndices_[node.first + i];
                float t = 0.f;
                if (RayTriangle(orig, dir,
                    meshVerts_[meshIdxs_[tri * 3 + 0]].Position,
                    meshVerts_[meshIdxs_[tri * 3 + 1]].Position,
                    meshVerts_[meshIdxs_[tri * 3 + 2]].Position, t))
                {
                    if (t < outT) { outT = t; hit = true; }
                }
            }
        }
        else if (sp + 2 <= 64)
        {
            stack[sp++] = node.left;
            stack[sp++] = node.right;
        }
    }
    return hit;
}

bool Game::RaycastMesh(XMFLOAT3 origin, XMFLOAT3 dir, float maxDist, float& outT) const
{
    if (meshIdxs_.size() < 3 || bvhNodes_.empty()) return false;

    outT = maxDist;

    XMMATRIX invWorld = XMMatrixInverse(nullptr, worldMatrix_);
    XMVECTOR origV = XMVector3TransformCoord(XMLoadFloat3(&origin), invWorld);
    XMVECTOR dirV = XMVector3Normalize(XMVector3TransformNormal(XMLoadFloat3(&dir), invWorld));
    XMFLOAT3 origOS, dirOS;
    XMStoreFloat3(&origOS, origV); XMStoreFloat3(&dirOS, dirV);
    
    const float kTiny = 1e-8f;
    XMFLOAT3 invDir = {
        1.0f / (fabsf(dirOS.x) < kTiny ? copysignf(kTiny, dirOS.x ? dirOS.x : 1.f) : dirOS.x),
        1.0f / (fabsf(dirOS.y) < kTiny ? copysignf(kTiny, dirOS.y ? dirOS.y : 1.f) : dirOS.y),
        1.0f / (fabsf(dirOS.z) < kTiny ? copysignf(kTiny, dirOS.z ? dirOS.z : 1.f) : dirOS.z)
    };

    return TraverseBVH(origOS, invDir, dirOS, outT);
}
void Game::OnShoot()
{
    if ((int)bullets_.size() >= MAX_BULLETS) return;
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
        [](const LightBullet& b) { return !b.Active; }), bullets_.end());
}


static XMFLOAT4X4 TransposeF4x4(const XMFLOAT4X4& m)
{
    XMFLOAT4X4 r;
    XMStoreFloat4x4(&r, XMMatrixTranspose(XMLoadFloat4x4(&m)));
    return r;
}

void Game::BuildSceneObjects()
{
    objects_.clear();
    objectBounds_.clear();
    objects_.reserve(SCENE_OBJECT_COUNT);
    objectBounds_.reserve(SCENE_OBJECT_COUNT);

    std::mt19937 rng(20240501);
    std::uniform_real_distribution<float> ux(-28.f, 28.f);
    std::uniform_real_distribution<float> uy(-2.f, 16.f);
    std::uniform_real_distribution<float> uz(-13.f, 13.f);
    std::uniform_real_distribution<float> us(0.12f, 0.38f);
    std::uniform_real_distribution<float> usp(-1.6f, 1.6f);
    std::uniform_real_distribution<float> uph(0.f, 6.283f);

    for (int i = 0; i < SCENE_OBJECT_COUNT; ++i)
    {
        SceneObject o;
        o.Position = { ux(rng), uy(rng), uz(rng) };
        o.Scale = us(rng);
        o.SpinSpeed = usp(rng);
        o.SpinPhase = uph(rng);
        objects_.push_back(o);

        const float r = o.Scale * 1.7321f;
        AABB b;
        b.Min = { o.Position.x - r, o.Position.y - r, o.Position.z - r };
        b.Max = { o.Position.x + r, o.Position.y + r, o.Position.z + r };
        objectBounds_.push_back(b);
    }

    octree_.Build(objectBounds_);
    visibleObjects_.reserve(SCENE_OBJECT_COUNT);
    instanceMatrices_.reserve(SCENE_OBJECT_COUNT);
}

void Game::CullSceneObjects()
{
    const LARGE_INTEGER t0 = []() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t; }();

    visibleObjects_.clear();
    statNodesVisited_ = 0;

    if (!frustumCullEnabled_)
    {
        visibleObjects_.resize(objects_.size());
        for (size_t i = 0; i < objects_.size(); ++i)
            visibleObjects_[i] = static_cast<int>(i);
    }
    else
    {
        XMFLOAT4X4 vp;
        XMStoreFloat4x4(&vp, XMMatrixMultiply(viewMatrix_, projMatrix_));
        const Frustum frustum = Frustum::FromViewProj(vp);

        if (octreeCullEnabled_ && !octree_.Empty())
        {
            octree_.Query(frustum, visibleObjects_);
            statNodesVisited_ = octree_.LastVisitedNodes();
        }
        else
        {
            for (size_t i = 0; i < objectBounds_.size(); ++i)
                if (frustum.Intersects(objectBounds_[i]))
                    visibleObjects_.push_back(static_cast<int>(i));
        }
    }

    LARGE_INTEGER t1, freq;
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&freq);
    statCullMs_ = 1000.0f * float(t1.QuadPart - t0.QuadPart) / float(freq.QuadPart);
    statVisible_ = static_cast<int>(visibleObjects_.size());

    visibleSubMeshes_.clear();
    statSubTotal_ = static_cast<int>(sponzaBounds_.size());
    if (!frustumCullEnabled_)
    {
        visibleSubMeshes_.resize(sponzaBounds_.size());
        for (size_t i = 0; i < sponzaBounds_.size(); ++i)
            visibleSubMeshes_[i] = static_cast<int>(i);
    }
    else
    {
        XMFLOAT4X4 vpm;
        XMStoreFloat4x4(&vpm, XMMatrixMultiply(viewMatrix_, projMatrix_));
        const Frustum fr = Frustum::FromViewProj(vpm);
        for (size_t i = 0; i < sponzaBounds_.size(); ++i)
            if (fr.Intersects(sponzaBounds_[i]))
                visibleSubMeshes_.push_back(static_cast<int>(i));
    }
    statSubVisible_ = static_cast<int>(visibleSubMeshes_.size());

    instanceMatrices_.clear();
    allInstanceMatrices_.clear();
    for (size_t i = 0; i < objects_.size(); ++i)
    {
        const SceneObject& ao = objects_[i];
        const XMMATRIX am =
            XMMatrixScaling(ao.Scale, ao.Scale, ao.Scale) *
            XMMatrixRotationY(ao.SpinPhase + time_ * ao.SpinSpeed) *
            XMMatrixTranslation(ao.Position.x, ao.Position.y, ao.Position.z);
        XMFLOAT4X4 af;
        XMStoreFloat4x4(&af, XMMatrixTranspose(am));
        allInstanceMatrices_.push_back(af);
    }
    for (int idx : visibleObjects_)
    {
        const SceneObject& o = objects_[idx];
        const XMMATRIX m =
            XMMatrixScaling(o.Scale, o.Scale, o.Scale) *
            XMMatrixRotationY(o.SpinPhase + time_ * o.SpinSpeed) *
            XMMatrixTranslation(o.Position.x, o.Position.y, o.Position.z);

        XMFLOAT4X4 f;
        XMStoreFloat4x4(&f, XMMatrixTranspose(m));
        instanceMatrices_.push_back(f);
    }
    rs_->UpdateInstances(instanceMatrices_, allInstanceMatrices_);
}

void Game::UpdateWindowTitle()
{
    const wchar_t* mode = !frustumCullEnabled_ ? L"off"
        : (octreeCullEnabled_ ? L"octree" : L"brute");

    wchar_t buf[256];
    swprintf_s(buf,
        L"%.1f ms (%.0f fps) | obj %d/%d %s(I) | Sponza %d/%d shadow %d | cull %s(F/O) %.3fms "
        L"| tess %s(T) | shadows %s(C) | particles %s(P) | wire %s(G)",
        frameMsAvg_, frameMsAvg_ > 0.01f ? 1000.0f / frameMsAvg_ : 0.0f,
        statVisible_, SCENE_OBJECT_COUNT, instancesEnabled_ ? L"on " : L"off",
        statSubVisible_, statSubTotal_, statShadowSubs_,
        mode, statCullMs_,
        tessEnabled_ ? L"on" : L"off",
        shadowsEnabled_ ? L"on" : L"off",
        particlesEnabled_ ? L"on" : L"off",
        wireframe_ ? L"on" : L"off");
    SetWindowTextW(hwnd_, buf);
}

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
    if (GetAsyncKeyState('A') & 0x8000) camPos_ = F3Add(camPos_, F3Scale(right, speed * deltaTime));
    if (GetAsyncKeyState('D') & 0x8000) camPos_ = F3Sub(camPos_, F3Scale(right, speed * deltaTime));
    if (GetAsyncKeyState(VK_SPACE) & 0x8000)   camPos_.y += speed * deltaTime;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) camPos_.y -= speed * deltaTime;
    // E поднимает камеру Q опускает
    if (GetAsyncKeyState('E') & 0x8000) camPos_.y += speed * deltaTime;
    if (GetAsyncKeyState('Q') & 0x8000) camPos_.y -= speed * deltaTime;

    const float modelSpeed = 3.0f;
    if (GetAsyncKeyState(VK_LEFT) & 0x8000)  mesh2Pos_.x -= modelSpeed * deltaTime;
    if (GetAsyncKeyState(VK_RIGHT) & 0x8000) mesh2Pos_.x += modelSpeed * deltaTime;
    if (GetAsyncKeyState(VK_UP) & 0x8000)    mesh2Pos_.z += modelSpeed * deltaTime;
    if (GetAsyncKeyState(VK_DOWN) & 0x8000)  mesh2Pos_.z -= modelSpeed * deltaTime;
    if (GetAsyncKeyState(VK_PRIOR) & 0x8000) mesh2Pos_.y += modelSpeed * deltaTime;
    if (GetAsyncKeyState(VK_NEXT) & 0x8000)  mesh2Pos_.y -= modelSpeed * deltaTime;
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
    float blend = 0.0f;

    const float tessMax = tessEnabled_ ? 5.0f : 1.0f;
    const float dispScaleEffective = dispScale_;

    ConstantBufferData geomCB = {};
    geomCB.World = XMMatrixTranspose(world);
    viewMatrix_ = view; projMatrix_ = proj;
    geomCB.View = XMMatrixTranspose(view);
    geomCB.Proj = XMMatrixTranspose(proj);
    geomCB.CameraPos = { camPos_.x,camPos_.y,camPos_.z,1.f };
    geomCB.Tiling = { 1.f,1.f };
    geomCB.UVOffset = { 0.f,0.f };
    geomCB.BlendFactor = blend;
    geomCB.TessNear = 0.5f; geomCB.TessFar = 12.0f;
    geomCB.TessMinLevel = 1.0f; geomCB.TessMaxLevel = tessMax;
    geomCB.DisplacementScale = dispScaleEffective;
    geomCB.WireMode = wireframe_ ? 1.0f : 0.0f;
    rs_->UpdateGeometryPassCB(geomCB);

    UpdateBullets(deltaTime);
    rs_->ClearLights();

    {
        float sa = time_ * 0.15f;
        sunDir_ = { sinf(sa), -0.6f - 0.3f * cosf(time_ * 0.07f), cosf(sa) };
        sunLightIndex_ = 0;
        rs_->AddDirectionalLight(sunDir_, { 1.f,.92f,.75f }, 1.4f);
    }
    { float ma = time_ * 0.15f + PI; rs_->AddDirectionalLight({ sinf(ma),-.4f,cosf(ma) }, { .35f,.4f,.65f }, .3f); }
    { float a = time_ * .9f, r = 4.5f; rs_->AddPointLight({ cosf(a) * r,1.f,sinf(a) * r }, { 1.f,.15f,.05f }, 6.f, 9.f); }
    { float a = time_ * .7f + 2.f * PI / 3.f, r = 3.8f; rs_->AddPointLight({ cosf(a) * r,3.f + sinf(time_ * .5f) * 1.5f,sinf(a) * r }, { .1f,.4f,1.f }, 5.f, 10.f); }
    { float a = time_ * 1.1f + 4.f * PI / 3.f, r = 3.2f; rs_->AddPointLight({ cosf(a) * r,1.5f + cosf(a * .8f) * 2.f,sinf(a) * r }, { .05f,1.f,.3f }, 4.5f, 8.f); }
    { float sa = time_ * .5f, sr = .6f; XMFLOAT3 sd = F3Norm({ sinf(sa) * sr,-1.f,cosf(sa) * sr }); rs_->AddSpotLight({ -5.f,6.f,-5.f }, sd, { 1.f,.85f,.5f }, 8.f, 14.f, 12.f, 25.f); }
    { float sa = -time_ * .7f + PI, sr = .5f; XMFLOAT3 sd = F3Norm({ sinf(sa) * sr - .4f,-1.f,cosf(sa) * sr - .4f }); rs_->AddSpotLight({ 5.f,5.f,5.f }, sd, { .8f,.1f,1.f }, 7.f, 12.f, 10.f, 22.f); }
    { float pulse = .6f + .4f * fabsf(sinf(time_ * 2.5f)); XMFLOAT3 sd = F3Norm({ 0.f,.6f,-1.f }); rs_->AddSpotLight({ 0.f,-3.f,6.f }, sd, { 1.f,1.f,1.f }, 5.f * pulse, 11.f, 8.f, 20.f); }

    for (auto& b : bullets_)
        if (b.Active && !b.Stuck)
            rs_->AddPointLight(b.Position, b.Color, b.Intensity * 0.7f, 2.5f);
    for (auto& sl : stuckLights_)
        rs_->AddPointLight(sl.Position, sl.Color, sl.Intensity, sl.Range);

    CullSceneObjects();

    frameMsAvg_ = frameMsAvg_ * 0.94f + (deltaTime * 1000.0f) * 0.06f;
    titleTimer_ += deltaTime;
    if (titleTimer_ >= 0.25f) { titleTimer_ = 0.f; UpdateWindowTitle(); }

    const float kShadowDistance = 80.0f;
    CascadedShadowMap& csm = rs_->Shadows();
    if (csm.Ready() && shadowsEnabled_)
    {
        csm.SetCasterDistance(60.0f);
        csm.Update(viewMatrix_, XMConvertToRadians(75.f), aspect,
            0.5f, kShadowDistance, sunDir_, 0.85f);
    }

    {
        XMMATRIX invView = XMMatrixInverse(nullptr, viewMatrix_);
        XMVECTOR r = XMVector3Normalize(invView.r[0]);
        XMVECTOR u = XMVector3Normalize(invView.r[1]);
        XMVECTOR f = XMVector3Normalize(invView.r[2]);

        XMStoreFloat4x4(&particleCB_.View, XMMatrixTranspose(viewMatrix_));
        XMStoreFloat4x4(&particleCB_.Proj, XMMatrixTranspose(projMatrix_));
        XMStoreFloat4(&particleCB_.CameraRight, r);
        XMStoreFloat4(&particleCB_.CameraUp, u);
        XMStoreFloat4(&particleCB_.CameraForward, f);

        particleCB_.EmitOrigin = { 0.0f, -1.6f, 4.0f, 0.0f };
        particleCB_.Gravity = { 0.0f, -0.9f, 0.0f, 0.0f };
        particleCB_.DeltaTime = deltaTime;
        particleCB_.TotalTime = time_;
        particleCB_.EmitCount = particlesEnabled_ ? 220u : 0u;
        particleCB_.EmitSpeed = 1.5f;
        particleCB_.LifeMin = 0.8f;  particleCB_.LifeSpan = 0.9f;
        particleCB_.SizeMin = 0.10f; particleCB_.SizeSpan = 0.18f;
        particleCB_.Turbulence = 1.4f;
        particleCB_.Buoyancy = 3.6f;
        particleCB_.SmokeLife = 3.5f; 
        particleCB_.SmokeParams = { 0.5f, 0.45f, 0.30f, 0.0f };
    }
    {
        waterCB_.World = XMMatrixIdentity();
        waterCB_.View = XMMatrixTranspose(view);
        waterCB_.Proj = XMMatrixTranspose(proj);
        waterCB_.CameraPos = { camPos_.x, camPos_.y, camPos_.z, 1.f };
        waterCB_.ShallowColor = { 0.25f, 0.55f, 0.50f, 1.f };
        waterCB_.DeepColor = { 0.02f, 0.08f, 0.12f, 1.f };
        waterCB_.WaveDirAmp[0] = { 1.00f,  0.00f, 0.18f * waterAmpScale_, 0.25f };
        waterCB_.WaveDirAmp[1] = { 0.60f,  0.80f, 0.10f * waterAmpScale_, 0.50f };
        waterCB_.WaveDirAmp[2] = { -0.94f, 0.34f, 0.06f * waterAmpScale_, 1.10f };
        waterCB_.WaveDirAmp[3] = { 0.20f, -0.98f, 0.03f * waterAmpScale_, 2.30f };
        waterCB_.WaveSpeed = { 1.1f, 1.6f, 2.0f, 2.6f };
        waterCB_.Time = time_;
        waterCB_.TessNear = 2.0f; waterCB_.TessFar = 40.0f;
        waterCB_.TessMinLevel = 1.0f;
        waterCB_.TessMaxLevel = tessEnabled_ ? 16.0f : 1.0f;
        waterCB_.Roughness = 0.05f;
        waterCB_.WaveCount = 4;
        rs_->UpdateWaterCB(waterCB_);
    }
    LightingPassCB lCB = rs_->GetLightingCB();
    if (csm.Ready() && shadowsEnabled_)
    {
        for (int c = 0; c < MAX_CASCADES; ++c)
            lCB.ShadowViewProj[c] = TransposeF4x4(csm.ViewProj(c));
        lCB.CascadeSplits = csm.SplitDepths();
        XMStoreFloat4x4(&lCB.CameraView, XMMatrixTranspose(viewMatrix_));
        lCB.ShadowLightIndex = sunLightIndex_;
        lCB.ShadowMapSize = static_cast<float>(csm.Resolution());
        lCB.ShadowBias = 0.0015f;
        lCB.DebugCascades = static_cast<float>(debugMode_);
    }
    else
    {
        lCB.ShadowLightIndex = -1;
    }
    lCB.CameraPos = { camPos_.x,camPos_.y,camPos_.z,1.f };
    rs_->UpdateLightingPassCB(lCB);

    const float MESH2_NEAR = 1.5f;
    const float MESH2_FAR = 2.0f;
    float blendMesh2 = saturate01((distToCenter - MESH2_NEAR) / (MESH2_FAR - MESH2_NEAR));

    mesh2Rotation_ += 0.3f * deltaTime;
    XMMATRIX world2 = XMMatrixRotationY(mesh2Rotation_) *
        XMMatrixTranslation(mesh2Pos_.x, mesh2Pos_.y, mesh2Pos_.z);
    worldMatrix2_ = world2;
    ConstantBufferData geomCB2 = {};
    geomCB2.World = XMMatrixTranspose(world2);
    geomCB2.View = XMMatrixTranspose(view);
    geomCB2.Proj = XMMatrixTranspose(proj);
    geomCB2.CameraPos = { camPos_.x,camPos_.y,camPos_.z,1.f };
    geomCB2.Tiling = { 1.f,1.f }; geomCB2.UVOffset = { 0.f,0.f };
    geomCB2.BlendFactor = blendMesh2;
    geomCB2.TessNear = 0.5f; geomCB2.TessFar = 12.0f;
    geomCB2.TessMinLevel = 1.0f; geomCB2.TessMaxLevel = tessMax;
    geomCB2.DisplacementScale = dispScaleEffective * 0.4f;
    geomCB2.WireMode = wireframe_ ? 1.0f : 0.0f;
    rs_->UpdateGeometryPassCB2(geomCB2);
}

void Game::RenderShadowCascades()
{
    CascadedShadowMap& csm = rs_->Shadows();
    if (!csm.Ready() || !shadowsEnabled_) return;

    for (int c = 0; c < csm.CascadeCount(); ++c)
    {
        rs_->BeginShadowCascade(c, csm.ViewProj(c));

        if (!sponzaBounds_.empty())
        {
            const Frustum lightFrustum = Frustum::FromViewProj(csm.ViewProj(c));
            shadowSubMeshes_.clear();
            for (size_t i = 0; i < sponzaBounds_.size(); ++i)
                if (lightFrustum.Intersects(sponzaBounds_[i]))
                    shadowSubMeshes_.push_back(static_cast<int>(i));
            statShadowSubs_ = static_cast<int>(shadowSubMeshes_.size());

            const size_t half = sponzaBounds_.size() / 2;
            if (shadowSubMeshes_.size() < half)
                rs_->DrawShadowMesh1Sub(shadowSubMeshes_);
            else
                rs_->DrawShadowMesh1();
        }
        else
        {
            rs_->DrawShadowMesh1();
        }
        if (rs_->HasMesh2() && instancesEnabled_)
            rs_->DrawShadowInstances(static_cast<UINT>(objects_.size()));
    }
    rs_->EndShadowPass();
}

void Game::Render()
{
    rs_->BeginFrame();

    if (particlesEnabled_) rs_->SimulateParticles(particleCB_);

    RenderShadowCascades();

    rs_->BindGeometryPass();
    rs_->DrawMesh1SubMeshes(sponzaBounds_.empty() ? nullptr : &visibleSubMeshes_);

    if (rs_->HasMesh2())
    {
        rs_->BindGeometryPassMesh2();
        rs_->DrawMesh2();

        if (instancesEnabled_)
        {
            rs_->BindInstancedPass();
            rs_->DrawInstances(static_cast<UINT>(visibleObjects_.size()));
        }
    }
    if (waterEnabled_ && rs_->HasWater())
    {
        rs_->BindWaterPass();
        rs_->DrawWater();
    }
    if (particlesEnabled_) rs_->DrawParticles();

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