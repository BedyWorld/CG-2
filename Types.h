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
    XMFLOAT4 PositionWS;
    XMFLOAT4 DirectionWS;
    XMFLOAT4 Color;
    float    Range;
    float    SpotInnerCosine;
    float    SpotOuterCosine;
    UINT     Type;
};  // 96 bytes

static const UINT MAX_LIGHTS = 16;

// ---- Constant Buffer для Geometry Pass (с тесселяцией) ----
struct ConstantBufferData
{
    XMMATRIX World;             // 64
    XMMATRIX View;              // 64
    XMMATRIX Proj;              // 64
    XMFLOAT4 CameraPos;         // 16
    XMFLOAT2 Tiling;            //  8
    XMFLOAT2 UVOffset;          //  8
    float    BlendFactor;       //  4
    // ---- тесселяция ----
    float    TessNear;          //  4  — дистанция максимального уровня тесселяции
    float    TessFar;           //  4  — дистанция минимального уровня тесселяции
    float    TessMinLevel;      //  4  — мин. уровень (вдали)
    float    TessMaxLevel;      //  4  — макс. уровень (вблизи)
    float    DisplacementScale; //  4  — сила displacement
    float    Padding[2];        //  8
                                // total = 256
};

// ---- Constant Buffer для Lighting Pass ----
struct LightingPassCB
{
    XMFLOAT4 CameraPos;
    int      LightCount;
    XMFLOAT3 Padding0;
    LightData Lights[MAX_LIGHTS];
};

// ============================================================
//  Световой снаряд
// ============================================================
struct LightBullet
{
    XMFLOAT3 Position;
    XMFLOAT3 Direction;
    XMFLOAT3 Color;
    float    Intensity;
    float    Range;
    float    Speed;
    bool     Active;
    bool     Stuck;
    float    StuckPosition[3];
};
