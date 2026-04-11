// ============================================================
//  GBufferShaders.hlsl — Geometry Pass
//
//  Записывает в GBuffer:
//    RT0 (R8G8B8A8_UNORM)      — Albedo.rgb (из текстур), A=1
//    RT1 (R16G16B16A16_FLOAT)  — Normal.xyz в world space, A=0
//    RT2 (R8G8B8A8_UNORM)      — Roughness(R), Metallic(G), AO(B), A=1
// ============================================================

cbuffer GeometryCB : register(b0)
{
    matrix   World;
    matrix   View;
    matrix   Proj;
    float4   CameraPos;
    float2   Tiling;
    float2   UVOffset;
    float    BlendFactor;
    float3   CBPad;
};

Texture2D gAlbedo1 : register(t0);   // первичная текстура
Texture2D gAlbedo2 : register(t1);   // вторичная текстура (blending)
SamplerState gSampler : register(s0);

struct VSInput
{
    float3 Pos      : POSITION;
    float3 Normal   : NORMAL;
    float4 Color    : COLOR;
    float2 TexCoord : TEXCOORD0;
};

struct VSOutput
{
    float4 Pos      : SV_POSITION;
    float3 WorldPos : TEXCOORD0;
    float3 Normal   : TEXCOORD1;
    float2 TexCoord : TEXCOORD2;
};

// ---- Vertex Shader ----
VSOutput VSMain(VSInput input)
{
    VSOutput output;
    float4 worldPos   = mul(float4(input.Pos, 1.0f), World);
    output.Pos        = mul(mul(worldPos, View), Proj);
    output.WorldPos   = worldPos.xyz;
    output.Normal     = normalize(mul(float4(input.Normal, 0.0f), World).xyz);
    output.TexCoord   = input.TexCoord * Tiling + UVOffset;
    return output;
}

// ---- Pixel Shader output (MRT) ----
struct PSOutput
{
    float4 Albedo : SV_Target0;   // RT0
    float4 Normal : SV_Target1;   // RT1
    float4 PBR    : SV_Target2;   // RT2
};

PSOutput PSMain(VSOutput input)
{
    PSOutput output;

    // ---- Albedo: смешение двух текстур ----
    float4 tex1 = gAlbedo1.Sample(gSampler, input.TexCoord);
    float4 tex2 = gAlbedo2.Sample(gSampler, input.TexCoord);
    output.Albedo = float4(lerp(tex1.rgb, tex2.rgb, BlendFactor), 1.0f);

    // ---- Normal: упаковываем из [-1,1] в [0,1] для хранения в R8G8B8A8 ----
    // RT1 — R16G16B16A16_FLOAT, поэтому храним напрямую без упаковки.
    float3 N = normalize(input.Normal);
    output.Normal = float4(N, 0.0f);

    // ---- PBR: статические материальные параметры ----
    // Roughness=0.6, Metallic=0.1, AO=1.0
    // В реальном проекте здесь читаются карты материала.
    output.PBR = float4(0.6f, 0.1f, 1.0f, 1.0f);

    return output;
}
