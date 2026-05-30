#pragma once
#include <windows.h>
#include <memory>
#include <string>
#include <vector>
#include "RenderingSystem.h"
#include "ObjLoader.h"
#include "TextureLoader.h"
#include "Types.h"

static const int   MAX_BULLETS = 64;
static const int   MAX_STUCK_LIGHTS = 8;
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
    void OnShoot();
    void OnMouseMove(int screenX, int screenY);
    void OnMouseDown(int button);
    void OnKeyDown(int vkey);

    void SetMeshForRaycast(const std::vector<Vertex>& verts, const std::vector<UINT>& idxs);

private:
    void LoadSceneGeometry();      // Sponza — загрузка меша + текстур через BuildMesh1
    void LoadSceneGeometry2();     // вторая модель
    void LoadSceneTextures2();     // текстуры второй модели

    bool RaycastMesh(XMFLOAT3 origin, XMFLOAT3 dir, float maxDist, float& outT) const;
    static bool RayTriangle(XMFLOAT3 orig, XMFLOAT3 dir,
        XMFLOAT3 v0, XMFLOAT3 v1, XMFLOAT3 v2, float& t);
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
    XMMATRIX worldMatrix2_ = {};
    float    mesh2Rotation_ = 0.0f;
};