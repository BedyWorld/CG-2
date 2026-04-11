#pragma once
#include <DirectXMath.h>
using namespace DirectX;

// ---- Vertex (with UV) ----
struct Vertex
{
    XMFLOAT3 Position;  // 12
    XMFLOAT3 Normal;    // 12
    XMFLOAT4 Color;     // 16
    XMFLOAT2 TexCoord;  // 8   -> total = 48 bytes
};

// ============================================================
//  Структуры источников света
// ============================================================

enum class LightType : UINT
{
    Directional = 0,
    Point       = 1,
    Spot        = 2,
};

// Один источник света (GPU-совместимый, 96 байт)
struct LightData
{
    XMFLOAT4 PositionWS;        // world-space позиция (Point / Spot)
    XMFLOAT4 DirectionWS;       // world-space направление (Directional / Spot)
    XMFLOAT4 Color;             // RGB цвет + интенсивность в W
    float    Range;             // радиус затухания (Point / Spot)
    float    SpotInnerCosine;   // cos внутреннего угла конуса (Spot)
    float    SpotOuterCosine;   // cos внешнего угла конуса (Spot)
    UINT     Type;              // LightType (0=Dir, 1=Point, 2=Spot)
};                              // 96 bytes

static const UINT MAX_LIGHTS = 16;

// ---- Constant Buffer для Geometry Pass (256 байт) ----
struct ConstantBufferData
{
    XMMATRIX World;       // 64
    XMMATRIX View;        // 64
    XMMATRIX Proj;        // 64
    XMFLOAT4 CameraPos;   // 16
    XMFLOAT2 Tiling;      //  8
    XMFLOAT2 UVOffset;    //  8
    float    BlendFactor; //  4
    float    Padding[3];  // 12
                          // total = 240
};

// ---- Constant Buffer для Lighting Pass ----
struct LightingPassCB
{
    XMFLOAT4 CameraPos;           // 16
    int      LightCount;          //  4
    XMFLOAT3 Padding0;            // 12
    LightData Lights[MAX_LIGHTS]; // MAX_LIGHTS * 96 = 1536
                                  // total = 1568
};

// ============================================================
//  Световой снаряд — летит по прямой, при ударе о геометрию
//  превращается в постоянный точечный источник света.
// ============================================================
struct LightBullet
{
    XMFLOAT3 Position;      // текущая позиция в world-space
    XMFLOAT3 Direction;     // нормализованное направление полёта
    XMFLOAT3 Color;         // цвет света
    float    Intensity;     // интенсивность
    float    Range;         // радиус освещения после прилипания
    float    Speed;         // скорость в unit/sec
    bool     Active;        // true = ещё летит, false = прилип или мёртв
    bool     Stuck;         // true = прилип к геометрии, продолжает светить
    float    StuckPosition[3]; // позиция прилипания (копия Position после удара)
};
