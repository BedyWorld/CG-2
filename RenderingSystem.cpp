#include "RenderingSystem.h"
#include "TextureLoader.h"
#include <cassert>
#include <cmath>

RenderingSystem::RenderingSystem(HWND hwnd, int width, int height)
    : hwnd_(hwnd), width_(width), height_(height)
{
    for (UINT i = 0; i < RS_FRAME_COUNT; ++i)
        fenceValues_[i] = 0;
    // Слоты 0..3 зарезервированы под fallback + common текстуры
    // Слоты 4..7 — GBuffer (заполняет gbuffer_.Create)
    // Слоты 8+ — динамические текстуры
    srvNextFree_ = 8;
}

RenderingSystem::~RenderingSystem()
{
    if (commandQueue_ && fence_ && fenceEvent_)
        try { WaitForGPU(); }
    catch (...) {}
    if (geometryCB_ && geometryCBMapped_)  geometryCB_->Unmap(0, nullptr);
    if (geometryCB2_ && geometryCB2Mapped_) geometryCB2_->Unmap(0, nullptr);
    if (lightingCBRes_ && lightingCBMapped_)  lightingCBRes_->Unmap(0, nullptr);
    if (fenceEvent_) CloseHandle(fenceEvent_);
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

    // Загружаем зарезервированные fallback-текстуры в слоты 0..3
    auto UploadReserved = [&](const TextureData& td, int slot)
        {
            D3D12_HEAP_PROPERTIES defHP = {}; defHP.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC texDesc = {};
            texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            texDesc.Width = td.width;
            texDesc.Height = td.height;
            texDesc.DepthOrArraySize = 1; texDesc.MipLevels = 1;
            texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            texDesc.SampleDesc = { 1, 0 };

            ComPtr<ID3D12Resource> texRes, upRes;
            ThrowIfFailed(device_->CreateCommittedResource(
                &defHP, D3D12_HEAP_FLAG_NONE, &texDesc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texRes)));

            UINT64 uploadSize = 0;
            device_->GetCopyableFootprints(&texDesc, 0, 1, 0, nullptr, nullptr, nullptr, &uploadSize);
            D3D12_HEAP_PROPERTIES upHP = {}; upHP.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC upDesc = {};
            upDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; upDesc.Width = uploadSize;
            upDesc.Height = upDesc.DepthOrArraySize = upDesc.MipLevels = 1;
            upDesc.Format = DXGI_FORMAT_UNKNOWN; upDesc.SampleDesc = { 1,0 };
            upDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ThrowIfFailed(device_->CreateCommittedResource(
                &upHP, D3D12_HEAP_FLAG_NONE, &upDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upRes)));

            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
            device_->GetCopyableFootprints(&texDesc, 0, 1, 0, &fp, nullptr, nullptr, nullptr);
            BYTE* mapped = nullptr;
            upRes->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
            for (UINT row = 0; row < td.height; ++row)
                memcpy(mapped + row * fp.Footprint.RowPitch,
                    td.pixels.data() + row * td.width * 4, td.width * 4);
            upRes->Unmap(0, nullptr);

            D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
            dst.pResource = texRes.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.pResource = upRes.Get();  src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint = fp;
            commandList_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = texRes.Get();
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            commandList_->ResourceBarrier(1, &barrier);

            CreateSRVInSlot(texRes.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, slot);
            texResources_.push_back(std::move(texRes));
            texUploads_.push_back(std::move(upRes));
        };

    UploadReserved(CreateSolidColor(200, 200, 200), SRV_FALLBACK_ALBEDO); // slot 0
    UploadReserved(CreateFlatNormal(), SRV_FALLBACK_NORMAL); // slot 1
    UploadReserved(CreateSolidColor(128, 128, 128), SRV_FALLBACK_DISP);   // slot 2
    UploadReserved(CreateSolidColor(200, 200, 200), SRV_COMMON_ALBEDO2);  // slot 3
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
    texUploads_.clear();
}

// ============================================================
//  Device / swapchain / heaps
// ============================================================
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
        factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
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
    D3D12_COMMAND_QUEUE_DESC d = {};
    d.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ThrowIfFailed(device_->CreateCommandQueue(&d, IID_PPV_ARGS(&commandQueue_)));
}

