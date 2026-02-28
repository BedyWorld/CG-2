cbuffer CB : register(b0)
{
    matrix World;
    matrix View;
    matrix Proj;

    float4 LightPos;
    float4 LightColor;
    float4 CameraPos;

    float2 Tiling;
    float2 UVOffset;
};

Texture2D gTexture  : register(t0);
Texture2D gTexture2 : register(t1);
SamplerState gSampler : register(s0);


struct VSInput
{
    float3 Pos : POSITION;
    float3 Normal : NORMAL;
    float4 Color : COLOR;
    float2 TexCoord : TEXCOORD0;
};


struct PSInput
{
    float4 Pos : SV_POSITION;
    float3 WorldPos : TEXCOORD0;
    float3 Normal : TEXCOORD1;
    float4 Color : COLOR;
    float2 TexCoord : TEXCOORD2;
};


PSInput VSMain(VSInput input)
{
    PSInput output;

    float4 worldPos = mul(float4(input.Pos,1), World);
    float4 viewPos  = mul(worldPos, View);

    output.Pos = mul(viewPos, Proj);

    output.WorldPos = worldPos.xyz;
    output.Normal = mul(float4(input.Normal,0),World).xyz;

    output.Color = input.Color;

    // ТЕКСТУРА + СМЕЩЕНИЕ
    output.TexCoord = input.TexCoord * Tiling + UVOffset;

    return output;
}



float4 PSMain(PSInput input) : SV_TARGET
{
    float3 N = normalize(input.Normal);

    float3 L = normalize(LightPos.xyz - input.WorldPos);
    float3 V = normalize(CameraPos.xyz - input.WorldPos);
    float3 R = reflect(-L,N);

    float diff = max(dot(N,L),0);

    float spec = pow(max(dot(R,V),0),32);


    // ДВИГАЮЩАЯСЯ ТЕКСТУРА
    static const float CHECKER_COUNT = 8.0f;

    float2 scaled  = input.TexCoord * CHECKER_COUNT;

    int2   cell    = int2(floor(scaled));
    float2 localUV = frac(scaled);

    bool useSecond = ((cell.x + cell.y) & 1) != 0;

    float4 texColor = useSecond
        ? gTexture2.Sample(gSampler, localUV)
        : gTexture.Sample (gSampler, localUV);


    float3 ambient = texColor.rgb * 0.2;
    float3 diffuse = texColor.rgb * diff;
    float3 specular = spec * LightColor.rgb;

    float3 finalColor = ambient + diffuse + specular;

    return float4(finalColor,1);
}