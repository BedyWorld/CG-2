// ================================================================
//  GBufferShaders.hlsl — Geometry Pass с тесселяцией и Normal Map
//
//  Записывает в GBuffer:
//    RT0 (R8G8B8A8_UNORM)      — Albedo.rgb (lerp двух текстур)
//    RT1 (R16G16B16A16_FLOAT)  — Normal в world space (из normal map)
//    RT2 (R8G8B8A8_UNORM)      — Roughness(R), Metallic(G), AO(B)
//
//  Стадии:
//    VS  — world-space transform, передаёт HSInput в Hull Shader
//    HS  — адаптивный TessFactor по расстоянию до камеры
//    DS  — барицентрическая интерполяция + displacement по нормали
//    PS  — normal map через TBN (ddx/ddy), MRT запись
//
//  Текстурные регистры:
//    t0 = gAlbedo1     (основная диффузная текстура)
//    t1 = gAlbedo2     (вторичная, blending по BlendFactor)
//    t2 = gNormalMap   (tangent-space normal map)
//    t3 = gDispMap     (displacement / height map, R-канал)
//
//  Сэмплеры:
//    s0 = wrap linear  (albedo, normal)
//    s1 = wrap linear  (displacement, SampleLevel в DS)
// ================================================================

cbuffer GeometryCB : register(b0)
{
    matrix   World;
    matrix   View;
    matrix   Proj;
    float4   CameraPos;
    float2   Tiling;
    float2   UVOffset;
    float    BlendFactor;
    float    TessNear;          // дистанция максимального уровня тесселяции
    float    TessFar;           // дистанция минимального уровня тесселяции
    float    TessMinLevel;      // мин. TessFactor (вдали)
    float    TessMaxLevel;      // макс. TessFactor (вблизи)
    float    DisplacementScale; // амплитуда смещения вдоль нормали
    float2   CBPad;
};

Texture2D    gAlbedo1    : register(t0);
Texture2D    gAlbedo2    : register(t1);
Texture2D    gNormalMap  : register(t2);
Texture2D    gDispMap    : register(t3);
SamplerState gSampler    : register(s0);
SamplerState gSamplerDisp: register(s1);

// ================================================================
//  Vertex Shader
//  Преобразует вершину в world-space и передаёт в HS.
//  Матрицы View/Proj применяются в DS после displacement.
// ================================================================
struct VSInput
{
    float3 Pos      : POSITION;
    float3 Normal   : NORMAL;
    float4 Color    : COLOR;
    float2 TexCoord : TEXCOORD0;
};

struct HSInput
{
    float3 PosWS    : POSITION;
    float3 Normal   : NORMAL;
    float2 TexCoord : TEXCOORD0;
};

HSInput VSMain(VSInput input)
{
    HSInput o;
    float4 wp  = mul(float4(input.Pos, 1.0f), World);
    o.PosWS    = wp.xyz;
    o.Normal   = normalize(mul(float4(input.Normal, 0.0f), World).xyz);
    // UV передаём без изменений — смещение применяется в PS при сэмплировании
    o.TexCoord = input.TexCoord;
    return o;
}

// ================================================================
//  Hull Shader
//  Константная функция патча вычисляет адаптивный TessFactor:
//    - Считаем расстояние от центра каждого ребра до камеры
//    - Линейно интерполируем между TessMaxLevel и TessMinLevel
//      по диапазону [TessNear, TessFar]
// ================================================================
struct HSConstOutput
{
    float EdgeTess[3]   : SV_TessFactor;
    float InsideTess[1] : SV_InsideTessFactor;
};

float DistanceTessFactor(float3 posWS)
{
    float dist = distance(posWS, CameraPos.xyz);
    float t    = saturate((dist - TessNear) / (TessFar - TessNear));
    return lerp(TessMaxLevel, TessMinLevel, t);
}

HSConstOutput HSConst(InputPatch<HSInput, 3> patch, uint pid : SV_PrimitiveID)
{
    HSConstOutput o;
    // TessFactor ребра = среднее двух угловых вершин
    o.EdgeTess[0]  = (DistanceTessFactor(patch[1].PosWS) + DistanceTessFactor(patch[2].PosWS)) * 0.5f;
    o.EdgeTess[1]  = (DistanceTessFactor(patch[2].PosWS) + DistanceTessFactor(patch[0].PosWS)) * 0.5f;
    o.EdgeTess[2]  = (DistanceTessFactor(patch[0].PosWS) + DistanceTessFactor(patch[1].PosWS)) * 0.5f;
    o.InsideTess[0]= (o.EdgeTess[0] + o.EdgeTess[1] + o.EdgeTess[2]) / 3.0f;
    return o;
}