void RenderingSystem::CreateSwapChain()
{
    ComPtr<IDXGIFactory4> factory;
    ThrowIfFailed(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    DXGI_SWAP_CHAIN_DESC1 d = {};
    d.BufferCount = RS_FRAME_COUNT; d.Width = (UINT)width_; d.Height = (UINT)height_;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; d.SampleDesc = { 1,0 };
    ComPtr<IDXGISwapChain1> sc1;
    ThrowIfFailed(factory->CreateSwapChainForHwnd(commandQueue_.Get(), hwnd_, &d, nullptr, nullptr, &sc1));
    factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
    ThrowIfFailed(sc1.As(&swapChain_));
    frameIndex_ = swapChain_->GetCurrentBackBufferIndex();
}

void RenderingSystem::CreateDescriptorHeaps()
{
    {
        D3D12_DESCRIPTOR_HEAP_DESC d = {};
        d.NumDescriptors = RS_FRAME_COUNT + GBUFFER_RT_COUNT;
        d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        ThrowIfFailed(device_->CreateDescriptorHeap(&d, IID_PPV_ARGS(&rtvHeap_)));
        rtvDescSize_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }
    {
        D3D12_DESCRIPTOR_HEAP_DESC d = {};
        d.NumDescriptors = 1; d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        ThrowIfFailed(device_->CreateDescriptorHeap(&d, IID_PPV_ARGS(&dsvHeap_)));
    }
    {
        D3D12_DESCRIPTOR_HEAP_DESC d = {};
        d.NumDescriptors = SRV_HEAP_SIZE;
        d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        d.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(device_->CreateDescriptorHeap(&d, IID_PPV_ARGS(&srvHeap_)));
        srvDescSize_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
}

void RenderingSystem::CreateRenderTargetViews()
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < RS_FRAME_COUNT; ++i)
    {
        ThrowIfFailed(swapChain_->GetBuffer(i, IID_PPV_ARGS(&renderTargets_[i])));
        device_->CreateRenderTargetView(renderTargets_[i].Get(), nullptr, h);
        rtvHandles_[i] = h;
        h.ptr += rtvDescSize_;
    }
}

void RenderingSystem::CreateDepthStencilBuffer()
{
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = (UINT)width_; d.Height = (UINT)height_;
    d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = DXGI_FORMAT_D32_FLOAT; d.SampleDesc = { 1,0 };
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE cv = {}; cv.Format = DXGI_FORMAT_D32_FLOAT; cv.DepthStencil.Depth = 1.0f;
    ThrowIfFailed(device_->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, IID_PPV_ARGS(&depthStencil_)));
    D3D12_DEPTH_STENCIL_VIEW_DESC dsvD = {};
    dsvD.Format = DXGI_FORMAT_D32_FLOAT; dsvD.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsvHandle_ = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
    device_->CreateDepthStencilView(depthStencil_.Get(), &dsvD, dsvHandle_);

    D3D12_CPU_DESCRIPTOR_HANDLE gbufRtvStart = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    gbufRtvStart.ptr += (SIZE_T)RS_FRAME_COUNT * rtvDescSize_;
    gbuffer_.Create(device_.Get(), width_, height_, rtvDescSize_, gbufRtvStart,
        srvHeap_.Get(), SRV_GBUF_BASE, dsvHandle_);
}

void RenderingSystem::CreateCommandObjects()
{
    for (UINT i = 0; i < RS_FRAME_COUNT; ++i)
        ThrowIfFailed(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocators_[i])));
    ThrowIfFailed(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        commandAllocators_[0].Get(), nullptr, IID_PPV_ARGS(&commandList_)));
    commandList_->Close();
}

void RenderingSystem::CreateFence()
{
    ThrowIfFailed(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)));
    fenceValues_[0] = fenceValues_[1] = 1;
    fenceEvent_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!fenceEvent_) ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
}

// ============================================================
//  Root Signatures
//  Geometry pass — 5 параметров:
//    [0] CBV b0
//    [1] DescriptorTable 1×SRV t0  — diffuse
//    [2] DescriptorTable 1×SRV t1  — albedo2
//    [3] DescriptorTable 1×SRV t2  — normal
//    [4] DescriptorTable 1×SRV t3  — displacement
//  Каждый DescriptorTable указывает на отдельный SRV-слот
//  без требования непрерывного расположения.
// ============================================================
void RenderingSystem::CreateGeometryPassRootSignature()
{
    D3D12_ROOT_PARAMETER params[5] = {};

    // param[0] — CBV
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // params[1..4] — по одному SRV каждый
    D3D12_DESCRIPTOR_RANGE ranges[4] = {};
    for (int i = 0; i < 4; ++i)
    {
        ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[i].NumDescriptors = 1;
        ranges[i].BaseShaderRegister = (UINT)i; // t0, t1, t2, t3
        ranges[i].OffsetInDescriptorsFromTableStart = 0;

        params[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1 + i].DescriptorTable.NumDescriptorRanges = 1;
        params[1 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
        params[1 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }

    D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
    samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[0].AddressU = samplers[0].AddressV = samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].MaxLOD = D3D12_FLOAT32_MAX; samplers[0].ShaderRegister = 0;
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    samplers[1] = samplers[0]; samplers[1].ShaderRegister = 1;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 5;
    desc.pParameters = params;
    desc.NumStaticSamplers = 2;
    desc.pStaticSamplers = samplers;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> sig, err;
    ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err));
    ThrowIfFailed(device_->CreateRootSignature(0, sig->GetBufferPointer(),
        sig->GetBufferSize(), IID_PPV_ARGS(&geometryRootSig_)));
}

void RenderingSystem::CreateLightingPassRootSignature()
{
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_DESCRIPTOR_RANGE gbufRange = {};
    gbufRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; gbufRange.NumDescriptors = 4;
    gbufRange.BaseShaderRegister = 0;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &gbufRange;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = 0.0f; sampler.ShaderRegister = 0; sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 2; desc.pParameters = params;
    desc.NumStaticSamplers = 1; desc.pStaticSamplers = &sampler;

    ComPtr<ID3DBlob> sig, err;
    ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err));
    ThrowIfFailed(device_->CreateRootSignature(0, sig->GetBufferPointer(),
        sig->GetBufferSize(), IID_PPV_ARGS(&lightingRootSig_)));
}

