#pragma once
#include <windows.h>
#include <memory>
#include <string>
#include <vector>
#include "RenderingSystem.h"
#include "ObjLoader.h"
#include "TextureLoader.h"
#include "Types.h"

// ============================================================
//  Game — логика сцены, делегирует рендеринг в RenderingSystem.
//
//  Реализует двупроходный Deferred Rendering:
//    1. Geometry Pass  — заполняет GBuffer через RenderingSystem
//    2. Lighting Pass  — освещение full-screen quad
//
//  Источники света управляются через RenderingSystem::AddXxxLight().
//
//  Стрельба световыми снарядами (LMB):
//    - При нажатии ЛКМ из позиции камеры в направлении взгляда
//      запускается LightBullet.
//    - Снаряд летит с заданной скоростью и на каждом Update()
//      проверяется против CPU-сайдкаста по треугольникам меша.
//    - При попадании снаряд «прилипает» к точке удара и начинает
//      постоянно светить как точечный источник.
//    - Максимум MAX_STUCK_LIGHTS прилипших источников хранятся
//      независимо от лимита динамических источников.
// ============================================================

static const int MAX_BULLETS      = 64;   // снарядов в воздухе одновременно
static const int MAX_STUCK_LIGHTS  = 8;   // прилипших источников (не больше MAX_LIGHTS - 9)
static const float BULLET_SPEED    = 12.0f;
static const float BULLET_MAX_DIST = 200.0f; // максимальная дистанция полёта

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
    // Вызывается из WndProc при нажатии ЛКМ
    void OnShoot();

    // Для передачи меша в рейкастер после загрузки
    void SetMeshForRaycast(const std::vector<Vertex>& verts,
                           const std::vector<UINT>&   idxs);

private:
    void LoadSceneGeometry();
    void LoadSceneTextures();

    // ---- CPU рейкаст ----
    // Возвращает true и записывает hitT (расстояние вдоль луча), если луч
    // (origin + dir*t) пересекает хотя бы один треугольник меша.
    bool RaycastMesh(XMFLOAT3 origin, XMFLOAT3 dir,
                     float maxDist, float& outT) const;

    // Пересечение луча с треугольником (алгоритм Мёллера–Трумбора).
    static bool RayTriangle(XMFLOAT3 orig, XMFLOAT3 dir,
                             XMFLOAT3 v0, XMFLOAT3 v1, XMFLOAT3 v2,
                             float& t);

    // Обновляет полёт снарядов, возможно переводит их в Stuck.
    void UpdateBullets(float dt);

    HWND hwnd_;
    int  width_;
    int  height_;

    std::unique_ptr<RenderingSystem> rs_;

    // ---- Меш для CPU рейкаста ----
    std::vector<Vertex> meshVerts_;
    std::vector<UINT>   meshIdxs_;

    // ---- Снаряды ----
    std::vector<LightBullet> bullets_;

    // ---- Прилипшие источники ----
    struct StuckLight
    {
        XMFLOAT3 Position;
        XMFLOAT3 Color;
        float    Intensity;
        float    Range;
    };
    std::vector<StuckLight> stuckLights_;

    // ---- Состояние кнопки мыши (защита от удержания) ----
    bool prevLMB_ = false;

    // ---- Анимация ----
    float rotationAngle_ = 0.0f;
    float uvOffsetX_ = 0.0f;
    float time_ = 0.0f;
    float cameraDistance_ = 3.0f;
    float cameraAngle_ = 0.0f;

    // ---- Текущая позиция и направление камеры (обновляется в Update) ----
    XMFLOAT3 camPos_    = {};
    XMFLOAT3 camForward_= {0,0,1};

    // ---- Мировая матрица сцены (для трансформации рейкаста) ----
    XMMATRIX worldMatrix_ = {};
};
