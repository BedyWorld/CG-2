#include "RenderingSystem.h"
#include <cassert>
#include <cmath>

RenderingSystem::RenderingSystem(HWND hwnd, int width, int height)
    : hwnd_(hwnd), width_(width), height_(height)
{
    for (UINT i = 0; i < RS_FRAME_COUNT; ++i)
        fenceValues_[i] = 0;
}

RenderingSystem::~RenderingSystem()
{
    if (commandQueue_ && fence_ && fenceEvent_)
    {
        try { WaitForGPU(); }
        catch (...) {}
    }
    if (geometryCB_ && geometryCBMapped_)
        geometryCB_->Unmap(0, nullptr);
    if (lightingCBRes_ && lightingCBMapped_)
        lightingCBRes_->Unmap(0, nullptr);
    if (fenceEvent_)
        CloseHandle(fenceEvent_);
}

void RenderingSystem::Initialize()
{
    CreateDevice();
    CreateCommandQueue();
    CreateSwapChain();
    CreateDescriptorHeaps();
    CreateRenderTargetViews();
    CreateDepthStencilBuffer();
    CreateCommandObjects();
    CreateFence();
    CreateGeometryPassRootSignature();
    CreateLightingPassRootSignature();
    CreateGeometryPassPSO();
    CreateLightingPassPSO();
}

void RenderingSystem::BeginResourceUpload()
{
    ThrowIfFailed(commandAllocators_[0]->Reset());
    ThrowIfFailed(commandList_->Reset(commandAllocators_[0].Get(), nullptr));
}

void RenderingSystem::EndResourceUpload()
{
    ThrowIfFailed(commandList_->Close());
    ID3D12CommandList* lists[] = { commandList_.Get() };
    commandQueue_->ExecuteCommandLists(1, lists);
    WaitForGPU();
}

void RenderingSystem::ReleaseTextureUploadBuffers()
{
    textureUpload_.Reset();
    textureUpload2_.Reset();
}

void RenderingSystem::CreateDevice()
{
#ifdef _DEBUG
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
        debug->EnableDebugLayer();
#endif
    ComPtr<IDXGIFactory6> factory;
    ThrowIfFailed(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0;
         factory->EnumAdapterByGpuPreference(
             i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
             IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(),
            D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_))))
            return;
    }
    ThrowIfFailed(E_FAIL, "No D3D12 device found");
}

void RenderingSystem::CreateCommandQueue()
{
    D3D12_COMMAND_QUEUE_DESC desc = {};
    desc.Type  = D3D12_COMMAND_LIST_TYPE_DIRECT;
    desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    ThrowIfFailed(device_->CreateCommandQueue(&desc, IID_PPV_ARGS(&commandQueue_)));
}

void RenderingSystem::CreateSwapChain()
{
    ComPtr<IDXGIFactory4> factory;
    ThrowIfFailed(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.BufferCount = RS_FRAME_COUNT;
    desc.Width       = static_cast<UINT>(width_);
    desc.Height      = static_cast<UINT>(height_);
    desc.Format      = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.SampleDesc  = { 1, 0 };
    ComPtr<IDXGISwapChain1> sc1;
    ThrowIfFailed(factory->CreateSwapChainForHwnd(
        commandQueue_.Get(), hwnd_, &desc, nullptr, nullptr, &sc1));
    factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
    ThrowIfFailed(sc1.As(&swapChain_));
    frameIndex_ = swapChain_->GetCurrentBackBufferIndex();
}

// RTV: 2 swapchain + 3 GBuffer = 5 total
// SRV: slot0-1 textures, slot4-6 GBuffer = 8 total
void RenderingSystem::CreateDescriptorHeaps()
{
    {
        D3D12_DESCRIPTOR_HEAP_DESC d = {};
        d.NumDescriptors = RS_FRAME_COUNT + GBUFFER_RT_COUNT;
        d.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        ThrowIfFailed(device_->CreateDescriptorHeap(&d, IID_PPV_ARGS(&rtvHeap_)));
        rtvDescSize_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }
    {
        D3D12_DESCRIPTOR_HEAP_DESC d = {};
        d.NumDescriptors = 1;
        d.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        ThrowIfFailed(device_->CreateDescriptorHeap(&d, IID_PPV_ARGS(&dsvHeap_)));
    }
    {
        D3D12_DESCRIPTOR_HEAP_DESC d = {};
        d.NumDescriptors = 8;
        d.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        d.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(device_->CreateDescriptorHeap(&d, IID_PPV_ARGS(&srvHeap_)));
        srvDescSize_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
}

void RenderingSystem::CreateRenderTargetViews()
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < RS_FRAME_COUNT; ++i)
    {
        ThrowIfFailed(swapChain_->GetBuffer(i, IID_PPV_ARGS(&renderTargets_[i])));
        device_->CreateRenderTargetView(renderTargets_[i].Get(), nullptr, handle);
        rtvHandles_[i] = handle;
        handle.ptr += rtvDescSize_;
    }
}

