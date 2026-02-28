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
    float BlendFactor; // НОВЫЙ ПАРАМЕТР: 0 = texture2, 1 = texture3
    float3 Padding; // выравнивание для 16-байтовой границы
};

Texture2D gTexture : register(t0); // texture2 (дальняя)
Texture2D gTexture2 : register(t1); // texture3 (ближняя)
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
    float DistanceToCamera : TEXCOORD3; // добавим расстояние для отладки
};


PSInput VSMain(VSInput input)
{
    PSInput output;

    float4 worldPos = mul(float4(input.Pos, 1), World);
    float4 viewPos = mul(worldPos, View);

    output.Pos = mul(viewPos, Proj);

    output.WorldPos = worldPos.xyz;
    output.Normal = mul(float4(input.Normal, 0), World).xyz;

    output.Color = input.Color;

    // ТЕКСТУРА + СМЕЩЕНИЕ
    output.TexCoord = input.TexCoord * Tiling + UVOffset;
    
    // Вычисляем расстояние от камеры до точки в мировых координатах
    float3 cameraToPoint = worldPos.xyz - CameraPos.xyz;
    output.DistanceToCamera = length(cameraToPoint);

    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float3 N = normalize(input.Normal);

    float3 L = normalize(LightPos.xyz - input.WorldPos);
    float3 V = normalize(CameraPos.xyz - input.WorldPos);
    float3 R = reflect(-L, N);

    float diff = max(dot(N, L), 0);
    float spec = pow(max(dot(R, V), 0), 32);

    // Сэмплируем обе текстуры с одинаковыми UV координатами
    float4 texColor1 = gTexture.Sample(gSampler, input.TexCoord); // texture2
    float4 texColor2 = gTexture2.Sample(gSampler, input.TexCoord); // texture3
    
    // Смешиваем текстуры на основе расстояния до камеры
    // BlendFactor приходит из C++ кода и вычисляется на основе расстояния
    float4 texColor = lerp(texColor1, texColor2, BlendFactor);
    
    // Для отладки: можно визуализировать расстояние цветом
    // Закомментируйте следующую строку, если не нужно
    // return float4(input.DistanceToCamera / 5.0f, 0, 1 - input.DistanceToCamera / 5.0f, 1);

    float3 ambient = texColor.rgb * 0.2;
    float3 diffuse = texColor.rgb * diff;
    float3 specular = spec * LightColor.rgb;

    float3 finalColor = ambient + diffuse + specular;

    return float4(finalColor, 1);
}