// ============================================================
//  Shader compiler
// ============================================================
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
    HRESULT hr = D3DCompile(source.c_str(), source.size(), nullptr, nullptr, nullptr,
        entry.c_str(), target.c_str(), flags, 0, &blob, &errors);
    if (FAILED(hr))
    {
        if (errors) MessageBoxA(hwnd_, (char*)errors->GetBufferPointer(), "Shader Error", MB_OK | MB_ICONERROR);
        ThrowIfFailed(hr, "CompileShader failed");
    }
    return blob;
}

// ============================================================
//  Geometry Pass PSO
// ============================================================
static const char* kGBufferShaderSrc = R"HLSL(
cbuffer GeometryCB : register(b0)
{
    matrix   World; matrix View; matrix Proj;
    float4   CameraPos; float2 Tiling; float2 UVOffset;
    float    BlendFactor; float TessNear; float TessFar;
    float    TessMinLevel; float TessMaxLevel; float DisplacementScale;
    float2   CBPad;
};
Texture2D gAlbedo1:register(t0); Texture2D gAlbedo2:register(t1);
Texture2D gNormalMap:register(t2); Texture2D gDispMap:register(t3);
SamplerState gSampler:register(s0); SamplerState gSamplerDisp:register(s1);

struct VSInput { float3 Pos:POSITION; float3 Normal:NORMAL; float4 Color:COLOR; float2 TexCoord:TEXCOORD0; };
struct HSInput { float3 PosWS:POSITION; float3 Normal:NORMAL; float2 TexCoord:TEXCOORD0; };

HSInput VSMain(VSInput i)
{
    HSInput o;
    o.PosWS   = mul(float4(i.Pos,1),World).xyz;
    o.Normal  = normalize(mul(float4(i.Normal,0),World).xyz);
    o.TexCoord= i.TexCoord;
    return o;
}

struct HSConstOut { float EdgeTess[3]:SV_TessFactor; float InsideTess[1]:SV_InsideTessFactor; };
float DTF(float3 p){ float d=distance(p,CameraPos.xyz); return lerp(TessMaxLevel,TessMinLevel,saturate((d-TessNear)/(TessFar-TessNear))); }
HSConstOut HSConst(InputPatch<HSInput,3> p, uint pid:SV_PrimitiveID)
{
    HSConstOut o;
    o.EdgeTess[0]=(DTF(p[1].PosWS)+DTF(p[2].PosWS))*.5f;
    o.EdgeTess[1]=(DTF(p[2].PosWS)+DTF(p[0].PosWS))*.5f;
    o.EdgeTess[2]=(DTF(p[0].PosWS)+DTF(p[1].PosWS))*.5f;
    o.InsideTess[0]=(o.EdgeTess[0]+o.EdgeTess[1]+o.EdgeTess[2])/3.f;
    return o;
}
[domain("tri")][partitioning("fractional_odd")][outputtopology("triangle_cw")]
[outputcontrolpoints(3)][patchconstantfunc("HSConst")][maxtessfactor(64.0f)]
HSInput HSMain(InputPatch<HSInput,3> p, uint i:SV_OutputControlPointID, uint pid:SV_PrimitiveID){ return p[i]; }

struct DSOut { float4 Pos:SV_POSITION; float3 PosWS:TEXCOORD0; float3 Normal:TEXCOORD1; float2 UV:TEXCOORD2; };
[domain("tri")]
DSOut DSMain(HSConstOut hsc, float3 b:SV_DomainLocation, const OutputPatch<HSInput,3> p)
{
    DSOut o;
    float3 pos = p[0].PosWS*b.x+p[1].PosWS*b.y+p[2].PosWS*b.z;
    float3 n   = normalize(p[0].Normal*b.x+p[1].Normal*b.y+p[2].Normal*b.z);
    float2 uv  = p[0].TexCoord*b.x+p[1].TexCoord*b.y+p[2].TexCoord*b.z;
    float disp = gDispMap.SampleLevel(gSamplerDisp,uv,0).r;
    pos += n*(disp*DisplacementScale);
    o.PosWS=pos; o.Normal=n; o.UV=uv;
    o.Pos=mul(mul(float4(pos,1),View),Proj);
    return o;
}

struct PSOut { float4 Albedo:SV_Target0; float4 Normal:SV_Target1; float4 PBR:SV_Target2; float4 WorldPos:SV_Target3; };
PSOut PSMain(DSOut i)
{
    PSOut o;
    float2 uv = i.UV + UVOffset;
    float4 t1=gAlbedo1.Sample(gSampler,uv), t2=gAlbedo2.Sample(gSampler,uv);
    o.Albedo  = float4(lerp(t1.rgb,t2.rgb,BlendFactor),1);
    float3 N=normalize(i.Normal);
    float3 T=normalize(ddx(i.PosWS)*ddy(i.UV).y - ddy(i.PosWS)*ddx(i.UV).y);
    float3 B=normalize(cross(N,T));
    float3 nm=gNormalMap.Sample(gSampler,i.UV).rgb*2-1;
    o.Normal  = float4(normalize(mul(nm,float3x3(T,B,N))),0);
    o.PBR     = float4(0.6f,0.1f,1.0f,1.0f);
    o.WorldPos= float4(i.PosWS,1);
    return o;
}
)HLSL";