void RenderingSystem::CreateDepthStencilBuffer()
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width            = static_cast<UINT>(width_);
    desc.Height           = static_cast<UINT>(height_);
    desc.DepthOrArraySize = 1;
    desc.MipLevels        = 1;
    desc.Format           = DXGI_FORMAT_D32_FLOAT;
    desc.SampleDesc       = { 1, 0 };
    desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE clear = {};
    clear.Format = DXGI_FORMAT_D32_FLOAT;
    clear.DepthStencil.Depth = 1.0f;
    ThrowIfFailed(device_->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE,
        &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &clear, IID_PPV_ARGS(&depthStencil_)));
    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format        = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsvHandle_ = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
    device_->CreateDepthStencilView(depthStencil_.Get(), &dsvDesc, dsvHandle_);

    // GBuffer RTV slots 2,3,4 and SRV slots 4,5,6
    D3D12_CPU_DESCRIPTOR_HANDLE gbufRtvStart = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    gbufRtvStart.ptr += static_cast<SIZE_T>(RS_FRAME_COUNT) * rtvDescSize_;
    gbuffer_.Create(device_.Get(), width_, height_,
                    rtvDescSize_, gbufRtvStart,
                    srvHeap_.Get(), 4, dsvHandle_);
}

void RenderingSystem::CreateCommandObjects()
{
    for (UINT i = 0; i < RS_FRAME_COUNT; ++i)
        ThrowIfFailed(device_->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&commandAllocators_[i])));
    ThrowIfFailed(device_->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        commandAllocators_[0].Get(), nullptr,
        IID_PPV_ARGS(&commandList_)));
    commandList_->Close();
}

void RenderingSystem::CreateFence()
{
    ThrowIfFailed(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)));
    fenceValues_[0] = 1;
    fenceValues_[1] = 1;
    fenceEvent_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!fenceEvent_) ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
}

// Geometry Pass Root Signature:
// param[0] = CBV b0 (ConstantBufferData)
// param[1] = SRV table t0..t1 (albedo textures, heap slots 0-1)
void RenderingSystem::CreateGeometryPassRootSignature()
{
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_DESCRIPTOR_RANGE texRange = {};
    texRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    texRange.NumDescriptors     = 2;
    texRange.BaseShaderRegister = 0;
    params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges   = &texRange;
    params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter   = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MaxLOD   = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister   = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters     = 2;
    desc.pParameters       = params;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers   = &sampler;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> sig, err;
    ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err));
    ThrowIfFailed(device_->CreateRootSignature(0, sig->GetBufferPointer(),
        sig->GetBufferSize(), IID_PPV_ARGS(&geometryRootSig_)));
}

// Lighting Pass Root Signature:
// param[0] = CBV b0 (LightingPassCB)
// param[1] = SRV table t0..t2 in shader (heap slots 4-6 = GBuffer)
void RenderingSystem::CreateLightingPassRootSignature()
{
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_DESCRIPTOR_RANGE gbufRange = {};
    gbufRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    gbufRange.NumDescriptors     = 3;
    gbufRange.BaseShaderRegister = 0;
    params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges   = &gbufRange;
    params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter   = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD   = 0.0f;
    sampler.ShaderRegister   = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters     = 2;
    desc.pParameters       = params;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers   = &sampler;
    desc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> sig, err;
    ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err));
    ThrowIfFailed(device_->CreateRootSignature(0, sig->GetBufferPointer(),
        sig->GetBufferSize(), IID_PPV_ARGS(&lightingRootSig_)));
}

