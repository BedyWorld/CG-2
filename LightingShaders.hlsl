// ============================================================
//  LightingShaders.hlsl — Lighting Pass (Deferred Rendering)
//
//  Читает GBuffer и вычисляет финальный цвет пикселя.
//
//  Поддерживаемые типы источников света:
//    0 = Directional  — бесконечно далёкий параллельный свет
//    1 = Point        — точечный свет с затуханием 1/(d² + ε)
//    2 = Spot         — конический свет с плавным penumbra
//
//  GBuffer layout:
//    t0 — Albedo  (R8G8B8A8_UNORM)      rgb=albedo
//    t1 — Normal  (R16G16B16A16_FLOAT)  rgb=world-space normal
//    t2 — PBR     (R8G8B8A8_UNORM)      r=roughness, g=metallic, b=ao
// ============================================================

#define MAX_LIGHTS 16
#define LIGHT_DIRECTIONAL 0
#define LIGHT_POINT       1
#define LIGHT_SPOT        2

struct LightData
{
    float4 PositionWS;      // xyz=pos,  w=1 (Point/Spot) или 0 (Dir)
    float4 DirectionWS;     // xyz=dir (нормализован в C++)
    float4 Color;           // xyz=rgb, w=intensity
    float  Range;
    float  SpotInnerCosine;
    float  SpotOuterCosine;
    uint   Type;
};

cbuffer LightingCB : register(b0)
{
    float4    CameraPos;
    uint      LightCount;
    float3    LCBPad;
    LightData Lights[MAX_LIGHTS];
};

Texture2D gAlbedo : register(t0);
Texture2D gNormal : register(t1);
Texture2D gPBR : register(t2);

Texture2D gWorldPos : register(t3);

SamplerState gSampler : register(s0);  

struct FSQOutput
{
    float4 Pos : SV_POSITION;
    float2 UV  : TEXCOORD0;
};

FSQOutput VSMain(uint vid : SV_VertexID)
{
    float2 pos;
    float2 uv;
    if (vid == 0) { pos = float2(-1, -1); uv = float2(0, 1); }
    else if (vid == 1) { pos = float2(-1,  1); uv = float2(0, 0); }
    else if (vid == 2) { pos = float2( 1,  1); uv = float2(1, 0); }
    else if (vid == 3) { pos = float2(-1, -1); uv = float2(0, 1); }
    else if (vid == 4) { pos = float2( 1,  1); uv = float2(1, 0); }
    else               { pos = float2( 1, -1); uv = float2(1, 1); }

    FSQOutput o;
    o.Pos = float4(pos, 0.0f, 1.0f);
    o.UV  = uv;
    return o;
}

// Затухание по расстоянию
float ComputeAttenuation(float distance, float range)
{
    // Smooth step к нулю на границе range
    float falloff = saturate(1.0f - (distance / range));
    return falloff * falloff / (distance * distance + 1.0f);
}

// Blinn-Phong BRDF: diffuse + specular
float3 BlinnPhong(float3 N, float3 L, float3 V,
                   float3 albedo, float roughness, float3 lightColor)
{
    float diff = max(dot(N, L), 0.0f);

    float3 H  = normalize(L + V);
    float shininess = max(2.0f / (roughness * roughness + 0.001f) - 2.0f, 1.0f);
    float spec = pow(max(dot(N, H), 0.0f), shininess);

    float3 diffuse  = albedo * diff * lightColor;
    float3 specular = spec * lightColor * (1.0f - roughness);

    return diffuse + specular;
}

// ---- Directional Light ----
float3 CalcDirectional(LightData light, float3 N, float3 V, float3 albedo, float roughness)
{
    float3 L = normalize(-light.DirectionWS.xyz);
    float3 color = BlinnPhong(N, L, V, albedo, roughness, light.Color.rgb * light.Color.w);
    return color;
}

// ---- Point Light ----
float3 CalcPoint(LightData light, float3 worldPos, float3 N, float3 V,
                  float3 albedo, float roughness)
{
    float3 toLight = light.PositionWS.xyz - worldPos;
    float  dist    = length(toLight);
    if (dist >= light.Range) return float3(0, 0, 0);

    float3 L   = toLight / dist;
    float  att = ComputeAttenuation(dist, light.Range);
    float3 col = BlinnPhong(N, L, V, albedo, roughness, light.Color.rgb * light.Color.w);
    return col * att;
}

float3 CalcSpot(LightData light, float3 worldPos, float3 N, float3 V,
                 float3 albedo, float roughness)
{
    float3 toLight = light.PositionWS.xyz - worldPos;
    float  dist    = length(toLight);
    if (dist >= light.Range) return float3(0, 0, 0);

    float3 L        = toLight / dist;
    float3 spotDir  = normalize(light.DirectionWS.xyz);
    float  cosAngle = dot(-L, spotDir);
    
    if (cosAngle < light.SpotOuterCosine) return float3(0, 0, 0);
    
    float spotFactor = saturate(
        (cosAngle - light.SpotOuterCosine) /
        (light.SpotInnerCosine - light.SpotOuterCosine + 0.0001f));
    spotFactor = spotFactor * spotFactor;  // квадрат для мягкого fade

    float  att = ComputeAttenuation(dist, light.Range);
    float3 col = BlinnPhong(N, L, V, albedo, roughness, light.Color.rgb * light.Color.w);
    return col * att * spotFactor;
}

float4 PSMain(FSQOutput input) : SV_TARGET
{
    float3 albedo    = gAlbedo.Sample(gSampler, input.UV).rgb;
    float3 normal    = gNormal.Sample(gSampler, input.UV).rgb;
    float4 pbrData   = gPBR.Sample(gSampler, input.UV);
    float  roughness = pbrData.r;
    float  metallic  = pbrData.g;
    float  ao        = pbrData.b;
    
    float normalLen = dot(normal, normal);
    if (normalLen < 0.01f)
    {
        // Фоновый цвет — тёмно-синий
        return float4(0.02f, 0.02f, 0.05f, 1.0f);
    }

    float3 N = normalize(normal);
    
    
    float3 worldPos = gWorldPos.Sample(gSampler, input.UV).rgb;

    float3 V = normalize(CameraPos.xyz - worldPos);
    
    float3 ambient = albedo * 0.08f * ao;
    
    float3 lighting = float3(0, 0, 0);

    for (uint i = 0; i < LightCount; ++i)
    {
        LightData light = Lights[i];

        if (light.Type == LIGHT_DIRECTIONAL)
            lighting += CalcDirectional(light, N, V, albedo, roughness);
        else if (light.Type == LIGHT_POINT)
            lighting += CalcPoint(light, worldPos, N, V, albedo, roughness);
        else if (light.Type == LIGHT_SPOT)
            lighting += CalcSpot(light, worldPos, N, V, albedo, roughness);
    }

    float3 finalColor = ambient + lighting;

    // Tone mapping (Reinhard) — защита от пересветов
    finalColor = finalColor / (finalColor + 1.0f);

    return float4(finalColor, 1.0f);
}