void RenderingSystem::CreateGeometryPassPSO()
{
    auto vs = CompileShader(kGBufferShaderSrc, "VSMain", "vs_5_0");
    auto hs = CompileShader(kGBufferShaderSrc, "HSMain", "hs_5_0");
    auto ds = CompileShader(kGBufferShaderSrc, "DSMain", "ds_5_0");
    auto ps = CompileShader(kGBufferShaderSrc, "PSMain", "ps_5_0");

    D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,   0, 0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"NORMAL",  0,DXGI_FORMAT_R32G32B32_FLOAT,   0,12,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"COLOR",   0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,24,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,      0,40,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
    };
    D3D12_RASTERIZER_DESC raster = {};
    raster.FillMode = D3D12_FILL_MODE_SOLID; raster.CullMode = D3D12_CULL_MODE_BACK; raster.DepthClipEnable = TRUE;
    D3D12_DEPTH_STENCIL_DESC dss = {};
    dss.DepthEnable = TRUE; dss.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL; dss.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    D3D12_BLEND_DESC blend = {};
    for (UINT i = 0;i < GBUFFER_RT_COUNT;++i) blend.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = geometryRootSig_.Get();
    pso.VS = { vs->GetBufferPointer(),vs->GetBufferSize() };
    pso.HS = { hs->GetBufferPointer(),hs->GetBufferSize() };
    pso.DS = { ds->GetBufferPointer(),ds->GetBufferSize() };
    pso.PS = { ps->GetBufferPointer(),ps->GetBufferSize() };
    pso.InputLayout = { layout,_countof(layout) };
    pso.RasterizerState = raster; pso.DepthStencilState = dss; pso.BlendState = blend;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    pso.NumRenderTargets = GBUFFER_RT_COUNT;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pso.RTVFormats[2] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.RTVFormats[3] = DXGI_FORMAT_R32G32B32A32_FLOAT;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT; pso.SampleDesc = { 1,0 };
    ThrowIfFailed(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&geometryPSO_)));
}

// ============================================================
//  Lighting Pass PSO
// ============================================================
static const char* kLightingShaderSrc = R"HLSL(
#define MAX_LIGHTS 16
#define LIGHT_DIR 0
#define LIGHT_PT  1
#define LIGHT_SPT 2
struct LightData{ float4 PositionWS; float4 DirectionWS; float4 Color; float Range; float SpotInnerCosine; float SpotOuterCosine; uint Type; };
cbuffer LightingCB:register(b0){ float4 CameraPos; int LightCount; float3 LCBPad; LightData Lights[MAX_LIGHTS]; };
Texture2D gAlbedo:register(t0); Texture2D gNormal:register(t1);
Texture2D gPBR:register(t2);    Texture2D gWorldPos:register(t3);
SamplerState gSampler:register(s0);
struct FSQ{ float4 Pos:SV_POSITION; float2 UV:TEXCOORD0; };
FSQ VSMain(uint vid:SV_VertexID){
    FSQ o; float2 p,u;
    if(vid==0){p=float2(-1,-1);u=float2(0,1);}else if(vid==1){p=float2(-1,1);u=float2(0,0);}
    else if(vid==2){p=float2(1,1);u=float2(1,0);}else if(vid==3){p=float2(-1,-1);u=float2(0,1);}
    else if(vid==4){p=float2(1,1);u=float2(1,0);}else{p=float2(1,-1);u=float2(1,1);}
    o.Pos=float4(p,0,1);o.UV=u;return o;
}
float Att(float d,float r){float f=saturate(1-d/r);return f*f/(d*d+1);}
float3 BP(float3 N,float3 L,float3 V,float3 alb,float rgh,float3 lc){
    float d=max(dot(N,L),0); float3 H=normalize(L+V);
    float sh=max(2/(rgh*rgh+0.001f)-2,1); float sp=pow(max(dot(N,H),0),sh);
    return alb*d*lc+sp*lc*(1-rgh);
}
float4 PSMain(FSQ i):SV_TARGET{
    float3 alb=gAlbedo.Sample(gSampler,i.UV).rgb;
    float3 nrm=gNormal.Sample(gSampler,i.UV).rgb;
    float4 pbr=gPBR.Sample(gSampler,i.UV);
    float rgh=pbr.r,ao=pbr.b;
    if(dot(nrm,nrm)<0.01f) return float4(0.02f,0.02f,0.05f,1);
    float3 N=normalize(nrm), wp=gWorldPos.Sample(gSampler,i.UV).rgb;
    float3 V=normalize(CameraPos.xyz-wp);
    float3 col=alb*0.08f*ao;
    for(int k=0;k<LightCount;++k){
        LightData l=Lights[k];
        if(l.Type==LIGHT_DIR){ col+=BP(N,normalize(-l.DirectionWS.xyz),V,alb,rgh,l.Color.rgb*l.Color.w); }
        else if(l.Type==LIGHT_PT){ float3 tl=l.PositionWS.xyz-wp; float d=length(tl); if(d<l.Range) col+=BP(N,tl/d,V,alb,rgh,l.Color.rgb*l.Color.w)*Att(d,l.Range); }
        else if(l.Type==LIGHT_SPT){ float3 tl=l.PositionWS.xyz-wp; float d=length(tl); if(d<l.Range){ float3 L=tl/d; float ca=dot(-L,normalize(l.DirectionWS.xyz)); if(ca>l.SpotOuterCosine){ float sf=saturate((ca-l.SpotOuterCosine)/(l.SpotInnerCosine-l.SpotOuterCosine+0.0001f)); sf*=sf; col+=BP(N,L,V,alb,rgh,l.Color.rgb*l.Color.w)*Att(d,l.Range)*sf; } } }
    }
    col=col/(col+1); return float4(col,1);
}
)HLSL";