// CompileShaderFromSource — компилирует из строки в памяти (нет зависимости от .hlsl файлов)
ComPtr<ID3DBlob> RenderingSystem::CompileShader(const std::string& source,
    const std::string& entry, const std::string& target)
{
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
    ComPtr<ID3DBlob> blob, errors;
    HRESULT hr = D3DCompile(
        source.c_str(), source.size(),
        nullptr, nullptr, nullptr,
        entry.c_str(), target.c_str(), flags, 0,
        &blob, &errors);
    if (FAILED(hr))
    {
        if (errors) MessageBoxA(hwnd_,
            static_cast<char*>(errors->GetBufferPointer()), "Shader Compile Error", MB_OK|MB_ICONERROR);
        ThrowIfFailed(hr, "CompileShader failed");
    }
    return blob;
}

// ---- Embedded GBuffer shader source ----
static const char* kGBufferShaderSrc = R"HLSL(
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

Texture2D    gAlbedo1 : register(t0);
Texture2D    gAlbedo2 : register(t1);
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

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    float4 worldPos  = mul(float4(input.Pos, 1.0f), World);
    output.Pos       = mul(mul(worldPos, View), Proj);
    output.WorldPos  = worldPos.xyz;
    output.Normal    = normalize(mul(float4(input.Normal, 0.0f), World).xyz);
    output.TexCoord  = input.TexCoord * Tiling + UVOffset;
    return output;
}

struct PSOutput
{
    float4 Albedo : SV_Target0;
    float4 Normal : SV_Target1;
    float4 PBR    : SV_Target2;
};

PSOutput PSMain(VSOutput input)
{
    PSOutput output;
    float4 tex1    = gAlbedo1.Sample(gSampler, input.TexCoord);
    float4 tex2    = gAlbedo2.Sample(gSampler, input.TexCoord);
    output.Albedo  = float4(lerp(tex1.rgb, tex2.rgb, BlendFactor), 1.0f);
    float3 N       = normalize(input.Normal);
    output.Normal  = float4(N, 0.0f);
    output.PBR     = float4(0.6f, 0.1f, 1.0f, 1.0f);
    return output;
}
)HLSL";

