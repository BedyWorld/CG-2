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

// ---- Constant Buffer (256 bytes exactly) ----
struct ConstantBufferData
{
    XMMATRIX World;       // 64
    XMMATRIX View;        // 64
    XMMATRIX Proj;        // 64
    XMFLOAT4 LightPos;    // 16
    XMFLOAT4 LightColor;  // 16
    XMFLOAT4 CameraPos;   // 16
    XMFLOAT2 Tiling;      //  8  -- texture tiling factor (U, V)
    XMFLOAT2 UVOffset;    //  8  -- animated UV scroll offset
                          // total = 256
};