void RenderingSystem::CreateLightingPassPSO()
{
    auto vs = CompileShader(kLightingShaderSrc, "VSMain", "vs_5_0");
    auto ps = CompileShader(kLightingShaderSrc, "PSMain", "ps_5_0");
    D3D12_RASTERIZER_DESC raster = {}; raster.FillMode = D3D12_FILL_MODE_SOLID; raster.CullMode = D3D12_CULL_MODE_NONE;
    D3D12_DEPTH_STENCIL_DESC dss = {}; dss.DepthEnable = FALSE; dss.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    D3D12_BLEND_DESC blend = {}; blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = lightingRootSig_.Get();
    pso.VS = { vs->GetBufferPointer(),vs->GetBufferSize() };
    pso.PS = { ps->GetBufferPointer(),ps->GetBufferSize() };
    pso.InputLayout = { nullptr,0 }; pso.RasterizerState = raster; pso.DepthStencilState = dss;
    pso.BlendState = blend; pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1; pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.DSVFormat = DXGI_FORMAT_UNKNOWN; pso.SampleDesc = { 1,0 };
    ThrowIfFailed(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&lightingPSO_)));
}

// ============================================================
//  Texture helpers
// ============================================================
D3D12_GPU_DESCRIPTOR_HANDLE RenderingSystem::GpuHandle(int slot) const
{
    D3D12_GPU_DESCRIPTOR_HANDLE h = srvHeap_->GetGPUDescriptorHandleForHeapStart();
    h.ptr += (UINT64)slot * srvDescSize_;
    return h;
}

void RenderingSystem::CreateSRVInSlot(ID3D12Resource* res, DXGI_FORMAT fmt, int slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = srvHeap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)slot * srvDescSize_;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = fmt; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(res, &srv, h);
}

int RenderingSystem::UploadTextureToHeap(const TextureData& td)
{
    if (!td.valid || td.width == 0 || td.height == 0)
        ThrowIfFailed(E_FAIL, "UploadTextureToHeap: invalid data");
    if (srvNextFree_ >= SRV_HEAP_SIZE)
        ThrowIfFailed(E_FAIL, "SRV heap overflow — слишком много уникальных текстур");

    int slot = srvNextFree_++;

    D3D12_HEAP_PROPERTIES defHP = {}; defHP.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td2 = {};
    td2.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td2.Width = td.width; td2.Height = td.height;
    td2.DepthOrArraySize = 1; td2.MipLevels = 1;
    td2.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td2.SampleDesc = { 1,0 };

    ComPtr<ID3D12Resource> texRes, upRes;
    ThrowIfFailed(device_->CreateCommittedResource(&defHP, D3D12_HEAP_FLAG_NONE, &td2,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texRes)));

    UINT64 uploadSize = 0;
    device_->GetCopyableFootprints(&td2, 0, 1, 0, nullptr, nullptr, nullptr, &uploadSize);
    D3D12_HEAP_PROPERTIES upHP = {}; upHP.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC upD = {};
    upD.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; upD.Width = uploadSize;
    upD.Height = upD.DepthOrArraySize = upD.MipLevels = 1;
    upD.Format = DXGI_FORMAT_UNKNOWN; upD.SampleDesc = { 1,0 };
    upD.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ThrowIfFailed(device_->CreateCommittedResource(&upHP, D3D12_HEAP_FLAG_NONE, &upD,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upRes)));

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    device_->GetCopyableFootprints(&td2, 0, 1, 0, &fp, nullptr, nullptr, nullptr);
    BYTE* mapped = nullptr; upRes->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
    for (UINT row = 0;row < td.height;++row)
        memcpy(mapped + row * fp.Footprint.RowPitch, td.pixels.data() + row * td.width * 4, td.width * 4);
    upRes->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = texRes.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = upRes.Get();  src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = fp;
    commandList_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = texRes.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    commandList_->ResourceBarrier(1, &b);

    CreateSRVInSlot(texRes.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, slot);
    texResources_.push_back(std::move(texRes));
    texUploads_.push_back(std::move(upRes));
    return slot;
}

int RenderingSystem::LoadAndCacheTexture(const std::wstring& path, bool isNormal)
{
    if (path.empty()) return isNormal ? SRV_FALLBACK_NORMAL : SRV_FALLBACK_ALBEDO;
    auto it = texCache_.find(path);
    if (it != texCache_.end()) return it->second;

    TextureData td = LoadTextureAuto(path);
    if (!td.valid) td = isNormal ? CreateFlatNormal() : CreateSolidColor(200, 200, 200);
    int slot = UploadTextureToHeap(td);
    texCache_[path] = slot;
    return slot;
}