void RenderingSystem::CreateGeometryPassPSO()
{
    auto vs = CompileShader(kGBufferShaderSrc,"VSMain","vs_5_0");
    auto ps = CompileShader(kGBufferShaderSrc,"PSMain","ps_5_0");

    D3D12_INPUT_ELEMENT_DESC layout[]=
    {
        {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,   0, 0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"NORMAL",  0,DXGI_FORMAT_R32G32B32_FLOAT,   0,12,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"COLOR",   0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,24,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,      0,40,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
    };
    D3D12_RASTERIZER_DESC raster={};
    raster.FillMode=D3D12_FILL_MODE_SOLID;
    raster.CullMode=D3D12_CULL_MODE_BACK;
    raster.DepthClipEnable=TRUE;
    D3D12_DEPTH_STENCIL_DESC ds={};
    ds.DepthEnable=TRUE;
    ds.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc=D3D12_COMPARISON_FUNC_LESS;
    D3D12_BLEND_DESC blend={};
    for(UINT i=0;i<GBUFFER_RT_COUNT;++i)
        blend.RenderTarget[i].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc={};
    psoDesc.pRootSignature=geometryRootSig_.Get();
    psoDesc.VS={vs->GetBufferPointer(),vs->GetBufferSize()};
    psoDesc.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
    psoDesc.InputLayout={layout,_countof(layout)};
    psoDesc.RasterizerState=raster;
    psoDesc.DepthStencilState=ds;
    psoDesc.BlendState=blend;
    psoDesc.SampleMask=UINT_MAX;
    psoDesc.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets=GBUFFER_RT_COUNT;
    psoDesc.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.RTVFormats[1]=DXGI_FORMAT_R16G16B16A16_FLOAT;
    psoDesc.RTVFormats[2]=DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.DSVFormat=DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc={1,0};
    ThrowIfFailed(device_->CreateGraphicsPipelineState(&psoDesc,IID_PPV_ARGS(&geometryPSO_)));
}

// ---- Embedded Lighting shader source ----
static const char* kLightingShaderSrc = R"HLSL(
#define MAX_LIGHTS 16
#define LIGHT_DIRECTIONAL 0
#define LIGHT_POINT       1
#define LIGHT_SPOT        2

struct LightData
{
    float4 PositionWS;
    float4 DirectionWS;
    float4 Color;
    float  Range;
    float  SpotInnerCosine;
    float  SpotOuterCosine;
    uint   Type;
};

cbuffer LightingCB : register(b0)
{
    float4    CameraPos;
    int       LightCount;
    float3    LCBPad;
    LightData Lights[MAX_LIGHTS];
};

Texture2D    gAlbedo  : register(t0);
Texture2D    gNormal  : register(t1);
Texture2D    gPBR     : register(t2);
SamplerState gSampler : register(s0);

struct FSQOutput
{
    float4 Pos : SV_POSITION;
    float2 UV  : TEXCOORD0;
};

FSQOutput VSMain(uint vid : SV_VertexID)
{
    FSQOutput o;
    float2 pos = float2(0, 0);
    float2 uv  = float2(0, 0);
    if      (vid == 0u) { pos = float2(-1.0f, -1.0f); uv = float2(0.0f, 1.0f); }
    else if (vid == 1u) { pos = float2(-1.0f,  1.0f); uv = float2(0.0f, 0.0f); }
    else if (vid == 2u) { pos = float2( 1.0f,  1.0f); uv = float2(1.0f, 0.0f); }
    else if (vid == 3u) { pos = float2(-1.0f, -1.0f); uv = float2(0.0f, 1.0f); }
    else if (vid == 4u) { pos = float2( 1.0f,  1.0f); uv = float2(1.0f, 0.0f); }
    else                { pos = float2( 1.0f, -1.0f); uv = float2(1.0f, 1.0f); }
    o.Pos = float4(pos, 0.0f, 1.0f);
    o.UV  = uv;
    return o;
}

float ComputeAttenuation(float dist, float range)
{
    float falloff = saturate(1.0f - (dist / range));
    return (falloff * falloff) / (dist * dist + 1.0f);
}

float3 BlinnPhong(float3 N, float3 L, float3 V,
                  float3 albedo, float roughness, float3 lightColor)
{
    float  diff     = max(dot(N, L), 0.0f);
    float3 H        = normalize(L + V);
    float  shininess= max(2.0f / (roughness * roughness + 0.001f) - 2.0f, 1.0f);
    float  spec     = pow(max(dot(N, H), 0.0f), shininess);
    float3 diffuse  = albedo * diff * lightColor;
    float3 specular = spec * lightColor * (1.0f - roughness);
    return diffuse + specular;
}

float3 CalcDirectional(LightData l, float3 N, float3 V, float3 alb, float rgh)
{
    float3 L = normalize(-l.DirectionWS.xyz);
    return BlinnPhong(N, L, V, alb, rgh, l.Color.rgb * l.Color.w);
}

float3 CalcPoint(LightData l, float3 wp, float3 N, float3 V, float3 alb, float rgh)
{
    float3 toLight = l.PositionWS.xyz - wp;
    float  d       = length(toLight);
    if (d >= l.Range) return float3(0.0f, 0.0f, 0.0f);
    float3 L   = toLight / d;
    float  att = ComputeAttenuation(d, l.Range);
    return BlinnPhong(N, L, V, alb, rgh, l.Color.rgb * l.Color.w) * att;
}

float3 CalcSpot(LightData l, float3 wp, float3 N, float3 V, float3 alb, float rgh)
{
    float3 toLight  = l.PositionWS.xyz - wp;
    float  d        = length(toLight);
    if (d >= l.Range) return float3(0.0f, 0.0f, 0.0f);
    float3 L        = toLight / d;
    float3 spotDir  = normalize(l.DirectionWS.xyz);
    float  cosAngle = dot(-L, spotDir);
    if (cosAngle < l.SpotOuterCosine) return float3(0.0f, 0.0f, 0.0f);
    float sf = saturate((cosAngle - l.SpotOuterCosine) /
                        (l.SpotInnerCosine - l.SpotOuterCosine + 0.0001f));
    sf = sf * sf;
    float att = ComputeAttenuation(d, l.Range);
    return BlinnPhong(N, L, V, alb, rgh, l.Color.rgb * l.Color.w) * att * sf;
}

float4 PSMain(FSQOutput input) : SV_TARGET
{
    float3 albedo   = gAlbedo.Sample(gSampler, input.UV).rgb;
    float3 normal   = gNormal.Sample(gSampler, input.UV).rgb;
    float4 pbrSample= gPBR.Sample(gSampler, input.UV);
    float  roughness= pbrSample.r;
    float  ao       = pbrSample.b;

    if (dot(normal, normal) < 0.01f)
        return float4(0.02f, 0.02f, 0.05f, 1.0f);

    float3 N        = normalize(normal);
    float3 worldPos = float3(
        (input.UV.x * 2.0f - 1.0f) * 5.0f,
        (1.0f - input.UV.y * 2.0f) * 5.0f,
        0.0f);
    float3 V        = normalize(CameraPos.xyz - worldPos);
    float3 color    = albedo * 0.08f * ao;

    int count = LightCount;
    for (int i = 0; i < count; ++i)
    {
        LightData li = Lights[i];
        if      (li.Type == LIGHT_DIRECTIONAL) color += CalcDirectional(li, N, V, albedo, roughness);
        else if (li.Type == LIGHT_POINT)       color += CalcPoint(li, worldPos, N, V, albedo, roughness);
        else if (li.Type == LIGHT_SPOT)        color += CalcSpot(li, worldPos, N, V, albedo, roughness);
    }

    color = color / (color + 1.0f);
    return float4(color, 1.0f);
}
)HLSL";

