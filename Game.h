#pragma once
#include <windows.h>
#include <memory>
#include <string>
#include <vector>
#include "RenderingSystem.h"
#include "Culling.h"
#include "ObjLoader.h"
#include "TextureLoader.h"
#include "Types.h"
#include "Terrain.h"

static const int   MAX_BULLETS = 8;
static const int   MAX_STUCK_LIGHTS = 6;
static const int   SCENE_LIGHT_COUNT = 8;   // сколько слотов резервирует сцена
static const float BULLET_SPEED = 12.0f;
static const float BULLET_MAX_DIST = 200.0f;

class Game
{
public:
    Game(HWND hwnd, int width, int height);
    ~Game() = default;

    Game(const Game&) = delete;
    Game& operator=(const Game&) = delete;

    bool Initialize();
    void Update(float deltaTime);
    void Render();
    void Resize(int width, int height);

    void ClearStuckLights();

    void ToggleFrustumCulling() { frustumCullEnabled_ = !frustumCullEnabled_; }
    void ToggleOctreeCulling() { octreeCullEnabled_ = !octreeCullEnabled_; }
    void OnShoot();
    void OnMouseMove(int screenX, int screenY);
    void OnMouseDown(int button);
    void OnKeyDown(int vkey);

    void SetMeshForRaycast(const std::vector<Vertex>& verts, const std::vector<UINT>& idxs);

    static const int SCENE_OBJECT_COUNT = 300;

    void  BuildSceneObjects();      // расстановка + построение октодерева
    void  CullSceneObjects();       // отбор видимых на текущий кадр
    void  UpdateWindowTitle();      // статистика в заголовке окна

    struct SceneObject
    {
        XMFLOAT3 Position{ 0,0,0 };
        float    Scale = 1.0f;
        float    SpinSpeed = 0.0f;
        float    SpinPhase = 0.0f;
    };

    std::vector<SceneObject>  objects_;
    std::vector<AABB>         objectBounds_;
    Octree                    octree_;
    static const int TERRAIN_TILES = 2;
    Terrain terrain_[TERRAIN_TILES * TERRAIN_TILES];
    bool terrainOnly_ = false;
    bool lodBoostEnabled_ = false;

    std::vector<int>          visibleObjects_;
    std::vector<XMFLOAT4X4>   instanceMatrices_;
    std::vector<XMFLOAT4X4>   allInstanceMatrices_;

    std::vector<AABB>         sponzaBounds_;
    std::vector<int>          visibleSubMeshes_;
    int  statSubVisible_ = 0;
    int  statShadowSubs_ = 0;
    std::vector<int> shadowSubMeshes_;
    float frameMsAvg_ = 16.0f;
    int  statSubTotal_ = 0;

    bool  frustumCullEnabled_ = true;   // клавиша F
    bool  octreeCullEnabled_ = true;   // клавиша O
    bool  tessEnabled_ = true;   // клавиша T
    bool  instancesEnabled_ = false;  // клавиша I, по умолчанию выключены
    bool  shadowsEnabled_ = true;   // клавиша C — каскадные тени
    bool  particlesEnabled_ = true;   // клавиша P
    ParticleCB particleCB_ = {};
    bool  wireframe_ = false;   // клавиша G — каркасный режим
    int   debugMode_ = 0;      // клавиша V: 0 выкл, 1 каскады, 2 карта теней

    void     RenderShadowCascades();
    XMFLOAT3 sunDir_ = { 0,-1,0 };   // направление источника, отбрасывающего тени
    int      sunLightIndex_ = -1;    // его индекс в массиве Lights
    int   statVisible_ = 0;
    int   statNodesVisited_ = 0;
    float statCullMs_ = 0.0f;
    float titleTimer_ = 0.0f;

    void BuildWaterPlane();
    WaterCB waterCB_ = {};
    bool    waterEnabled_ = true;
    float   waterHeight_ = 9.5f;
    float waterAmpScale_ = 1.0f;

private:
    void LoadSceneGeometry();      // Sponza — загрузка меша + текстур через BuildMesh1
    void LoadSceneGeometry2();     // вторая модель
    void LoadSceneTextures2();     // текстуры второй модели

    bool RaycastMesh(XMFLOAT3 origin, XMFLOAT3 dir, float maxDist, float& outT) const;
    static bool RayTriangle(XMFLOAT3 orig, XMFLOAT3 dir,
        XMFLOAT3 v0, XMFLOAT3 v1, XMFLOAT3 v2, float& t);

    struct BVHNode
    {
        XMFLOAT3 bmin;
        XMFLOAT3 bmax;
        int      left = -1;      // индекс левого потомка  (-1 у листа)
        int      right = -1;     // индекс правого потомка (-1 у листа)
        int      first = 0;      // первый треугольник в triIndices_ (лист)
        int      count = 0;      // кол-во треугольников (0 у внутреннего узла)
    };

    void BuildBVH();
    int  BuildBVHRecursive(int first, int count, int depth);
    bool TraverseBVH(const XMFLOAT3& orig, const XMFLOAT3& invDir,
        const XMFLOAT3& dir, float& outT) const;

    std::vector<BVHNode> bvhNodes_;
    std::vector<int>     triIndices_;   // перестановка индексов треугольников
    std::vector<XMFLOAT3> triCentroids_;
    void UpdateBullets(float dt);
    void CaptureMouse();
    void ReleaseMouse();

    HWND hwnd_;
    int  width_, height_;

    std::unique_ptr<RenderingSystem> rs_;

    std::vector<Vertex> meshVerts_;
    std::vector<UINT>   meshIdxs_;

    std::vector<LightBullet> bullets_;

    struct StuckLight { XMFLOAT3 Position; XMFLOAT3 Color; float Intensity; float Range; };
    std::vector<StuckLight> stuckLights_;

    float uvOffsetX_ = 0.0f;
    float time_ = 0.0f;

    XMFLOAT3 camPos_ = { 0.0f, 5.0f, 0.0f };
    XMFLOAT3 camForward_ = { 0.0f, 0.0f,  1.0f };
    float    yaw_ = 0.0f;
    float    pitch_ = 0.0f;

    bool  mouseCaptured_ = false;
    int   centerX_ = 0, centerY_ = 0;

    float    dispScale_ = 0.3f;

    XMMATRIX worldMatrix_ = {};
    XMMATRIX viewMatrix_ = {};   // сохраняются для построения фрустума
    XMMATRIX projMatrix_ = {};

    bool taaEnabled_ = true;
    int  taaAlphaIdx_ = 1;
    UINT taaFrame_ = 0;
    XMFLOAT2 taaJitter_ = { 0.f, 0.f };
    XMFLOAT2 taaPrevJitter_ = { 0.f, 0.f };
    XMFLOAT4X4 viewProjNoJitter_ = {};
    XMFLOAT4X4 prevViewProjNoJitter_ = {};

    XMMATRIX worldMatrix2_ = {};
    XMFLOAT3 mesh2Pos_ = { 0.0f, 0.0f, 0.0f };   // стрелки двигают модель
    float    mesh2Rotation_ = 0.0f;
};