// ============================================================
//  BuildMesh1
// ============================================================
void RenderingSystem::BuildMesh1(
    const std::vector<Vertex>& verts, const std::vector<UINT>& idxs,
    const std::vector<UINT>& subIndexStarts, const std::vector<UINT>& subIndexCounts,
    const std::vector<std::wstring>& subDiffuse, const std::vector<std::wstring>& subNormal)
{
    indexCount_ = (UINT)idxs.size();
    MakeUploadBuffer(verts.data(), verts.size() * sizeof(Vertex), vertexBuffer_);
    vbView_.BufferLocation = vertexBuffer_->GetGPUVirtualAddress();
    vbView_.SizeInBytes = (UINT)(verts.size() * sizeof(Vertex)); vbView_.StrideInBytes = sizeof(Vertex);
    MakeUploadBuffer(idxs.data(), idxs.size() * sizeof(UINT), indexBuffer_);
    ibView_.BufferLocation = indexBuffer_->GetGPUVirtualAddress();
    ibView_.SizeInBytes = (UINT)(idxs.size() * sizeof(UINT)); ibView_.Format = DXGI_FORMAT_R32_UINT;

    size_t n = subIndexStarts.size();
    subDrawCalls_.resize(n);
    for (size_t i = 0; i < n; ++i)
    {
        subDrawCalls_[i].indexStart = subIndexStarts[i];
        subDrawCalls_[i].indexCount = subIndexCounts[i];
        // Загружаем или берём из кэша — одна текстура грузится ровно один раз
        subDrawCalls_[i].srvDiffuse = LoadAndCacheTexture(subDiffuse[i], false);
        subDrawCalls_[i].srvNormal = LoadAndCacheTexture(subNormal[i], true);
    }
}

// ============================================================
//  BuildBuffers2 / UploadTextureMesh2
// ============================================================
void RenderingSystem::BuildBuffers2(const std::vector<Vertex>& verts, const std::vector<UINT>& idxs)
{
    indexCount2_ = (UINT)idxs.size();
    MakeUploadBuffer(verts.data(), verts.size() * sizeof(Vertex), vertexBuffer2_);
    vbView2_.BufferLocation = vertexBuffer2_->GetGPUVirtualAddress();
    vbView2_.SizeInBytes = (UINT)(verts.size() * sizeof(Vertex)); vbView2_.StrideInBytes = sizeof(Vertex);
    MakeUploadBuffer(idxs.data(), idxs.size() * sizeof(UINT), indexBuffer2_);
    ibView2_.BufferLocation = indexBuffer2_->GetGPUVirtualAddress();
    ibView2_.SizeInBytes = (UINT)(idxs.size() * sizeof(UINT)); ibView2_.Format = DXGI_FORMAT_R32_UINT;

    // Резервируем 4 слота для mesh2 и заполняем fallback
    mesh2SrvBase_ = srvNextFree_;
    srvNextFree_ += 4;
    if (srvNextFree_ > SRV_HEAP_SIZE) ThrowIfFailed(E_FAIL, "SRV heap overflow (mesh2)");

    // Копируем fallback дескрипторы в 4 слота mesh2
    auto CopySlot = [&](int from, int to)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE s = srvHeap_->GetCPUDescriptorHandleForHeapStart();
            s.ptr += (SIZE_T)from * srvDescSize_;
            D3D12_CPU_DESCRIPTOR_HANDLE d = srvHeap_->GetCPUDescriptorHandleForHeapStart();
            d.ptr += (SIZE_T)to * srvDescSize_;
            device_->CopyDescriptorsSimple(1, d, s, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        };
    CopySlot(SRV_FALLBACK_ALBEDO, mesh2SrvBase_ + 0);
    CopySlot(SRV_COMMON_ALBEDO2, mesh2SrvBase_ + 1);
    CopySlot(SRV_FALLBACK_NORMAL, mesh2SrvBase_ + 2);
    CopySlot(SRV_FALLBACK_DISP, mesh2SrvBase_ + 3);
}

void RenderingSystem::UploadTextureMesh2(const TextureData& td, int slot)
{
    if (slot < 0 || slot >= 4 || !td.valid) return;
    int tempSlot = UploadTextureToHeap(td);
    D3D12_CPU_DESCRIPTOR_HANDLE s = srvHeap_->GetCPUDescriptorHandleForHeapStart();
    s.ptr += (SIZE_T)tempSlot * srvDescSize_;
    D3D12_CPU_DESCRIPTOR_HANDLE d = srvHeap_->GetCPUDescriptorHandleForHeapStart();
    d.ptr += (SIZE_T)(mesh2SrvBase_ + slot) * srvDescSize_;
    device_->CopyDescriptorsSimple(1, d, s, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void RenderingSystem::MakeUploadBuffer(const void* data, UINT64 byteSize, ComPtr<ID3D12Resource>& buf)
{
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = byteSize;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.Format = DXGI_FORMAT_UNKNOWN; d.SampleDesc = { 1,0 };
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ThrowIfFailed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buf)));
    BYTE* m = nullptr; buf->Map(0, nullptr, reinterpret_cast<void**>(&m));
    memcpy(m, data, (size_t)byteSize); buf->Unmap(0, nullptr);
}