void RenderingSystem::CreateLightingPassPSO()
{
    auto vs = CompileShader(kLightingShaderSrc,"VSMain","vs_5_0");
    auto ps = CompileShader(kLightingShaderSrc,"PSMain","ps_5_0");

    D3D12_RASTERIZER_DESC raster={};
    raster.FillMode=D3D12_FILL_MODE_SOLID;
    raster.CullMode=D3D12_CULL_MODE_NONE;
    D3D12_DEPTH_STENCIL_DESC ds={};
    ds.DepthEnable=FALSE;
    ds.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;
    D3D12_BLEND_DESC blend={};
    blend.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc={};
    psoDesc.pRootSignature=lightingRootSig_.Get();
    psoDesc.VS={vs->GetBufferPointer(),vs->GetBufferSize()};
    psoDesc.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
    psoDesc.InputLayout={nullptr,0};
    psoDesc.RasterizerState=raster;
    psoDesc.DepthStencilState=ds;
    psoDesc.BlendState=blend;
    psoDesc.SampleMask=UINT_MAX;
    psoDesc.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets=1;
    psoDesc.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.DSVFormat=DXGI_FORMAT_UNKNOWN;
    psoDesc.SampleDesc={1,0};
    ThrowIfFailed(device_->CreateGraphicsPipelineState(&psoDesc,IID_PPV_ARGS(&lightingPSO_)));
}

