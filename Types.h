#pragma once
#include <cstddef>   // offsetof
#include <DirectXMath.h>
using namespace DirectX;

struct Vertex
{
    XMFLOAT3 Position;  // 12
    XMFLOAT3 Normal;    // 12
    XMFLOAT4 Color;     // 16
    XMFLOAT2 TexCoord;  // 8
};


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

struct ConstantBufferData
{
    XMMATRIX World;             // 64
    XMMATRIX View;              // 64
    XMMATRIX Proj;              // 64
    XMFLOAT4 CameraPos;         // 16
    XMFLOAT2 Tiling;            //  8
    XMFLOAT2 UVOffset;          //  8
    float    BlendFactor;       //  4
    //  тесселяция 
    float    TessNear;          //  4  — дистанция максимального уровня тесселяции
    float    TessFar;           //  4  — дистанция минимального уровня тесселяции
    float    TessMinLevel;      //  4  — мин. уровень (вдали)
    float    TessMaxLevel;      //  4  — макс. уровень (вблизи)
    float    DisplacementScale; //  4  — сила displacement
    float    WireMode;          //  4  — >0.5 рисуем сетку самосветящейся
    float    Padding;           //  4
                                // total = 256
};

static const int MAX_CASCADES = 4;

struct LightingPassCB
{
    XMFLOAT4 CameraPos;
    int      LightCount;
    XMFLOAT3 Padding0;
    LightData Lights[MAX_LIGHTS];

    //каскадные тени
    XMFLOAT4X4 ShadowViewProj[MAX_CASCADES];   // матрицы света по каскадам
    XMFLOAT4   CascadeSplits;                  // дальние границы во view-space
    XMFLOAT4X4 CameraView;                     // нужна для глубины в view-space
    int        ShadowLightIndex;               // -1 = теней нет
    float      ShadowMapSize;
    float      ShadowBias;
    float      DebugCascades;      // >0.5 — подкрасить каскады (клавиша V)
};

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

static_assert(sizeof(LightData) == 64, "LightData: 64 байта");
static_assert(offsetof(LightingPassCB, Lights) == 32, "сдвинулся массив Lights");
static_assert(offsetof(LightingPassCB, ShadowViewProj) == 1056, "сдвинулись матрицы каскадов");
static_assert(offsetof(LightingPassCB, CascadeSplits) == 1312, "сдвинулись границы каскадов");
static_assert(offsetof(LightingPassCB, CameraView) == 1328, "сдвинулась матрица вида");
static_assert(offsetof(LightingPassCB, ShadowLightIndex) == 1392, "сдвинулся хвост");
static_assert(sizeof(LightingPassCB) == 1408, "изменился размер LightingPassCB");

static_assert(offsetof(ConstantBufferData, CameraPos) == 192, "ConstantBufferData: CameraPos");
static_assert(offsetof(ConstantBufferData, BlendFactor) == 224, "ConstantBufferData: BlendFactor");
static_assert(sizeof(ConstantBufferData) == 256, "изменился размер ConstantBufferData");