// ============================================================
//  Constant Buffers
// ============================================================
void RenderingSystem::CreateConstantBuffers()
{
    auto Align256 = [](UINT64 n) {return (n + 255) & ~UINT64(255);};
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    auto make = [&](UINT64 sz, ComPtr<ID3D12Resource>& res) {
        D3D12_RESOURCE_DESC d = {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = Align256(sz);
        d.Height = d.DepthOrArraySize = d.MipLevels = 1;
        d.Format = DXGI_FORMAT_UNKNOWN; d.SampleDesc = { 1,0 };
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ThrowIfFailed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&res)));
        };
    make(sizeof(ConstantBufferData), geometryCB_);
    ThrowIfFailed(geometryCB_->Map(0, nullptr, reinterpret_cast<void**>(&geometryCBMapped_)));
    make(sizeof(ConstantBufferData), geometryCB2_);
    ThrowIfFailed(geometryCB2_->Map(0, nullptr, reinterpret_cast<void**>(&geometryCB2Mapped_)));
    make(sizeof(LightingPassCB), lightingCBRes_);
    ThrowIfFailed(lightingCBRes_->Map(0, nullptr, reinterpret_cast<void**>(&lightingCBMapped_)));
}

void RenderingSystem::UpdateGeometryPassCB(const ConstantBufferData& data) { if (geometryCBMapped_)*geometryCBMapped_ = data; }
void RenderingSystem::UpdateGeometryPassCB2(const ConstantBufferData& data) { if (geometryCB2Mapped_)*geometryCB2Mapped_ = data; }
void RenderingSystem::UpdateLightingPassCB(const LightingPassCB& data) { lightingCB_ = data; if (lightingCBMapped_)*lightingCBMapped_ = data; }

// ============================================================
//  Lights
// ============================================================
void RenderingSystem::ClearLights() { lightingCB_.LightCount = 0; }
void RenderingSystem::AddDirectionalLight(XMFLOAT3 dir, XMFLOAT3 col, float intens) {
    if (lightingCB_.LightCount >= MAX_LIGHTS)return;
    LightData& l = lightingCB_.Lights[lightingCB_.LightCount++];
    l.PositionWS = { 0,0,0,0 };l.DirectionWS = { dir.x,dir.y,dir.z,0 };
    l.Color = { col.x,col.y,col.z,intens };l.Range = l.SpotInnerCosine = l.SpotOuterCosine = 0;
    l.Type = (UINT)LightType::Directional;
}
void RenderingSystem::AddPointLight(XMFLOAT3 pos, XMFLOAT3 col, float intens, float range) {
    if (lightingCB_.LightCount >= MAX_LIGHTS)return;
    LightData& l = lightingCB_.Lights[lightingCB_.LightCount++];
    l.PositionWS = { pos.x,pos.y,pos.z,1 };l.DirectionWS = { 0,0,0,0 };
    l.Color = { col.x,col.y,col.z,intens };l.Range = range;l.SpotInnerCosine = l.SpotOuterCosine = 0;
    l.Type = (UINT)LightType::Point;
}
void RenderingSystem::AddSpotLight(XMFLOAT3 pos, XMFLOAT3 dir, XMFLOAT3 col, float intens, float range, float iDeg, float oDeg) {
    if (lightingCB_.LightCount >= MAX_LIGHTS)return;
    const float D2R = 3.14159265f / 180.f;
    LightData& l = lightingCB_.Lights[lightingCB_.LightCount++];
    l.PositionWS = { pos.x,pos.y,pos.z,1 };l.DirectionWS = { dir.x,dir.y,dir.z,0 };
    l.Color = { col.x,col.y,col.z,intens };l.Range = range;
    l.SpotInnerCosine = cosf(iDeg * D2R);l.SpotOuterCosine = cosf(oDeg * D2R);
    l.Type = (UINT)LightType::Spot;
}