void RenderingSystem::UploadTexture(const TextureData& td, int slot)
{
    if(!td.valid||td.width==0||td.height==0)
        ThrowIfFailed(E_FAIL,"Invalid TextureData");
    ComPtr<ID3D12Resource>& texRes=(slot==0)?texture_:texture2_;
    ComPtr<ID3D12Resource>& uploadRes=(slot==0)?textureUpload_:textureUpload2_;
    D3D12_HEAP_PROPERTIES defHP={};
    defHP.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC texDesc={};
    texDesc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width=static_cast<UINT>(td.width);
    texDesc.Height=static_cast<UINT>(td.height);
    texDesc.DepthOrArraySize=1;
    texDesc.MipLevels=1;
    texDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc={1,0};
    ThrowIfFailed(device_->CreateCommittedResource(
        &defHP,D3D12_HEAP_FLAG_NONE,&texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&texRes)));
    UINT64 uploadSize=0;
    device_->GetCopyableFootprints(&texDesc,0,1,0,nullptr,nullptr,nullptr,&uploadSize);
    D3D12_HEAP_PROPERTIES upHP={};
    upHP.Type=D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC upDesc={};
    upDesc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    upDesc.Width=uploadSize;
    upDesc.Height=1;
    upDesc.DepthOrArraySize=1;
    upDesc.MipLevels=1;
    upDesc.Format=DXGI_FORMAT_UNKNOWN;
    upDesc.SampleDesc={1,0};
    upDesc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ThrowIfFailed(device_->CreateCommittedResource(
        &upHP,D3D12_HEAP_FLAG_NONE,&upDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&uploadRes)));
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint={};
    device_->GetCopyableFootprints(&texDesc,0,1,0,&footprint,nullptr,nullptr,nullptr);
    BYTE* mapped=nullptr;
    ThrowIfFailed(uploadRes->Map(0,nullptr,reinterpret_cast<void**>(&mapped)));
    UINT rowPitch=footprint.Footprint.RowPitch;
    UINT srcPitch=td.width*4;
    for(UINT row=0;row<static_cast<UINT>(td.height);++row)
        memcpy(mapped+row*rowPitch,td.pixels.data()+row*srcPitch,srcPitch);
    uploadRes->Unmap(0,nullptr);
    D3D12_TEXTURE_COPY_LOCATION dst={};
    dst.pResource=texRes.Get();
    dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src={};
    src.pResource=uploadRes.Get();
    src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint=footprint;
    commandList_->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    D3D12_RESOURCE_BARRIER barrier={};
    barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource=texRes.Get();
    barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    commandList_->ResourceBarrier(1,&barrier);
    D3D12_CPU_DESCRIPTOR_HANDLE srvHandle=srvHeap_->GetCPUDescriptorHandleForHeapStart();
    srvHandle.ptr+=static_cast<SIZE_T>(slot)*srvDescSize_;
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc={};
    srvDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels=1;
    device_->CreateShaderResourceView(texRes.Get(),&srvDesc,srvHandle);
}

void RenderingSystem::BuildBuffers(const std::vector<Vertex>& verts, const std::vector<UINT>& idxs)
{
    indexCount_=static_cast<UINT>(idxs.size());
    MakeUploadBuffer(verts.data(),verts.size()*sizeof(Vertex),vertexBuffer_);
    vbView_.BufferLocation=vertexBuffer_->GetGPUVirtualAddress();
    vbView_.SizeInBytes=static_cast<UINT>(verts.size()*sizeof(Vertex));
    vbView_.StrideInBytes=sizeof(Vertex);
    MakeUploadBuffer(idxs.data(),idxs.size()*sizeof(UINT),indexBuffer_);
    ibView_.BufferLocation=indexBuffer_->GetGPUVirtualAddress();
    ibView_.SizeInBytes=static_cast<UINT>(idxs.size()*sizeof(UINT));
    ibView_.Format=DXGI_FORMAT_R32_UINT;
}

void RenderingSystem::MakeUploadBuffer(const void* data, UINT64 byteSize, ComPtr<ID3D12Resource>& buf)
{
    D3D12_HEAP_PROPERTIES hp={};
    hp.Type=D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc={};
    desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width=byteSize;
    desc.Height=1;
    desc.DepthOrArraySize=1;
    desc.MipLevels=1;
    desc.Format=DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc={1,0};
    desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ThrowIfFailed(device_->CreateCommittedResource(
        &hp,D3D12_HEAP_FLAG_NONE,&desc,
        D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&buf)));
    BYTE* mapped=nullptr;
    ThrowIfFailed(buf->Map(0,nullptr,reinterpret_cast<void**>(&mapped)));
    memcpy(mapped,data,static_cast<size_t>(byteSize));
    buf->Unmap(0,nullptr);
}

void RenderingSystem::CreateConstantBuffers()
{
    auto Align256=[](UINT64 n){return (n+255)&~UINT64(255);};
    D3D12_HEAP_PROPERTIES hp={};
    hp.Type=D3D12_HEAP_TYPE_UPLOAD;
    auto makeBuffer=[&](UINT64 size, ComPtr<ID3D12Resource>& res)
    {
        D3D12_RESOURCE_DESC desc={};
        desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width=Align256(size);
        desc.Height=1;
        desc.DepthOrArraySize=1;
        desc.MipLevels=1;
        desc.Format=DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc={1,0};
        desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ThrowIfFailed(device_->CreateCommittedResource(
            &hp,D3D12_HEAP_FLAG_NONE,&desc,
            D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&res)));
    };
    makeBuffer(sizeof(ConstantBufferData),geometryCB_);
    ThrowIfFailed(geometryCB_->Map(0,nullptr,reinterpret_cast<void**>(&geometryCBMapped_)));
    makeBuffer(sizeof(LightingPassCB),lightingCBRes_);
    ThrowIfFailed(lightingCBRes_->Map(0,nullptr,reinterpret_cast<void**>(&lightingCBMapped_)));
}

