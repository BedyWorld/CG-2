cbuffer GeometryCB : register(b0)
{
    matrix   World;
    matrix   View;
    matrix   Proj;
    float4   CameraPos;
    float2   Tiling;
    float2   UVOffset;
    float    BlendFactor;
    float    TessNear;
    float    TessFar;
    float    TessMinLevel;
    float    TessMaxLevel;
    float    DisplacementScale;
    float2   CBPad;
};

Texture2D    gAlbedo1    : register(t0);
Texture2D    gAlbedo2    : register(t1);
Texture2D    gNormalMap  : register(t2);
Texture2D    gDispMap    : register(t3);
SamplerState gSampler    : register(s0);
SamplerState gSamplerDisp: register(s1);

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
    o.TexCoord = input.TexCoord;
    return o;
}

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

//  Domain Shader
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
    
    float disp = gDispMap.SampleLevel(gSamplerDisp, uv, 0).r;
    posWS += normalWS * ((disp - 0.5f) * 2.0f * DisplacementScale);

    o.PosWS    = posWS;
    o.Normal   = normalWS;
    o.TexCoord = uv;
    o.Pos      = mul(mul(float4(posWS, 1.0f), View), Proj);
    return o;
}

struct PSOutput
{
    float4 Albedo   : SV_Target0;
    float4 Normal   : SV_Target1;
    float4 PBR      : SV_Target2;
    float4 WorldPos : SV_Target3;
};

PSOutput PSMain(DSOutput input)
{
    PSOutput o;
    
    float2 uvAnim = input.TexCoord + UVOffset;

    float4 tex1 = gAlbedo1.Sample(gSampler, uvAnim);
    float4 tex2 = gAlbedo2.Sample(gSampler, uvAnim);
    o.Albedo    = float4(lerp(tex1.rgb, tex2.rgb, BlendFactor), 1.0f);

    float3 N   = normalize(input.Normal);
    float3 dp1  = ddx(input.PosWS);
    float3 dp2  = ddy(input.PosWS);
    float2 duv1 = ddx(input.TexCoord);
    float2 duv2 = ddy(input.TexCoord);

    float3 Traw  = dp1 * duv2.y - dp2 * duv1.y;
    float  Tlen2 = dot(Traw, Traw);
    float3 T, B;
    if (Tlen2 > 1e-12f)
    {
        T = normalize(Traw - N * dot(N, Traw * rsqrt(Tlen2)));
        B = cross(N, T);
    }
    else
    {
        float3 up = abs(N.y) < 0.999f ? float3(0, 1, 0) : float3(1, 0, 0);
        T = normalize(cross(up, N));
        B = cross(N, T);
    }
    float3x3 TBN = float3x3(T, B, N);

    float3 nmSample = gNormalMap.Sample(gSampler, input.TexCoord).rgb;
    float3 nmLocal  = nmSample * 2.0f - 1.0f;
    float3 nmWorld  = normalize(mul(nmLocal, TBN));

    o.Normal   = float4(nmWorld, 0.0f);
    o.PBR      = float4(0.6f, 0.1f, 1.0f, 1.0f);
    o.WorldPos = float4(input.PosWS, 1.0f);

    return o;
}