// ============================================================
//  Frame
// ============================================================
ID3D12GraphicsCommandList* RenderingSystem::BeginFrame()
{
    ThrowIfFailed(commandAllocators_[frameIndex_]->Reset());
    ThrowIfFailed(commandList_->Reset(commandAllocators_[frameIndex_].Get(), nullptr));
    gbuffer_.TransitionTo(commandList_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
    gbuffer_.Clear(commandList_.Get());
    commandList_->ClearDepthStencilView(dsvHandle_, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    commandList_->OMSetRenderTargets(GBUFFER_RT_COUNT, gbuffer_.GetRTVHandles(), FALSE, &dsvHandle_);
    D3D12_VIEWPORT vp = { 0,0,(float)width_,(float)height_,0,1 };
    D3D12_RECT sr = { 0,0,width_,height_ };
    commandList_->RSSetViewports(1, &vp);
    commandList_->RSSetScissorRects(1, &sr);
    return commandList_.Get();
}

void RenderingSystem::BindGeometryPass()
{
    commandList_->SetGraphicsRootSignature(geometryRootSig_.Get());
    commandList_->SetPipelineState(geometryPSO_.Get());
    ID3D12DescriptorHeap* heaps[] = { srvHeap_.Get() };
    commandList_->SetDescriptorHeaps(1, heaps);
    commandList_->SetGraphicsRootConstantBufferView(0, geometryCB_->GetGPUVirtualAddress());
    // param[2] = albedo2 (общий), param[4] = displacement (общий) — не меняются между draw calls
    commandList_->SetGraphicsRootDescriptorTable(2, GpuHandle(SRV_COMMON_ALBEDO2));
    commandList_->SetGraphicsRootDescriptorTable(4, GpuHandle(SRV_FALLBACK_DISP));
    commandList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
    commandList_->IASetVertexBuffers(0, 1, &vbView_);
    commandList_->IASetIndexBuffer(&ibView_);
}

void RenderingSystem::DrawMesh1SubMeshes()
{
    for (const auto& dc : subDrawCalls_)
    {
        if (dc.indexCount == 0) continue;
        // Меняем только diffuse (param[1]) и normal (param[3])
        commandList_->SetGraphicsRootDescriptorTable(1, GpuHandle(dc.srvDiffuse));
        commandList_->SetGraphicsRootDescriptorTable(3, GpuHandle(dc.srvNormal));
        commandList_->DrawIndexedInstanced(dc.indexCount, 1, dc.indexStart, 0, 0);
    }
}

void RenderingSystem::BindGeometryPassMesh2()
{
    commandList_->SetGraphicsRootConstantBufferView(0, geometryCB2_->GetGPUVirtualAddress());
    // Mesh2: все 4 текстуры из mesh2SrvBase_
    commandList_->SetGraphicsRootDescriptorTable(1, GpuHandle(mesh2SrvBase_ + 0)); // diffuse
    commandList_->SetGraphicsRootDescriptorTable(2, GpuHandle(mesh2SrvBase_ + 1)); // albedo2
    commandList_->SetGraphicsRootDescriptorTable(3, GpuHandle(mesh2SrvBase_ + 2)); // normal
    commandList_->SetGraphicsRootDescriptorTable(4, GpuHandle(mesh2SrvBase_ + 3)); // disp
}

void RenderingSystem::DrawMesh2()
{
    commandList_->IASetVertexBuffers(0, 1, &vbView2_);
    commandList_->IASetIndexBuffer(&ibView2_);
    commandList_->DrawIndexedInstanced(indexCount2_, 1, 0, 0, 0);
}

void RenderingSystem::BeginLightingPass()
{
    gbuffer_.TransitionTo(commandList_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = renderTargets_[frameIndex_].Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    commandList_->ResourceBarrier(1, &b);
    const float cc[] = { 0,0,0,1 };
    commandList_->ClearRenderTargetView(rtvHandles_[frameIndex_], cc, 0, nullptr);
    commandList_->OMSetRenderTargets(1, &rtvHandles_[frameIndex_], FALSE, nullptr);
    if (lightingCBMapped_)*lightingCBMapped_ = lightingCB_;
    commandList_->SetGraphicsRootSignature(lightingRootSig_.Get());
    commandList_->SetPipelineState(lightingPSO_.Get());
    ID3D12DescriptorHeap* heaps[] = { srvHeap_.Get() };
    commandList_->SetDescriptorHeaps(1, heaps);
    commandList_->SetGraphicsRootConstantBufferView(0, lightingCBRes_->GetGPUVirtualAddress());
    commandList_->SetGraphicsRootDescriptorTable(1, gbuffer_.GetSRVGpuStart());
    commandList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList_->IASetVertexBuffers(0, 0, nullptr);
    commandList_->IASetIndexBuffer(nullptr);
}

void RenderingSystem::DrawLightingQuad() { commandList_->DrawInstanced(6, 1, 0, 0); }

void RenderingSystem::EndFrame()
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = renderTargets_[frameIndex_].Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    commandList_->ResourceBarrier(1, &b);
    ThrowIfFailed(commandList_->Close());
    ID3D12CommandList* lists[] = { commandList_.Get() };
    commandQueue_->ExecuteCommandLists(1, lists);
    ThrowIfFailed(swapChain_->Present(1, 0));
    MoveToNextFrame();
}

void RenderingSystem::Resize(int width, int height)
{
    if (width == width_ && height == height_)return;
    WaitForGPU(); width_ = width; height_ = height;
    for (auto& rt : renderTargets_)rt.Reset(); depthStencil_.Reset();
    ThrowIfFailed(swapChain_->ResizeBuffers(RS_FRAME_COUNT, (UINT)width, (UINT)height, DXGI_FORMAT_R8G8B8A8_UNORM, 0));
    frameIndex_ = swapChain_->GetCurrentBackBufferIndex();
    CreateRenderTargetViews(); CreateDepthStencilBuffer();
}

void RenderingSystem::WaitForGPU()
{
    ThrowIfFailed(commandQueue_->Signal(fence_.Get(), fenceValues_[frameIndex_]));
    ThrowIfFailed(fence_->SetEventOnCompletion(fenceValues_[frameIndex_], fenceEvent_));
    WaitForSingleObject(fenceEvent_, INFINITE);
    ++fenceValues_[frameIndex_];
}

void RenderingSystem::MoveToNextFrame()
{
    const UINT64 cur = fenceValues_[frameIndex_];
    ThrowIfFailed(commandQueue_->Signal(fence_.Get(), cur));
    frameIndex_ = swapChain_->GetCurrentBackBufferIndex();
    if (fence_->GetCompletedValue() < fenceValues_[frameIndex_])
    {
        ThrowIfFailed(fence_->SetEventOnCompletion(fenceValues_[frameIndex_], fenceEvent_));
        WaitForSingleObject(fenceEvent_, INFINITE);
    }
    fenceValues_[frameIndex_] = cur + 1;
}