void RenderingSystem::UpdateGeometryPassCB(const ConstantBufferData& data)
{
    if(geometryCBMapped_) *geometryCBMapped_=data;
}

void RenderingSystem::UpdateLightingPassCB(const LightingPassCB& data)
{
    lightingCB_=data;
    if(lightingCBMapped_) *lightingCBMapped_=data;
}

void RenderingSystem::ClearLights()
{
    lightingCB_.LightCount=0;
}

void RenderingSystem::AddDirectionalLight(XMFLOAT3 direction, XMFLOAT3 color, float intensity)
{
    if(lightingCB_.LightCount>=MAX_LIGHTS) return;
    LightData& l=lightingCB_.Lights[lightingCB_.LightCount++];
    l.PositionWS={0,0,0,0};
    l.DirectionWS={direction.x,direction.y,direction.z,0};
    l.Color={color.x,color.y,color.z,intensity};
    l.Range=0.f; l.SpotInnerCosine=0.f; l.SpotOuterCosine=0.f;
    l.Type=static_cast<UINT>(LightType::Directional);
}

void RenderingSystem::AddPointLight(XMFLOAT3 position, XMFLOAT3 color, float intensity, float range)
{
    if(lightingCB_.LightCount>=MAX_LIGHTS) return;
    LightData& l=lightingCB_.Lights[lightingCB_.LightCount++];
    l.PositionWS={position.x,position.y,position.z,1};
    l.DirectionWS={0,0,0,0};
    l.Color={color.x,color.y,color.z,intensity};
    l.Range=range; l.SpotInnerCosine=0.f; l.SpotOuterCosine=0.f;
    l.Type=static_cast<UINT>(LightType::Point);
}

void RenderingSystem::AddSpotLight(XMFLOAT3 position, XMFLOAT3 direction,
    XMFLOAT3 color, float intensity, float range,
    float innerAngleDeg, float outerAngleDeg)
{
    if(lightingCB_.LightCount>=MAX_LIGHTS) return;
    const float D2R=3.14159265f/180.f;
    LightData& l=lightingCB_.Lights[lightingCB_.LightCount++];
    l.PositionWS={position.x,position.y,position.z,1};
    l.DirectionWS={direction.x,direction.y,direction.z,0};
    l.Color={color.x,color.y,color.z,intensity};
    l.Range=range;
    l.SpotInnerCosine=cosf(innerAngleDeg*D2R);
    l.SpotOuterCosine=cosf(outerAngleDeg*D2R);
    l.Type=static_cast<UINT>(LightType::Spot);
}