[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("HSConst")]
[maxtessfactor(64.0f)]
HSInput HSMain(InputPatch<HSInput, 3> patch,
               uint i   : SV_OutputControlPointID,
               uint pid : SV_PrimitiveID)
{
    return patch[i];
}

// ================================================================
//  Domain Shader
//  1. Барицентрически интерполирует PosWS, Normal, TexCoord
//  2. Читает displacement map и смещает позицию вдоль нормали
//  3. Применяет View * Proj к финальной позиции
// ================================================================
struct DSOutput
{
    float4 Pos      : SV_POSITION;
    float3 PosWS    : TEXCOORD0;
    float3 Normal   : TEXCOORD1;
    float2 TexCoord : TEXCOORD2;
};

[domain("tri")]
DSOutput DSMain(HSConstOutput hsConst,
                float3 bary : SV_DomainLocation,
                const OutputPatch<HSInput, 3> patch)
{
    DSOutput o;

    // Барицентрическая интерполяция
    float3 posWS    = patch[0].PosWS    * bary.x + patch[1].PosWS    * bary.y + patch[2].PosWS    * bary.z;
    float3 normalWS = patch[0].Normal   * bary.x + patch[1].Normal   * bary.y + patch[2].Normal   * bary.z;
    float2 uv       = patch[0].TexCoord * bary.x + patch[1].TexCoord * bary.y + patch[2].TexCoord * bary.z;

    normalWS = normalize(normalWS);

    // Displacement: смещение вдоль нормали
    // SampleLevel(mip=0) обязателен в DS — градиентные инструкции недоступны
    float disp = gDispMap.SampleLevel(gSamplerDisp, uv, 0).r;
    posWS += normalWS * (disp * DisplacementScale);

    o.PosWS    = posWS;
    o.Normal   = normalWS;
    o.TexCoord = uv;
    o.Pos      = mul(mul(float4(posWS, 1.0f), View), Proj);
    return o;
}

// ================================================================
//  Pixel Shader — MRT GBuffer
//
//  Normal map:
//    - Строим TBN-фрейм из экранных производных (ddx/ddy)
//      позиции и UV — это точнее, чем передавать T/B из VS,
//      так как tessellation меняет геометрию
//    - Декодируем tangent-space нормаль из [0,1] → [-1,1]
//    - Трансформируем в world space через TBN
// ================================================================
struct PSOutput
{
    float4 Albedo : SV_Target0;
    float4 Normal : SV_Target1;
    float4 PBR    : SV_Target2;
};

PSOutput PSMain(DSOutput input)
{
    PSOutput o;

    // Смещение только при сэмплировании — UV модели не трогаем
    float2 uvAnim = input.TexCoord + UVOffset;

    float4 tex1 = gAlbedo1.Sample(gSampler, uvAnim);
    float4 tex2 = gAlbedo2.Sample(gSampler, uvAnim);
    o.Albedo    = float4(lerp(tex1.rgb, tex2.rgb, BlendFactor), 1.0f);

    float3 N   = normalize(input.Normal);
    float3 dp1  = ddx(input.PosWS);
    float3 dp2  = ddy(input.PosWS);
    float2 duv1 = ddx(input.TexCoord);
    float2 duv2 = ddy(input.TexCoord);

    float3 T = normalize(dp1 * duv2.y - dp2 * duv1.y);
    float3 B = normalize(cross(N, T));
    float3x3 TBN = float3x3(T, B, N);

    float3 nmSample = gNormalMap.Sample(gSampler, input.TexCoord).rgb;
    float3 nmLocal  = nmSample * 2.0f - 1.0f;
    float3 nmWorld  = normalize(mul(nmLocal, TBN));

    o.Normal = float4(nmWorld, 0.0f);
    o.PBR    = float4(0.6f, 0.1f, 1.0f, 1.0f);

    return o;
}