// --- BeginFrame: reset, GBuffer SRV->RT, clear, set GBuffer as output ---
ID3D12GraphicsCommandList* RenderingSystem::BeginFrame()
{
    ThrowIfFailed(commandAllocators_[frameIndex_]->Reset());
    ThrowIfFailed(commandList_->Reset(commandAllocators_[frameIndex_].Get(),nullptr));

    gbuffer_.TransitionTo(commandList_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
    gbuffer_.Clear(commandList_.Get());
    commandList_->ClearDepthStencilView(dsvHandle_,D3D12_CLEAR_FLAG_DEPTH,1.0f,0,0,nullptr);
    commandList_->OMSetRenderTargets(GBUFFER_RT_COUNT,gbuffer_.GetRTVHandles(),FALSE,&dsvHandle_);

    D3D12_VIEWPORT vp={0,0,static_cast<float>(width_),static_cast<float>(height_),0.f,1.f};
    D3D12_RECT     sr={0,0,width_,height_};
    commandList_->RSSetViewports(1,&vp);
    commandList_->RSSetScissorRects(1,&sr);
    return commandList_.Get();
}

void RenderingSystem::BindGeometryPass()
{
    commandList_->SetGraphicsRootSignature(geometryRootSig_.Get());
    commandList_->SetPipelineState(geometryPSO_.Get());
    ID3D12DescriptorHeap* heaps[]={srvHeap_.Get()};
    commandList_->SetDescriptorHeaps(1,heaps);
    commandList_->SetGraphicsRootConstantBufferView(0,geometryCB_->GetGPUVirtualAddress());
    commandList_->SetGraphicsRootDescriptorTable(1,srvHeap_->GetGPUDescriptorHandleForHeapStart());
    commandList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void RenderingSystem::DrawMesh()
{
    commandList_->IASetVertexBuffers(0,1,&vbView_);
    commandList_->IASetIndexBuffer(&ibView_);
    commandList_->DrawIndexedInstanced(indexCount_,1,0,0,0);
}

// --- BeginLightingPass: GBuffer RT->SRV, swap chain Present->RT, bind lighting ---
void RenderingSystem::BeginLightingPass()
{
    gbuffer_.TransitionTo(commandList_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    D3D12_RESOURCE_BARRIER barrier={};
    barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource=renderTargets_[frameIndex_].Get();
    barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET;
    commandList_->ResourceBarrier(1,&barrier);

    const float cc[]={0.f,0.f,0.f,1.f};
    commandList_->ClearRenderTargetView(rtvHandles_[frameIndex_],cc,0,nullptr);
    commandList_->OMSetRenderTargets(1,&rtvHandles_[frameIndex_],FALSE,nullptr);

    if(lightingCBMapped_) *lightingCBMapped_=lightingCB_;

    commandList_->SetGraphicsRootSignature(lightingRootSig_.Get());
    commandList_->SetPipelineState(lightingPSO_.Get());
    ID3D12DescriptorHeap* heaps[]={srvHeap_.Get()};
    commandList_->SetDescriptorHeaps(1,heaps);
    commandList_->SetGraphicsRootConstantBufferView(0,lightingCBRes_->GetGPUVirtualAddress());
    commandList_->SetGraphicsRootDescriptorTable(1,gbuffer_.GetSRVGpuStart());
    commandList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList_->IASetVertexBuffers(0,0,nullptr);
    commandList_->IASetIndexBuffer(nullptr);
}

void RenderingSystem::DrawLightingQuad()
{
    commandList_->DrawInstanced(6,1,0,0);
}

void RenderingSystem::EndFrame()
{
    D3D12_RESOURCE_BARRIER barrier={};
    barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource=renderTargets_[frameIndex_].Get();
    barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_PRESENT;
    commandList_->ResourceBarrier(1,&barrier);
    ThrowIfFailed(commandList_->Close());
    ID3D12CommandList* lists[]={commandList_.Get()};
    commandQueue_->ExecuteCommandLists(1,lists);
    ThrowIfFailed(swapChain_->Present(1,0));
    MoveToNextFrame();
}

void RenderingSystem::Resize(int width, int height)
{
    if(width==width_&&height==height_) return;
    WaitForGPU();
    width_=width; height_=height;
    for(auto& rt:renderTargets_) rt.Reset();
    depthStencil_.Reset();
    ThrowIfFailed(swapChain_->ResizeBuffers(
        RS_FRAME_COUNT,static_cast<UINT>(width),static_cast<UINT>(height),
        DXGI_FORMAT_R8G8B8A8_UNORM,0));
    frameIndex_=swapChain_->GetCurrentBackBufferIndex();
    CreateRenderTargetViews();
    CreateDepthStencilBuffer();
}

void RenderingSystem::WaitForGPU()
{
    ThrowIfFailed(commandQueue_->Signal(fence_.Get(),fenceValues_[frameIndex_]));
    ThrowIfFailed(fence_->SetEventOnCompletion(fenceValues_[frameIndex_],fenceEvent_));
    WaitForSingleObject(fenceEvent_,INFINITE);
    ++fenceValues_[frameIndex_];
}

void RenderingSystem::MoveToNextFrame()
{
    const UINT64 cur=fenceValues_[frameIndex_];
    ThrowIfFailed(commandQueue_->Signal(fence_.Get(),cur));
    frameIndex_=swapChain_->GetCurrentBackBufferIndex();
    if(fence_->GetCompletedValue()<fenceValues_[frameIndex_])
    {
        ThrowIfFailed(fence_->SetEventOnCompletion(fenceValues_[frameIndex_],fenceEvent_));
        WaitForSingleObject(fenceEvent_,INFINITE);
    }
    fenceValues_[frameIndex_]=cur+1;
}
