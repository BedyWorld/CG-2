#include "RenderingSystem.h"
#include "TextureLoader.h"
#include <cassert>
#include <cmath>
#include <cstring>

RenderingSystem::RenderingSystem(HWND hwnd, int width, int height)
    : hwnd_(hwnd), width_(width), height_(height)
{
    for (UINT i = 0; i < RS_FRAME_COUNT; ++i)
        fenceValues_[i] = 0;
    srvNextFree_ = 16;
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
    CreateInstancedPSO();
    CreateShadowPassRootSignature();
    CreateShadowPSOs();
    CreateLightingPassPSO();
    CreatePostRootSignature();
    CreateTonemapPSO();
    CreateTaaRootSignature();
    CreateTaaPSO();
    CreateWaterPassRootSignature();
    CreateWaterPSO();
    CreateTerrainRootSignature();
    CreateTerrainPSO();
    BuildTerrainGrid();
}

void RenderingSystem::BeginResourceUpload()
{
    ThrowIfFailed(commandAllocators_[0]->Reset());
    ThrowIfFailed(commandList_->Reset(commandAllocators_[0].Get(), nullptr));

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
        d.NumDescriptors = RS_FRAME_COUNT + GBUFFER_RT_COUNT + 1 + 2;
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
    CreateSceneHDR();
    CreateTaaHistory();
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

void RenderingSystem::CreateGeometryPassRootSignature()
{
    D3D12_ROOT_PARAMETER params[6] = {};

    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_DESCRIPTOR_RANGE ranges[4] = {};
    for (int i = 0; i < 4; ++i)
    {
        ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[i].NumDescriptors = 1;
        ranges[i].BaseShaderRegister = (UINT)i;
        ranges[i].OffsetInDescriptorsFromTableStart = 0;

        params[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1 + i].DescriptorTable.NumDescriptorRanges = 1;
        params[1 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
        params[1 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }

    params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[5].Descriptor.ShaderRegister = 4;
    params[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
    samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[0].AddressU = samplers[0].AddressV = samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].MaxLOD = D3D12_FLOAT32_MAX; samplers[0].ShaderRegister = 0;
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    samplers[1] = samplers[0]; samplers[1].ShaderRegister = 1;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 6;
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
    D3D12_ROOT_PARAMETER params[3] = {};
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

    D3D12_DESCRIPTOR_RANGE shadowRange = {};
    shadowRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; shadowRange.NumDescriptors = 1;
    shadowRange.BaseShaderRegister = 4;                      // t4
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &shadowRange;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
    samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samplers[0].AddressU = samplers[0].AddressV = samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].MaxLOD = 0.0f; samplers[0].ShaderRegister = 0;
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    samplers[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    samplers[1].AddressU = samplers[1].AddressV = samplers[1].AddressW =
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    samplers[1].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    samplers[1].MaxLOD = 0.0f; samplers[1].ShaderRegister = 1;
    samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 3; desc.pParameters = params;
    desc.NumStaticSamplers = 2; desc.pStaticSamplers = samplers;

    ComPtr<ID3DBlob> sig, err;
    ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err));
    ThrowIfFailed(device_->CreateRootSignature(0, sig->GetBufferPointer(),
        sig->GetBufferSize(), IID_PPV_ARGS(&lightingRootSig_)));
}

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

static const char* kGBufferShaderSrc = R"HLSL(
cbuffer GeometryCB : register(b0)
{
    matrix   World; matrix View; matrix Proj;
    float4   CameraPos; float2 Tiling; float2 UVOffset;
    float    BlendFactor; float TessNear; float TessFar;
    float    TessMinLevel; float TessMaxLevel; float DisplacementScale;
    float    WireMode;
    float    CBPad;
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

// Cull a patch that lies entirely outside the view frustum. Zero tess
// factors make the tessellator drop it BEFORE subdivision, which is the
// cheapest way to avoid paying for off-screen geometry.
// The 1.15 margin and the DisplacementScale term account for vertices
// being displaced later, which can pull the patch back into view.
bool PatchCulled(float3 a, float3 b, float3 c)
{
    float4 ca=mul(mul(float4(a,1),View),Proj);
    float4 cb=mul(mul(float4(b,1),View),Proj);
    float4 cc=mul(mul(float4(c,1),View),Proj);
    // A point behind the camera gives w<=0 and the comparisons stop being
    // reliable, so leave those patches alone.
    if (ca.w<=0.0f || cb.w<=0.0f || cc.w<=0.0f) return false;
    float wa=ca.w*1.15f+DisplacementScale;
    float wb=cb.w*1.15f+DisplacementScale;
    float wc=cc.w*1.15f+DisplacementScale;
    if (ca.x<-wa && cb.x<-wb && cc.x<-wc) return true;
    if (ca.x> wa && cb.x> wb && cc.x> wc) return true;
    if (ca.y<-wa && cb.y<-wb && cc.y<-wc) return true;
    if (ca.y> wa && cb.y> wb && cc.y> wc) return true;
    if (ca.z> wa && cb.z> wb && cc.z> wc) return true;
    return false;
}

HSConstOut HSConst(InputPatch<HSInput,3> p, uint pid:SV_PrimitiveID)
{
    HSConstOut o;
    if (PatchCulled(p[0].PosWS,p[1].PosWS,p[2].PosWS))
    {
        o.EdgeTess[0]=0; o.EdgeTess[1]=0; o.EdgeTess[2]=0; o.InsideTess[0]=0;
        return o;
    }
    o.EdgeTess[0]=(DTF(p[1].PosWS)+DTF(p[2].PosWS))*.5f;
    o.EdgeTess[1]=(DTF(p[2].PosWS)+DTF(p[0].PosWS))*.5f;
    o.EdgeTess[2]=(DTF(p[0].PosWS)+DTF(p[1].PosWS))*.5f;
    o.InsideTess[0]=(o.EdgeTess[0]+o.EdgeTess[1]+o.EdgeTess[2])/3.f;
    return o;
}
[domain("tri")][partitioning("fractional_odd")][outputtopology("triangle_cw")]
[outputcontrolpoints(3)][patchconstantfunc("HSConst")][maxtessfactor(12.0f)]
HSInput HSMain(InputPatch<HSInput,3> p, uint i:SV_OutputControlPointID, uint pid:SV_PrimitiveID){ return p[i]; }

struct DSOut { float4 Pos:SV_POSITION; float3 PosWS:TEXCOORD0; float3 Normal:TEXCOORD1; float2 UV:TEXCOORD2; };
[domain("tri")]
DSOut DSMain(HSConstOut hsc, float3 b:SV_DomainLocation, const OutputPatch<HSInput,3> p)
{
    DSOut o;
    float3 pos = p[0].PosWS*b.x+p[1].PosWS*b.y+p[2].PosWS*b.z;
    float3 n   = normalize(p[0].Normal*b.x+p[1].Normal*b.y+p[2].Normal*b.z);
    float2 uv  = p[0].TexCoord*b.x+p[1].TexCoord*b.y+p[2].TexCoord*b.z;
    // 0.5 is the neutral level. Displacement used to be one-sided, so a
    // flat grey map inflated the mesh uniformly along its normals.
    float disp = gDispMap.SampleLevel(gSamplerDisp,uv,0).r;
    pos += n*((disp-0.5f)*2.0f*DisplacementScale);
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
    // TBN from screen-space derivatives. Where UVs are degenerate
    // (flat or welded unwraps, common in Sponza) the derivatives vanish,
    // normalize returns NaN and the pixel turns to garbage.
    // So build the tangent only when it is non-zero, else use a fallback basis.
    float3 Traw = ddx(i.PosWS)*ddy(i.UV).y - ddy(i.PosWS)*ddx(i.UV).y;
    float  Tlen2 = dot(Traw,Traw);
    float3 T, B;
    if (Tlen2 > 1e-12f)
    {
        T = Traw * rsqrt(Tlen2);
        T = normalize(T - N*dot(N,T));      // Gram-Schmidt orthogonalization
        B = cross(N,T);
    }
    else
    {
        float3 up = abs(N.y) < 0.999f ? float3(0,1,0) : float3(1,0,0);
        T = normalize(cross(up,N));
        B = cross(N,T);
    }
    float3 nm=gNormalMap.Sample(gSampler,i.UV).rgb*2-1;
    float3 nWS = mul(nm,float3x3(T,B,N));
    float  nLen2 = dot(nWS,nWS);
    o.Normal  = float4(nLen2 > 1e-12f ? nWS*rsqrt(nLen2) : N, 0);
    // Wireframe: the metallic channel is the emissive flag the lighting pass
    // already understands, so the mesh lines come out bright and readable
    // instead of being shaded and lost in shadow.
    if (WireMode > 0.5f)
    {
        o.Albedo = float4(0.15f, 0.85f, 0.35f, 1.0f);
        o.PBR    = float4(0.6f, 1.0f, 1.0f, 1.0f);
    }
    else
    {
        o.PBR     = float4(0.6f,0.1f,1.0f,1.0f);
    }
    o.WorldPos= float4(i.PosWS,1);
    return o;
}
)HLSL";
static const char* kWaterShaderSrc = R"HLSL(
#define MAX_WATER_WAVES 4
cbuffer WaterCB : register(b0)
{
    matrix World; matrix View; matrix Proj;
    float4 CameraPos;
    float4 ShallowColor;
    float4 DeepColor;
    float4 WaveDirAmp[MAX_WATER_WAVES];
    float4 WaveSpeed;
    float  Time;
    float  TessNear; float TessFar;
    float  TessMinLevel; float TessMaxLevel;
    float  Roughness;
    int    WaveCount;
    float  _pad;
};

struct VSInput { float3 Pos:POSITION; float3 Normal:NORMAL; float4 Color:COLOR; float2 TexCoord:TEXCOORD0; };
struct HSInput { float3 PosWS:POSITION; float2 UV:TEXCOORD0; };

HSInput VSMain(VSInput i)
{
    HSInput o;
    o.PosWS = mul(float4(i.Pos,1), World).xyz;
    o.UV    = i.TexCoord;
    return o;
}

float DistTess(float3 p)
{
    float d = distance(p, CameraPos.xyz);
    return lerp(TessMaxLevel, TessMinLevel, saturate((d - TessNear)/(TessFar - TessNear)));
}

struct HSConstOut { float EdgeTess[3]:SV_TessFactor; float InsideTess[1]:SV_InsideTessFactor; };

HSConstOut HSConst(InputPatch<HSInput,3> p, uint pid:SV_PrimitiveID)
{
    HSConstOut o;
    o.EdgeTess[0]=(DistTess(p[1].PosWS)+DistTess(p[2].PosWS))*.5f;
    o.EdgeTess[1]=(DistTess(p[2].PosWS)+DistTess(p[0].PosWS))*.5f;
    o.EdgeTess[2]=(DistTess(p[0].PosWS)+DistTess(p[1].PosWS))*.5f;
    o.InsideTess[0]=(o.EdgeTess[0]+o.EdgeTess[1]+o.EdgeTess[2])/3.f;
    return o;
}

[domain("tri")][partitioning("fractional_odd")][outputtopology("triangle_cw")]
[outputcontrolpoints(3)][patchconstantfunc("HSConst")][maxtessfactor(32.0f)]
HSInput HSMain(InputPatch<HSInput,3> p, uint i:SV_OutputControlPointID, uint pid:SV_PrimitiveID)
{
    return p[i];
}

void EvalWave(float2 xz, out float height, out float2 grad)
{
    height = 0.0f; grad = float2(0,0);
    [unroll]
    for (int k = 0; k < MAX_WATER_WAVES; ++k)
    {
        if (k >= WaveCount) break;
        float2 dir  = WaveDirAmp[k].xy;
        float  amp  = WaveDirAmp[k].z;
        float  freq = WaveDirAmp[k].w;
        float  spd  = WaveSpeed[k];
        float  phase = dot(dir, xz) * freq + Time * spd;
        height += amp * sin(phase);
        grad   += (amp * freq * cos(phase)) * dir;
    }
}

struct DSOut { float4 Pos:SV_POSITION; float3 PosWS:TEXCOORD0; float3 Normal:TEXCOORD1; float2 UV:TEXCOORD2; };

[domain("tri")]
DSOut DSMain(HSConstOut hsc, float3 b:SV_DomainLocation, const OutputPatch<HSInput,3> p)
{
    DSOut o;
    float3 pos = p[0].PosWS*b.x + p[1].PosWS*b.y + p[2].PosWS*b.z;
    float2 uv  = p[0].UV*b.x    + p[1].UV*b.y    + p[2].UV*b.z;

    float h; float2 grad;
    EvalWave(pos.xz, h, grad);
    pos.y += h;

    float3 n = normalize(float3(-grad.x, 1.0f, -grad.y));

    o.PosWS = pos; o.Normal = n; o.UV = uv;
    o.Pos = mul(mul(float4(pos,1), View), Proj);
    return o;
}

struct PSOut { float4 Albedo:SV_Target0; float4 Normal:SV_Target1; float4 PBR:SV_Target2; float4 WorldPos:SV_Target3; };

PSOut PSMain(DSOut i)
{
    PSOut o;
    float3 N = normalize(i.Normal);
    float3 V = normalize(CameraPos.xyz - i.PosWS);
    float  fres = pow(saturate(1.0f - dot(N,V)), 5.0f);
    float3 col = lerp(DeepColor.rgb, ShallowColor.rgb, fres);

    o.Albedo   = float4(col, 1.0f);
    o.Normal   = float4(N, 0.0f);
    o.PBR      = float4(Roughness, 0.0f, 1.0f, 1.0f);  // metallic=0 -> не emissive-флаг
    o.WorldPos = float4(i.PosWS, 1.0f);
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
    for (UINT i = 0; i < GBUFFER_RT_COUNT; ++i) blend.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

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

    // Каркасный вариант
    raster.FillMode = D3D12_FILL_MODE_WIREFRAME;
    raster.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState = raster;
    ThrowIfFailed(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&geometryWirePSO_)));
}
void RenderingSystem::CreateWaterPassRootSignature()
{
    D3D12_ROOT_PARAMETER params[1] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 1; desc.pParameters = params;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> sig, err;
    ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err));
    ThrowIfFailed(device_->CreateRootSignature(0, sig->GetBufferPointer(),
        sig->GetBufferSize(), IID_PPV_ARGS(&waterRootSig_)));
}

void RenderingSystem::CreateWaterPSO()
{
    auto vs = CompileShader(kWaterShaderSrc, "VSMain", "vs_5_0");
    auto hs = CompileShader(kWaterShaderSrc, "HSMain", "hs_5_0");
    auto ds = CompileShader(kWaterShaderSrc, "DSMain", "ds_5_0");
    auto ps = CompileShader(kWaterShaderSrc, "PSMain", "ps_5_0");

    D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,   0, 0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"NORMAL",  0,DXGI_FORMAT_R32G32B32_FLOAT,   0,12,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"COLOR",   0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,24,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,      0,40,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
    };
    D3D12_RASTERIZER_DESC raster = {};
    raster.FillMode = D3D12_FILL_MODE_SOLID;
    raster.CullMode = D3D12_CULL_MODE_NONE;
    raster.DepthClipEnable = TRUE;

    D3D12_DEPTH_STENCIL_DESC dss = {};
    dss.DepthEnable = TRUE;
    dss.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    dss.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

    D3D12_BLEND_DESC blend = {};
    for (UINT i = 0; i < GBUFFER_RT_COUNT; ++i)
        blend.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = waterRootSig_.Get();
    pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pso.HS = { hs->GetBufferPointer(), hs->GetBufferSize() };
    pso.DS = { ds->GetBufferPointer(), ds->GetBufferSize() };
    pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pso.InputLayout = { layout, _countof(layout) };
    pso.RasterizerState = raster;
    pso.DepthStencilState = dss;
    pso.BlendState = blend;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    pso.NumRenderTargets = GBUFFER_RT_COUNT;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pso.RTVFormats[2] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.RTVFormats[3] = DXGI_FORMAT_R32G32B32A32_FLOAT;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc = { 1,0 };
    ThrowIfFailed(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&waterPSO_)));
}

void RenderingSystem::BuildWater(const std::vector<Vertex>& verts, const std::vector<UINT>& idxs)
{
    indexCountWater_ = (UINT)idxs.size();
    MakeUploadBuffer(verts.data(), verts.size() * sizeof(Vertex), waterVB_);
    vbViewWater_.BufferLocation = waterVB_->GetGPUVirtualAddress();
    vbViewWater_.SizeInBytes = (UINT)(verts.size() * sizeof(Vertex));
    vbViewWater_.StrideInBytes = sizeof(Vertex);
    MakeUploadBuffer(idxs.data(), idxs.size() * sizeof(UINT), waterIB_);
    ibViewWater_.BufferLocation = waterIB_->GetGPUVirtualAddress();
    ibViewWater_.SizeInBytes = (UINT)(idxs.size() * sizeof(UINT));
    ibViewWater_.Format = DXGI_FORMAT_R32_UINT;
}

void RenderingSystem::UpdateWaterCB(const WaterCB& data)
{
    if (waterCBMapped_)
        memcpy(waterCBMapped_ + frameIndex_ * waterCBStride_, &data, sizeof(data));
}

void RenderingSystem::BindWaterPass()
{
    if (indexCountWater_ == 0) return;
    commandList_->SetGraphicsRootSignature(waterRootSig_.Get());
    commandList_->SetPipelineState(waterPSO_.Get());
    commandList_->SetGraphicsRootConstantBufferView(0,
        waterCB_->GetGPUVirtualAddress() + frameIndex_ * waterCBStride_);
    commandList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
    commandList_->IASetVertexBuffers(0, 1, &vbViewWater_);
    commandList_->IASetIndexBuffer(&ibViewWater_);
}

void RenderingSystem::DrawWater()
{
    if (indexCountWater_ == 0) return;
    commandList_->DrawIndexedInstanced(indexCountWater_, 1, 0, 0, 0);
}
static const char* kInstancedShaderSrc = R"HLSL(
cbuffer GeometryCB : register(b0)
{
    matrix   World; matrix View; matrix Proj;
    float4   CameraPos; float2 Tiling; float2 UVOffset;
    float    BlendFactor; float TessNear; float TessFar;
    float    TessMinLevel; float TessMaxLevel; float DisplacementScale;
    float    WireMode;
    float    CBPad;
};
Texture2D gAlbedo1:register(t0);
Texture2D gNormalMap:register(t2);
SamplerState gSampler:register(s0);

// World matrices of visible instances, indexed by SV_InstanceID, so CPU
// culling just changes the contents and the instance count.
StructuredBuffer<matrix> gInstances : register(t4);

struct VSInput { float3 Pos:POSITION; float3 Normal:NORMAL; float4 Color:COLOR; float2 TexCoord:TEXCOORD0; };
struct VSOut   { float4 Pos:SV_POSITION; float3 PosWS:TEXCOORD0; float3 Normal:TEXCOORD1; float2 UV:TEXCOORD2; };

VSOut VSMain(VSInput i, uint iid : SV_InstanceID)
{
    matrix W = gInstances[iid];
    VSOut o;
    float3 pw = mul(float4(i.Pos,1), W).xyz;
    o.PosWS  = pw;
    o.Normal = normalize(mul(float4(i.Normal,0), W).xyz);
    o.UV     = i.TexCoord;
    o.Pos    = mul(mul(float4(pw,1), View), Proj);
    return o;
}

struct PSOut { float4 Albedo:SV_Target0; float4 Normal:SV_Target1; float4 PBR:SV_Target2; float4 WorldPos:SV_Target3; };

PSOut PSMain(VSOut i)
{
    PSOut o;
    float2 uv = i.UV * Tiling + UVOffset;
    o.Albedo   = gAlbedo1.Sample(gSampler, uv);
    o.Normal   = float4(normalize(i.Normal), 0);
    o.PBR      = float4(0.5f, 0.0f, 1.0f, 1.0f);
    o.WorldPos = float4(i.PosWS, 1.0f);
    return o;
}
)HLSL";

static const char* kLightingShaderSrc = R"HLSL(
#define MAX_LIGHTS 16
#define LIGHT_DIR 0
#define LIGHT_PT  1
#define LIGHT_SPT 2
struct LightData{ float4 PositionWS; float4 DirectionWS; float4 Color; float Range; float SpotInnerCosine; float SpotOuterCosine; uint Type; };
#define MAX_CASCADES 4
cbuffer LightingCB:register(b0){
    float4 CameraPos; int LightCount; float3 LCBPad; LightData Lights[MAX_LIGHTS];
    matrix ShadowViewProj[MAX_CASCADES];
    float4 CascadeSplits;
    matrix CameraView;
    int    ShadowLightIndex; float ShadowMapSize; float ShadowBias; float DebugCascades;
};
Texture2D gAlbedo:register(t0); Texture2D gNormal:register(t1);
Texture2D gPBR:register(t2);    Texture2D gWorldPos:register(t3);
Texture2DArray gShadowMap:register(t4);
SamplerState gSampler:register(s0);
// Comparison sampler: the hardware compares depth and filters the
// comparison RESULT, not the depth values, so a single tap already
// yields a bilinearly smoothed transition.
SamplerComparisonState gShadowSampler:register(s1);
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
// Pick the cascade by view-space depth, then sample with PCF.
// PCF (percentage-closer filtering): instead of one comparison we take
// a grid of neighbouring texels and average the unshadowed fraction,
// which gives a soft edge instead of a stair-stepped one.
float SampleShadowPCF(float3 wp, float viewDepth)
{
    if (ShadowLightIndex < 0) return 1.0f;

    // Cascades are ordered by increasing far bound
    int c = 0;
    if (viewDepth > CascadeSplits.x) c = 1;
    if (viewDepth > CascadeSplits.y) c = 2;
    if (viewDepth > CascadeSplits.z) c = 3;
    if (viewDepth > CascadeSplits.w) return 1.0f;   // beyond the last cascade

    float4 lp = mul(float4(wp, 1.0f), ShadowViewProj[c]);
    lp.xyz /= lp.w;

    // NDC -> UV: y flips because texture coordinates grow downwards
    float2 uv = lp.xy * float2(0.5f, -0.5f) + 0.5f;
    if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f ||
        lp.z < 0.0f || lp.z > 1.0f)
        return 1.0f;                                 // outside the map: leave it lit

    // Slope-scaled bias is already set on the shadow pass rasterizer,
    // so only a small constant margin remains here.
    float depth = lp.z - ShadowBias;

    float texel = 1.0f / max(ShadowMapSize, 1.0f);
    float sum = 0.0f;

    // 5x5 grid = 25 taps. Constant bounds so it can be unrolled:
    // the compiler cannot unroll a dynamic loop bound.
    [unroll]
    for (int y = -2; y <= 2; ++y)
    {
        [unroll]
        for (int x = -2; x <= 2; ++x)
        {
            float2 o = float2(x, y) * texel;
            sum += gShadowMap.SampleCmpLevelZero(gShadowSampler,
                float3(uv + o, (float)c), depth);
        }
    }
    return sum / 25.0f;
}

float4 PSMain(FSQ i):SV_TARGET{
    float3 alb=gAlbedo.Sample(gSampler,i.UV).rgb;
    float3 nrm=gNormal.Sample(gSampler,i.UV).rgb;
    float4 pbr=gPBR.Sample(gSampler,i.UV);
    float rgh=pbr.r,ao=pbr.b;
    // Metallic channel doubles as an emissive flag: it takes no part in the
    // lighting maths, Sponza writes 0.1 and the scattered objects write 0.
    // Fire particles write 1.0 and are output directly instead of being lit,
    // which is how they glow without any blending.
    if (pbr.g > 0.5f)
    {
        float3 e = alb * 3.2f;
        return float4(e, 1.0f);
    }
    if(dot(nrm,nrm)<0.01f) return float4(0.02f,0.02f,0.05f,1);
    float3 N=normalize(nrm), wp=gWorldPos.Sample(gSampler,i.UV).rgb;
    float3 V=normalize(CameraPos.xyz-wp);
    float viewDepth = mul(float4(wp,1.0f), CameraView).z;   // depth used to pick the cascade
    float3 col=alb*0.08f*ao;
    for(int k=0;k<LightCount;++k){
        LightData l=Lights[k];
        if(l.Type==LIGHT_DIR){
            // Only the selected directional light casts shadows:
            // the cascades are built for its direction.
            float sh = (k==ShadowLightIndex) ? SampleShadowPCF(wp, viewDepth) : 1.0f;
            col+=BP(N,normalize(-l.DirectionWS.xyz),V,alb,rgh,l.Color.rgb*l.Color.w)*sh;
        }
        else if(l.Type==LIGHT_PT){ float3 tl=l.PositionWS.xyz-wp; float d=length(tl); if(d<l.Range) col+=BP(N,tl/d,V,alb,rgh,l.Color.rgb*l.Color.w)*Att(d,l.Range); }
        else if(l.Type==LIGHT_SPT){ float3 tl=l.PositionWS.xyz-wp; float d=length(tl); if(d<l.Range){ float3 L=tl/d; float ca=dot(-L,normalize(l.DirectionWS.xyz)); if(ca>l.SpotOuterCosine){ float sf=saturate((ca-l.SpotOuterCosine)/(l.SpotInnerCosine-l.SpotOuterCosine+0.0001f)); sf*=sf; col+=BP(N,L,V,alb,rgh,l.Color.rgb*l.Color.w)*Att(d,l.Range)*sf; } } }
    }
    // Debug mode 2: is anything written into the shadow map at all?
    // A cleared texel stays at exactly 1.0, so green means geometry was
    // rendered for this cascade and red means the map is empty.
    if (DebugCascades > 1.5f && ShadowLightIndex >= 0)
    {
        int sc = 0;
        if (viewDepth > CascadeSplits.x) sc = 1;
        if (viewDepth > CascadeSplits.y) sc = 2;
        if (viewDepth > CascadeSplits.z) sc = 3;
        float d = gShadowMap.SampleLevel(gSampler, float3(i.UV, (float)sc), 0).r;
        return float4(d < 0.999f ? float3(0,1,0) : float3(1,0,0), 1);
    }
    // Debug mode 1: tint each cascade so the split distribution is visible.
    if (DebugCascades > 0.5f && ShadowLightIndex >= 0)
    {
        int dc = 0;
        if (viewDepth > CascadeSplits.x) dc = 1;
        if (viewDepth > CascadeSplits.y) dc = 2;
        if (viewDepth > CascadeSplits.z) dc = 3;
        float3 tint = (dc==0) ? float3(1,0.4f,0.4f)
                    : (dc==1) ? float3(0.4f,1,0.4f)
                    : (dc==2) ? float3(0.4f,0.6f,1)
                              : float3(1,1,0.4f);
        if (viewDepth <= CascadeSplits.w) col = lerp(col, col*tint, 0.55f);
    }
    return float4(col,1);
}
)HLSL";


void RenderingSystem::CreateInstancedPSO()
{
    ComPtr<ID3DBlob> vs, ps, err;
    UINT flags = 0;
#ifdef _DEBUG
    flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    auto Compile = [&](const char* entry, const char* target, ComPtr<ID3DBlob>& out)
        {
            HRESULT hr = D3DCompile(kInstancedShaderSrc, strlen(kInstancedShaderSrc),
                nullptr, nullptr, nullptr, entry, target, flags, 0, &out, &err);
            if (FAILED(hr))
            {
                if (err) OutputDebugStringA((char*)err->GetBufferPointer());
                ThrowIfFailed(hr);
            }
        };
    Compile("VSMain", "vs_5_0", vs);
    Compile("PSMain", "ps_5_0", ps);

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
    for (UINT i = 0; i < GBUFFER_RT_COUNT; ++i) blend.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = geometryRootSig_.Get();
    pso.VS = { vs->GetBufferPointer(),vs->GetBufferSize() };
    pso.PS = { ps->GetBufferPointer(),ps->GetBufferSize() };
    pso.InputLayout = { layout,_countof(layout) };
    pso.RasterizerState = raster; pso.DepthStencilState = dss; pso.BlendState = blend;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = GBUFFER_RT_COUNT;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pso.RTVFormats[2] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.RTVFormats[3] = DXGI_FORMAT_R32G32B32A32_FLOAT;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT; pso.SampleDesc = { 1,0 };
    ThrowIfFailed(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&instancedPSO_)));
}

static const char* kTonemapShaderSrc = R"HLSL(
Texture2D gScene : register(t0);

struct VSOut { float4 Pos : SV_POSITION; };

VSOut VSMain(uint vid : SV_VertexID)
{
    float2 t = float2((vid << 1) & 2, vid & 2);
    VSOut o;
    o.Pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float4 PSMain(VSOut i) : SV_TARGET
{
    float3 c = gScene.Load(int3((int2)i.Pos.xy, 0)).rgb;
    c = c / (c + 1.0f);
    return float4(c, 1.0f);
}
)HLSL";
//не забудь
void RenderingSystem::CreateSceneHDR()
{
    sceneHDR_.Reset();

    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = (UINT)width_; d.Height = (UINT)height_;
    d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; d.SampleDesc = { 1,0 };
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE cv = {};
    cv.Format = d.Format;
    cv.Color[3] = 1.0f;

    ThrowIfFailed(device_->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &d,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cv, IID_PPV_ARGS(&sceneHDR_)));

    D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)(RS_FRAME_COUNT + GBUFFER_RT_COUNT) * rtvDescSize_;
    device_->CreateRenderTargetView(sceneHDR_.Get(), nullptr, h);
    sceneHdrRtv_ = h;

    CreateSRVInSlot(sceneHDR_.Get(), d.Format, SRV_SCENE_HDR);
}

void RenderingSystem::CreatePostRootSignature()
{
    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER param = {};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.DescriptorTable.NumDescriptorRanges = 1;
    param.DescriptorTable.pDescriptorRanges = &range;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 1; desc.pParameters = &param;

    ComPtr<ID3DBlob> sig, err;
    ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err));
    ThrowIfFailed(device_->CreateRootSignature(0, sig->GetBufferPointer(),
        sig->GetBufferSize(), IID_PPV_ARGS(&postRootSig_)));
}

void RenderingSystem::CreateTonemapPSO()
{
    auto vs = CompileShader(kTonemapShaderSrc, "VSMain", "vs_5_0");
    auto ps = CompileShader(kTonemapShaderSrc, "PSMain", "ps_5_0");

    D3D12_RASTERIZER_DESC raster = {};
    raster.FillMode = D3D12_FILL_MODE_SOLID; raster.CullMode = D3D12_CULL_MODE_NONE;
    D3D12_DEPTH_STENCIL_DESC dss = {};
    D3D12_BLEND_DESC blend = {};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = postRootSig_.Get();
    pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pso.InputLayout = { nullptr, 0 };
    pso.RasterizerState = raster; pso.DepthStencilState = dss; pso.BlendState = blend;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1; pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.DSVFormat = DXGI_FORMAT_UNKNOWN; pso.SampleDesc = { 1,0 };
    ThrowIfFailed(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&tonemapPSO_)));
}
//taa
static const char* kTaaShaderSrc = R"HLSL(
Texture2D gCurrent : register(t0);
Texture2D gHistory : register(t1);

cbuffer TaaCB : register(b0)
{
    float  Alpha;
    float  HistoryValid;
    float2 Pad;
};

struct VSOut { float4 Pos : SV_POSITION; };

VSOut VSMain(uint vid : SV_VertexID)
{
    float2 t = float2((vid << 1) & 2, vid & 2);
    VSOut o;
    o.Pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float4 PSMain(VSOut i) : SV_TARGET
{
    int2 p = int2(i.Pos.xy);
    float3 cur = gCurrent.Load(int3(p, 0)).rgb;

    if (HistoryValid < 0.5f)
        return float4(cur, 1.0f);

    float3 hist = gHistory.Load(int3(p, 0)).rgb;

    return float4(lerp(hist, cur, Alpha), 1.0f);
}
)HLSL";

void RenderingSystem::CreateTaaHistory()
{
    for (int i = 0; i < 2; ++i)
    {
        taaHist_[i].Reset();

        D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d = {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = (UINT)width_; d.Height = (UINT)height_;
        d.DepthOrArraySize = 1; d.MipLevels = 1;
        d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; d.SampleDesc = { 1,0 };
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE cv = {};
        cv.Format = d.Format;
        cv.Color[3] = 1.0f;

        ThrowIfFailed(device_->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &d,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cv, IID_PPV_ARGS(&taaHist_[i])));

        D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)(RS_FRAME_COUNT + GBUFFER_RT_COUNT + 1 + i) * rtvDescSize_;
        device_->CreateRenderTargetView(taaHist_[i].Get(), nullptr, h);
        taaRtv_[i] = h;

        CreateSRVInSlot(taaHist_[i].Get(), d.Format, SRV_TAA_HIST + i);
    }
    taaCur_ = 0;
    taaHistoryValid_ = false;
}

void RenderingSystem::CreateTaaRootSignature()
{
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1; ranges[0].BaseShaderRegister = 0;   // t0 = текущий кадр
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].NumDescriptors = 1; ranges[1].BaseShaderRegister = 1;   // t1 = история

    D3D12_ROOT_PARAMETER params[3] = {};
    for (int i = 0; i < 2; ++i)
    {
        params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[i].DescriptorTable.NumDescriptorRanges = 1;
        params[i].DescriptorTable.pDescriptorRanges = &ranges[i];
        params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].Constants.ShaderRegister = 0;
    params[2].Constants.RegisterSpace = 0;
    params[2].Constants.Num32BitValues = 4;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 3; desc.pParameters = params;

    ComPtr<ID3DBlob> sig, err;
    ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err));
    ThrowIfFailed(device_->CreateRootSignature(0, sig->GetBufferPointer(),
        sig->GetBufferSize(), IID_PPV_ARGS(&taaRootSig_)));
}

void RenderingSystem::CreateTaaPSO()
{
    auto vs = CompileShader(kTaaShaderSrc, "VSMain", "vs_5_0");
    auto ps = CompileShader(kTaaShaderSrc, "PSMain", "ps_5_0");

    D3D12_RASTERIZER_DESC raster = {};
    raster.FillMode = D3D12_FILL_MODE_SOLID; raster.CullMode = D3D12_CULL_MODE_NONE;
    D3D12_DEPTH_STENCIL_DESC dss = {};
    D3D12_BLEND_DESC blend = {};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = taaRootSig_.Get();
    pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pso.InputLayout = { nullptr, 0 };
    pso.RasterizerState = raster; pso.DepthStencilState = dss; pso.BlendState = blend;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1; pso.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;   // как и история
    pso.DSVFormat = DXGI_FORMAT_UNKNOWN; pso.SampleDesc = { 1,0 };
    ThrowIfFailed(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&taaPSO_)));
}

void RenderingSystem::UpdateInstances(const std::vector<XMFLOAT4X4>& visible,
    const std::vector<XMFLOAT4X4>& all)
{
    instanceCount_ = static_cast<UINT>(visible.size());
    if (instanceCount_ > RS_MAX_INSTANCES) instanceCount_ = RS_MAX_INSTANCES;
    shadowInstanceCount_ = static_cast<UINT>(all.size());
    if (shadowInstanceCount_ > RS_MAX_INSTANCES) shadowInstanceCount_ = RS_MAX_INSTANCES;
    if (!instanceMapped_) return;

    BYTE* base = instanceMapped_ + frameIndex_ * instanceStride_;
    if (instanceCount_)
        memcpy(base, visible.data(),
            static_cast<size_t>(instanceCount_) * sizeof(XMFLOAT4X4));
    if (shadowInstanceCount_)
        memcpy(base + SHADOW_INSTANCE_OFFSET, all.data(),
            static_cast<size_t>(shadowInstanceCount_) * sizeof(XMFLOAT4X4));
}

void RenderingSystem::BindInstancedPass()
{
    commandList_->SetPipelineState(instancedPSO_.Get());
    commandList_->SetGraphicsRootSignature(geometryRootSig_.Get());
    ID3D12DescriptorHeap* heaps[] = { srvHeap_.Get() };
    commandList_->SetDescriptorHeaps(1, heaps);

    commandList_->SetGraphicsRootConstantBufferView(0,
        geometryCB2_->GetGPUVirtualAddress() + frameIndex_ * geometryCB2Stride_);
    commandList_->SetGraphicsRootDescriptorTable(1, GpuHandle(mesh2SrvBase_ + 0)); // albedo
    commandList_->SetGraphicsRootDescriptorTable(2, GpuHandle(SRV_COMMON_ALBEDO2));
    commandList_->SetGraphicsRootDescriptorTable(3, GpuHandle(mesh2SrvBase_ + 2)); // normal
    commandList_->SetGraphicsRootDescriptorTable(4, GpuHandle(mesh2SrvBase_ + 3)); // disp (не используется)
    commandList_->SetGraphicsRootShaderResourceView(5,
        instanceBuffer_->GetGPUVirtualAddress() + frameIndex_ * instanceStride_);
}

void RenderingSystem::DrawInstances(UINT instanceCount)
{
    if (instanceCount == 0 || indexCount2_ == 0) return;
    if (instanceCount > instanceCount_) instanceCount = instanceCount_;

    commandList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList_->IASetVertexBuffers(0, 1, &vbView2_);
    commandList_->IASetIndexBuffer(&ibView2_);
    commandList_->DrawIndexedInstanced(indexCount2_, instanceCount, 0, 0, 0);
}

static const char* kShadowShaderSrc = R"HLSL(
cbuffer ShadowCB : register(b0)
{
    matrix LightViewProj;
    matrix World;
};
StructuredBuffer<matrix> gInstances : register(t4);

struct VSInput { float3 Pos:POSITION; float3 Normal:NORMAL; float4 Color:COLOR; float2 TexCoord:TEXCOORD0; };

// Sponza: one world matrix for the whole mesh.
float4 VSMain(VSInput i) : SV_POSITION
{
    return mul(mul(float4(i.Pos,1), World), LightViewProj);
}

// Scattered objects: per-instance matrix, same buffer as the main pass.
float4 VSInstanced(VSInput i, uint iid : SV_InstanceID) : SV_POSITION
{
    return mul(mul(float4(i.Pos,1), gInstances[iid]), LightViewProj);
}
)HLSL";

void RenderingSystem::CreateShadowPassRootSignature()
{
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[1].Descriptor.ShaderRegister = 4;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 2; desc.pParameters = params;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> sig, err;
    ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err));
    ThrowIfFailed(device_->CreateRootSignature(0, sig->GetBufferPointer(),
        sig->GetBufferSize(), IID_PPV_ARGS(&shadowRootSig_)));
}

void RenderingSystem::CreateShadowPSOs()
{
    ComPtr<ID3DBlob> vs, vsInst, err;
    UINT flags = 0;
#ifdef _DEBUG
    flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    auto Compile = [&](const char* entry, ComPtr<ID3DBlob>& out)
        {
            HRESULT hr = D3DCompile(kShadowShaderSrc, strlen(kShadowShaderSrc),
                nullptr, nullptr, nullptr, entry, "vs_5_0", flags, 0, &out, &err);
            if (FAILED(hr))
            {
                if (err) OutputDebugStringA((char*)err->GetBufferPointer());
                ThrowIfFailed(hr, "shadow VS");
            }
        };
    Compile("VSMain", vs);
    Compile("VSInstanced", vsInst);

    D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,   0, 0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"NORMAL",  0,DXGI_FORMAT_R32G32B32_FLOAT,   0,12,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"COLOR",   0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,24,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,      0,40,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
    };

    D3D12_RASTERIZER_DESC raster = {};
    raster.FillMode = D3D12_FILL_MODE_SOLID;
    raster.CullMode = D3D12_CULL_MODE_NONE;
    raster.DepthClipEnable = TRUE;
    raster.DepthBias = 3000;
    raster.SlopeScaledDepthBias = 3.0f;
    raster.DepthBiasClamp = 0.0f;

    D3D12_DEPTH_STENCIL_DESC dss = {};
    dss.DepthEnable = TRUE;
    dss.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    dss.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = shadowRootSig_.Get();
    pso.InputLayout = { layout,_countof(layout) };
    pso.RasterizerState = raster;
    pso.DepthStencilState = dss;
    pso.BlendState = {};
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 0;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc = { 1,0 };

    pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    ThrowIfFailed(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&shadowPSO_)));

    pso.VS = { vsInst->GetBufferPointer(), vsInst->GetBufferSize() };
    ThrowIfFailed(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&shadowInstancedPSO_)));
}

void RenderingSystem::BeginShadowCascade(int cascade, const XMFLOAT4X4& lightViewProj)
{
    if (!shadows_.Ready()) return;
    shadows_.BeginCascade(commandList_.Get(), cascade);

    struct ShadowCB { XMFLOAT4X4 LightViewProj; XMFLOAT4X4 World; } cb;
    cb.LightViewProj = lightViewProj;
    XMStoreFloat4x4(&cb.World, XMMatrixTranspose(XMMatrixIdentity()));
    XMMATRIX lvp = XMLoadFloat4x4(&lightViewProj);
    XMStoreFloat4x4(&cb.LightViewProj, XMMatrixTranspose(lvp));

    shadowCBSlot_ = frameIndex_ * CascadedShadowMap::MAX_CASCADES + cascade;
    if (shadowCBMapped_)
        memcpy(shadowCBMapped_ + shadowCBSlot_ * shadowCBStride_, &cb, sizeof(cb));

    commandList_->SetGraphicsRootSignature(shadowRootSig_.Get());
    commandList_->SetGraphicsRootConstantBufferView(0,
        shadowCB_->GetGPUVirtualAddress() + shadowCBSlot_ * shadowCBStride_);
    commandList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void RenderingSystem::DrawShadowMesh1()
{
    if (!shadows_.Ready() || indexCount_ == 0) return;
    commandList_->SetPipelineState(shadowPSO_.Get());
    commandList_->IASetVertexBuffers(0, 1, &vbView_);
    commandList_->IASetIndexBuffer(&ibView_);
    commandList_->DrawIndexedInstanced(indexCount_, 1, 0, 0, 0);
}

void RenderingSystem::DrawShadowMesh1Sub(const std::vector<int>& visible)
{
    if (!shadows_.Ready() || visible.empty()) return;
    commandList_->SetPipelineState(shadowPSO_.Get());
    commandList_->IASetVertexBuffers(0, 1, &vbView_);
    commandList_->IASetIndexBuffer(&ibView_);
    for (int i : visible)
    {
        if (i < 0 || i >= static_cast<int>(subDrawCalls_.size())) continue;
        const SubDrawCall& dc = subDrawCalls_[i];
        if (dc.indexCount == 0) continue;
        commandList_->DrawIndexedInstanced(dc.indexCount, 1, dc.indexStart, 0, 0);
    }
}

void RenderingSystem::DrawShadowInstances(UINT instanceCount)
{
    if (!shadows_.Ready() || instanceCount == 0 || indexCount2_ == 0) return;
    if (instanceCount > shadowInstanceCount_) instanceCount = shadowInstanceCount_;
    if (instanceCount == 0) return;

    commandList_->SetPipelineState(shadowInstancedPSO_.Get());
    commandList_->SetGraphicsRootShaderResourceView(1,
        instanceBuffer_->GetGPUVirtualAddress() + frameIndex_ * instanceStride_
        + SHADOW_INSTANCE_OFFSET);
    commandList_->IASetVertexBuffers(0, 1, &vbView2_);
    commandList_->IASetIndexBuffer(&ibView2_);
    commandList_->DrawIndexedInstanced(indexCount2_, instanceCount, 0, 0, 0);
}

void RenderingSystem::SimulateParticles(const ParticleCB& cb)
{
    particles_.Simulate(commandList_.Get(), srvHeap_.Get(), cb);
}

void RenderingSystem::DrawParticles()
{
    particles_.Render(commandList_.Get(), srvHeap_.Get());
}

void RenderingSystem::EndShadowPass()
{
    if (!shadows_.Ready()) return;
    shadows_.EndPass(commandList_.Get());

    commandList_->OMSetRenderTargets(GBUFFER_RT_COUNT, gbuffer_.GetRTVHandles(),
        FALSE, &dsvHandle_);
    D3D12_VIEWPORT vp = { 0,0,(float)width_,(float)height_,0,1 };
    D3D12_RECT sr = { 0,0,width_,height_ };
    commandList_->RSSetViewports(1, &vp);
    commandList_->RSSetScissorRects(1, &sr);
}

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
    pso.NumRenderTargets = 1; pso.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pso.DSVFormat = DXGI_FORMAT_UNKNOWN; pso.SampleDesc = { 1,0 };
    ThrowIfFailed(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&lightingPSO_)));
}

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

int RenderingSystem::LoadAndCacheDisplacement(const std::wstring& dispPath,
    const std::wstring& normalPath)
{
    const std::wstring key = !dispPath.empty() ? dispPath
        : (normalPath.empty() ? std::wstring() : L"@height:" + normalPath);
    if (key.empty()) return SRV_FALLBACK_DISP;

    auto it = texCache_.find(key);
    if (it != texCache_.end()) return it->second;

    TextureData td;
    if (!dispPath.empty()) td = LoadTextureAuto(dispPath);

    if (!td.valid && !normalPath.empty())
    {
        TextureData nm = LoadTextureAuto(normalPath);
        if (nm.valid)
        {
            LoadTimer t("  восстановление карты высот");
            td = CreateHeightFromNormal(nm);
        }
    }
    if (!td.valid) return SRV_FALLBACK_DISP;

    const int slot = UploadTextureToHeap(td);
    texCache_[key] = slot;
    return slot;
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
    for (UINT row = 0; row < td.height; ++row)
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

void RenderingSystem::BuildMesh1(
    const std::vector<Vertex>& verts, const std::vector<UINT>& idxs,
    const std::vector<UINT>& subIndexStarts, const std::vector<UINT>& subIndexCounts,
    const std::vector<std::wstring>& subDiffuse, const std::vector<std::wstring>& subNormal,
    const std::vector<std::wstring>& subDisp)
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
        subDrawCalls_[i].srvDiffuse = LoadAndCacheTexture(subDiffuse[i], false);
        subDrawCalls_[i].srvNormal = LoadAndCacheTexture(subNormal[i], true);
        subDrawCalls_[i].srvDisp = LoadAndCacheDisplacement(
            i < subDisp.size() ? subDisp[i] : std::wstring(), subNormal[i]);
    }
}

void RenderingSystem::BuildBuffers2(const std::vector<Vertex>& verts, const std::vector<UINT>& idxs)
{
    indexCount2_ = (UINT)idxs.size();
    MakeUploadBuffer(verts.data(), verts.size() * sizeof(Vertex), vertexBuffer2_);
    vbView2_.BufferLocation = vertexBuffer2_->GetGPUVirtualAddress();
    vbView2_.SizeInBytes = (UINT)(verts.size() * sizeof(Vertex)); vbView2_.StrideInBytes = sizeof(Vertex);
    MakeUploadBuffer(idxs.data(), idxs.size() * sizeof(UINT), indexBuffer2_);
    ibView2_.BufferLocation = indexBuffer2_->GetGPUVirtualAddress();
    ibView2_.SizeInBytes = (UINT)(idxs.size() * sizeof(UINT)); ibView2_.Format = DXGI_FORMAT_R32_UINT;

    mesh2SrvBase_ = srvNextFree_;
    srvNextFree_ += 4;
    if (srvNextFree_ > SRV_HEAP_SIZE) ThrowIfFailed(E_FAIL, "SRV heap overflow (mesh2)");

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

void RenderingSystem::CreateConstantBuffers()
{
    auto Align256 = [](UINT64 n) {return (n + 255) & ~UINT64(255); };
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
    geometryCBStride_ = Align256(sizeof(ConstantBufferData));
    geometryCB2Stride_ = Align256(sizeof(ConstantBufferData));
    lightingCBStride_ = Align256(sizeof(LightingPassCB));

    make(geometryCBStride_ * RS_FRAME_COUNT, geometryCB_);
    ThrowIfFailed(geometryCB_->Map(0, nullptr, reinterpret_cast<void**>(&geometryCBMapped_)));
    make(geometryCB2Stride_ * RS_FRAME_COUNT, geometryCB2_);
    ThrowIfFailed(geometryCB2_->Map(0, nullptr, reinterpret_cast<void**>(&geometryCB2Mapped_)));
    make(lightingCBStride_ * RS_FRAME_COUNT, lightingCBRes_);
    ThrowIfFailed(lightingCBRes_->Map(0, nullptr, reinterpret_cast<void**>(&lightingCBMapped_)));

    instanceStride_ = Align256(sizeof(XMFLOAT4X4) * RS_MAX_INSTANCES * 2);
    make(instanceStride_ * RS_FRAME_COUNT, instanceBuffer_);
    ThrowIfFailed(instanceBuffer_->Map(0, nullptr, reinterpret_cast<void**>(&instanceMapped_)));

    shadowCBStride_ = Align256(sizeof(XMFLOAT4X4) * 2);
    make(shadowCBStride_ * RS_FRAME_COUNT * CascadedShadowMap::MAX_CASCADES, shadowCB_);
    ThrowIfFailed(shadowCB_->Map(0, nullptr, reinterpret_cast<void**>(&shadowCBMapped_)));

    waterCBStride_ = Align256(sizeof(WaterCB));
    make(waterCBStride_ * RS_FRAME_COUNT, waterCB_);
    ThrowIfFailed(waterCB_->Map(0, nullptr, reinterpret_cast<void**>(&waterCBMapped_)));

    terrainCBStride_ = Align256(sizeof(TerrainCB));
    make(terrainCBStride_ * RS_FRAME_COUNT * MAX_TERRAIN_TILES, terrainCB_);
    ThrowIfFailed(terrainCB_->Map(0, nullptr, reinterpret_cast<void**>(&terrainCBMapped_)));

    shadows_.Create(device_.Get(), srvHeap_.Get(), SRV_SHADOWMAP, srvDescSize_);
    particles_.Create(device_.Get(), srvHeap_.Get(), SRV_PARTICLES, srvDescSize_);
}

void RenderingSystem::UpdateGeometryPassCB(const ConstantBufferData& data)
{
    if (geometryCBMapped_)
        memcpy(geometryCBMapped_ + frameIndex_ * geometryCBStride_, &data, sizeof(data));
}
void RenderingSystem::UpdateGeometryPassCB2(const ConstantBufferData& data)
{
    if (geometryCB2Mapped_)
        memcpy(geometryCB2Mapped_ + frameIndex_ * geometryCB2Stride_, &data, sizeof(data));
}
void RenderingSystem::UpdateLightingPassCB(const LightingPassCB& data)
{
    lightingCB_ = data;
    if (lightingCBMapped_)
        memcpy(lightingCBMapped_ + frameIndex_ * lightingCBStride_, &data, sizeof(data));
}

void RenderingSystem::ClearLights() { lightingCB_.LightCount = 0; }
void RenderingSystem::AddDirectionalLight(XMFLOAT3 dir, XMFLOAT3 col, float intens) {
    if (lightingCB_.LightCount >= MAX_LIGHTS)return;
    LightData& l = lightingCB_.Lights[lightingCB_.LightCount++];
    l.PositionWS = { 0,0,0,0 }; l.DirectionWS = { dir.x,dir.y,dir.z,0 };
    l.Color = { col.x,col.y,col.z,intens }; l.Range = l.SpotInnerCosine = l.SpotOuterCosine = 0;
    l.Type = (UINT)LightType::Directional;
}
void RenderingSystem::AddPointLight(XMFLOAT3 pos, XMFLOAT3 col, float intens, float range) {
    if (lightingCB_.LightCount >= MAX_LIGHTS)return;
    LightData& l = lightingCB_.Lights[lightingCB_.LightCount++];
    l.PositionWS = { pos.x,pos.y,pos.z,1 }; l.DirectionWS = { 0,0,0,0 };
    l.Color = { col.x,col.y,col.z,intens }; l.Range = range; l.SpotInnerCosine = l.SpotOuterCosine = 0;
    l.Type = (UINT)LightType::Point;
}
void RenderingSystem::AddSpotLight(XMFLOAT3 pos, XMFLOAT3 dir, XMFLOAT3 col, float intens, float range, float iDeg, float oDeg) {
    if (lightingCB_.LightCount >= MAX_LIGHTS)return;
    const float D2R = 3.14159265f / 180.f;
    LightData& l = lightingCB_.Lights[lightingCB_.LightCount++];
    l.PositionWS = { pos.x,pos.y,pos.z,1 }; l.DirectionWS = { dir.x,dir.y,dir.z,0 };
    l.Color = { col.x,col.y,col.z,intens }; l.Range = range;
    l.SpotInnerCosine = cosf(iDeg * D2R); l.SpotOuterCosine = cosf(oDeg * D2R);
    l.Type = (UINT)LightType::Spot;
}

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
    commandList_->SetPipelineState(
        (wireframe_ && geometryWirePSO_) ? geometryWirePSO_.Get() : geometryPSO_.Get());
    ID3D12DescriptorHeap* heaps[] = { srvHeap_.Get() };
    commandList_->SetDescriptorHeaps(1, heaps);
    commandList_->SetGraphicsRootConstantBufferView(0,
        geometryCB_->GetGPUVirtualAddress() + frameIndex_ * geometryCBStride_);
    commandList_->SetGraphicsRootDescriptorTable(2, GpuHandle(SRV_COMMON_ALBEDO2));
    commandList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
    commandList_->IASetVertexBuffers(0, 1, &vbView_);
    commandList_->IASetIndexBuffer(&ibView_);
}

void RenderingSystem::DrawMesh1SubMeshes(const std::vector<int>* visible)
{
    auto Draw = [this](const SubDrawCall& dc)
        {
            if (dc.indexCount == 0) return;
            commandList_->SetGraphicsRootDescriptorTable(1, GpuHandle(dc.srvDiffuse));
            commandList_->SetGraphicsRootDescriptorTable(3, GpuHandle(dc.srvNormal));
            commandList_->SetGraphicsRootDescriptorTable(4, GpuHandle(dc.srvDisp));
            commandList_->DrawIndexedInstanced(dc.indexCount, 1, dc.indexStart, 0, 0);
        };

    if (!visible)
    {
        for (const auto& dc : subDrawCalls_) Draw(dc);
        return;
    }
    for (int i : *visible)
        if (i >= 0 && i < static_cast<int>(subDrawCalls_.size()))
            Draw(subDrawCalls_[i]);
}

void RenderingSystem::BindGeometryPassMesh2()
{
    commandList_->SetGraphicsRootConstantBufferView(0,
        geometryCB2_->GetGPUVirtualAddress() + frameIndex_ * geometryCB2Stride_);
    commandList_->SetGraphicsRootDescriptorTable(1, GpuHandle(mesh2SrvBase_ + 0));
    commandList_->SetGraphicsRootDescriptorTable(2, GpuHandle(mesh2SrvBase_ + 1));
    commandList_->SetGraphicsRootDescriptorTable(3, GpuHandle(mesh2SrvBase_ + 2));
    commandList_->SetGraphicsRootDescriptorTable(4, GpuHandle(mesh2SrvBase_ + 3));
}

void RenderingSystem::DrawMesh2()
{
    commandList_->IASetVertexBuffers(0, 1, &vbView2_);
    commandList_->IASetIndexBuffer(&ibView2_);
    commandList_->DrawIndexedInstanced(indexCount2_, 1, 0, 0, 0);
}

static D3D12_RESOURCE_BARRIER MakeTransition(ID3D12Resource* res,
    D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    return b;
}

void RenderingSystem::BeginLightingPass()
{
    gbuffer_.TransitionTo(commandList_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    D3D12_RESOURCE_BARRIER b = MakeTransition(sceneHDR_.Get(),
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    commandList_->ResourceBarrier(1, &b);
    const float cc[] = { 0,0,0,1 };
    commandList_->ClearRenderTargetView(sceneHdrRtv_, cc, 0, nullptr);
    commandList_->OMSetRenderTargets(1, &sceneHdrRtv_, FALSE, nullptr);

    if (lightingCBMapped_)
        memcpy(lightingCBMapped_ + frameIndex_ * lightingCBStride_, &lightingCB_, sizeof(lightingCB_));
    commandList_->SetGraphicsRootSignature(lightingRootSig_.Get());
    commandList_->SetPipelineState(lightingPSO_.Get());
    ID3D12DescriptorHeap* heaps[] = { srvHeap_.Get() };
    commandList_->SetDescriptorHeaps(1, heaps);
    commandList_->SetGraphicsRootConstantBufferView(0,
        lightingCBRes_->GetGPUVirtualAddress() + frameIndex_ * lightingCBStride_);
    commandList_->SetGraphicsRootDescriptorTable(1, gbuffer_.GetSRVGpuStart());
    if (shadows_.Ready())
        commandList_->SetGraphicsRootDescriptorTable(2, shadows_.SrvGpu());
    commandList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList_->IASetVertexBuffers(0, 0, nullptr);
    commandList_->IASetIndexBuffer(nullptr);
}

void RenderingSystem::DrawLightingQuad() { commandList_->DrawInstanced(6, 1, 0, 0); }
//обновлено
void RenderingSystem::EndFrame()
{
    D3D12_VIEWPORT vp = { 0,0,(float)width_,(float)height_,0,1 };
    D3D12_RECT sr = { 0,0,width_,height_ };
    ID3D12DescriptorHeap* heaps[] = { srvHeap_.Get() };

    int tonemapSrc = SRV_SCENE_HDR;

    if (taaEnabled_)
    {
        const int cur = taaCur_;       //сюда пишем :steamhappy:
        const int prev = 1 - cur;      //отсюда читаем историю :steamsad:

        D3D12_RESOURCE_BARRIER b1[2] = {
            MakeTransition(sceneHDR_.Get(),
                D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            MakeTransition(taaHist_[cur].Get(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET)
        };
        commandList_->ResourceBarrier(2, b1);

        commandList_->OMSetRenderTargets(1, &taaRtv_[cur], FALSE, nullptr);
        commandList_->RSSetViewports(1, &vp);
        commandList_->RSSetScissorRects(1, &sr);
        commandList_->SetGraphicsRootSignature(taaRootSig_.Get());
        commandList_->SetPipelineState(taaPSO_.Get());
        commandList_->SetDescriptorHeaps(1, heaps);
        commandList_->SetGraphicsRootDescriptorTable(0, GpuHandle(SRV_SCENE_HDR));
        commandList_->SetGraphicsRootDescriptorTable(1, GpuHandle(SRV_TAA_HIST + prev));
        const float taaConsts[4] = { 0.1f, taaHistoryValid_ ? 1.0f : 0.0f, 0.0f, 0.0f };
        commandList_->SetGraphicsRoot32BitConstants(2, 4, taaConsts, 0);
        commandList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commandList_->DrawInstanced(3, 1, 0, 0);

        D3D12_RESOURCE_BARRIER b2 = MakeTransition(taaHist_[cur].Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        commandList_->ResourceBarrier(1, &b2);

        tonemapSrc = SRV_TAA_HIST + cur;
        taaHistoryValid_ = true;
        taaCur_ = prev;
    }
    else
    {
        D3D12_RESOURCE_BARRIER b = MakeTransition(sceneHDR_.Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        commandList_->ResourceBarrier(1, &b);
        taaHistoryValid_ = false;
    }

    D3D12_RESOURCE_BARRIER toBack = MakeTransition(renderTargets_[frameIndex_].Get(),
        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    commandList_->ResourceBarrier(1, &toBack);

    commandList_->OMSetRenderTargets(1, &rtvHandles_[frameIndex_], FALSE, nullptr);
    commandList_->RSSetViewports(1, &vp);
    commandList_->RSSetScissorRects(1, &sr);
    commandList_->SetGraphicsRootSignature(postRootSig_.Get());
    commandList_->SetPipelineState(tonemapPSO_.Get());
    commandList_->SetDescriptorHeaps(1, heaps);
    commandList_->SetGraphicsRootDescriptorTable(0, GpuHandle(tonemapSrc));
    commandList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList_->DrawInstanced(3, 1, 0, 0);

    D3D12_RESOURCE_BARRIER b = MakeTransition(renderTargets_[frameIndex_].Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
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

//Terrain!!!

static const char* kTerrainShaderSrc = R"HLSL(
cbuffer TerrainCB : register(b0)
{
    matrix View;
    matrix Proj;
    float4 CameraPos;
    float  OriginX;
    float  OriginZ;
    float  OriginY;
    float  HeightScale;
    float  WireMode;
    float  MapSize;
    float  LodBoost;
    float  MaxLevel;
};
cbuffer NodeCB : register(b1)
{
    float NodeX;
    float NodeZ;
    float NodeSize;
    float NodeLevel;
};

Texture2D<float> gHeight : register(t0);

struct VSInput { float3 Pos:POSITION; float3 Normal:NORMAL; float4 Color:COLOR; float2 TexCoord:TEXCOORD0; };
struct VSOut   { float4 Pos:SV_POSITION; float3 PosWS:TEXCOORD0; float3 Normal:TEXCOORD1; float Level:TEXCOORD2; };

// Высота 0..1 в клетке (x, z). Load вместо Sample: без фильтрации, точное значение.
float HeightAt(int x, int z)
{
    int last = (int)MapSize - 1;
    return gHeight.Load(int3(clamp(x, 0, last), clamp(z, 0, last), 0));
}

VSOut VSMain(VSInput i)
{
    VSOut o;
    float cx = NodeX + i.Pos.x * NodeSize;
    float cz = NodeZ + i.Pos.z * NodeSize;
    int ix = (int)(cx + 0.5f);
    int iz = (int)(cz + 0.5f);

    float h = HeightAt(ix, iz);

    // нормаль по соседним клеткам; у крупных узлов шаг больше
    int step = max(1, (int)(NodeSize / 32.0f + 0.5f));
    float hL = HeightAt(ix - step, iz);
    float hR = HeightAt(ix + step, iz);
    float hD = HeightAt(ix, iz - step);
    float hU = HeightAt(ix, iz + step);

    float boost = 1.0f + LodBoost * (MaxLevel - NodeLevel) / max(MaxLevel, 1.0f);
    float hScale = HeightScale * boost;
//баянчики4
    float3 posWS = float3(cx + OriginX, h * hScale + OriginY, cz + OriginZ);

    o.PosWS  = posWS;
    o.Normal = normalize(float3((hL - hR) * hScale, 2.0f * step, (hD - hU) * hScale));
    o.Level  = NodeLevel;
    o.Pos    = mul(mul(float4(posWS, 1.0f), View), Proj);
    return o;
}

struct PSOut { float4 Albedo:SV_Target0; float4 Normal:SV_Target1; float4 PBR:SV_Target2; float4 WorldPos:SV_Target3; };

static const float3 kLevelColor[6] = {
    float3(1.00f, 0.20f, 0.20f),
    float3(1.00f, 0.60f, 0.10f),
    float3(0.95f, 0.90f, 0.20f),
    float3(0.30f, 0.80f, 0.30f),
    float3(0.20f, 0.80f, 0.90f),
    float3(0.30f, 0.40f, 1.00f)
};

PSOut PSMain(VSOut i)
{
    PSOut o;
    int lv = clamp((int)(i.Level + 0.5f), 0, 5);
    float3 col = kLevelColor[lv];

    o.Albedo = float4(col, 1.0f);
    o.Normal = float4(normalize(i.Normal), 0.0f);
    if (WireMode > 0.5f)
        o.PBR = float4(0.6f, 1.0f, 1.0f, 1.0f);   // metallic>0.5 = эмиссия
    else
        o.PBR = float4(0.9f, 0.0f, 1.0f, 1.0f);
    o.WorldPos = float4(i.PosWS, 1.0f);
    return o;
}
)HLSL";

void RenderingSystem::CreateTerrainRootSignature()
{
    D3D12_DESCRIPTOR_RANGE heightRange = {};
    heightRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    heightRange.NumDescriptors = 1;
    heightRange.BaseShaderRegister = 0;      // t0
    heightRange.RegisterSpace = 0;
    heightRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;            // b0
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; // b1
    params[1].Constants.ShaderRegister = 1;
    params[1].Constants.RegisterSpace = 0;
    params[1].Constants.Num32BitValues = 4;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; // t0
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &heightRange;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 3; desc.pParameters = params;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> sig, err;
    ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err));
    ThrowIfFailed(device_->CreateRootSignature(0, sig->GetBufferPointer(),
        sig->GetBufferSize(), IID_PPV_ARGS(&terrainRootSig_)));
}

void RenderingSystem::CreateTerrainPSO()
{
    auto vs = CompileShader(kTerrainShaderSrc, "VSMain", "vs_5_0");
    auto ps = CompileShader(kTerrainShaderSrc, "PSMain", "ps_5_0");

    D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,   0, 0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"NORMAL",  0,DXGI_FORMAT_R32G32B32_FLOAT,   0,12,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"COLOR",   0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,24,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,      0,40,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
    };
    D3D12_RASTERIZER_DESC raster = {};
    raster.FillMode = D3D12_FILL_MODE_SOLID;
    raster.CullMode = D3D12_CULL_MODE_NONE;
    raster.DepthClipEnable = TRUE;

    D3D12_DEPTH_STENCIL_DESC dss = {};
    dss.DepthEnable = TRUE;
    dss.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    dss.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

    D3D12_BLEND_DESC blend = {};
    for (UINT i = 0; i < GBUFFER_RT_COUNT; ++i)
        blend.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = terrainRootSig_.Get();
    pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pso.InputLayout = { layout, _countof(layout) };
    pso.RasterizerState = raster;
    pso.DepthStencilState = dss;
    pso.BlendState = blend;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = GBUFFER_RT_COUNT;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pso.RTVFormats[2] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.RTVFormats[3] = DXGI_FORMAT_R32G32B32A32_FLOAT;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc = { 1,0 };
    ThrowIfFailed(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&terrainPSO_)));

    raster.FillMode = D3D12_FILL_MODE_WIREFRAME;
    pso.RasterizerState = raster;
    ThrowIfFailed(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&terrainWirePSO_)));
}

void RenderingSystem::BuildTerrainGrid()
{
    const int N = 32;   // 32x32 cells = 33x33 vertices

    std::vector<Vertex> verts;
    std::vector<UINT>   idxs;
    verts.reserve((N + 1) * (N + 1));

    for (int z = 0; z <= N; ++z)
    {
        for (int x = 0; x <= N; ++x)
        {
            Vertex v = {};
            v.Position = { float(x) / N, 0.0f, float(z) / N };
            v.Normal = { 0.0f, 1.0f, 0.0f };
            v.Color = { 1.0f, 1.0f, 1.0f, 1.0f };
            v.TexCoord = { float(x) / N, float(z) / N };
            verts.push_back(v);
        }
    }

    auto IDX = [&](int x, int z) { return (UINT)(z * (N + 1) + x); };
    for (int z = 0; z < N; ++z)
    {
        for (int x = 0; x < N; ++x)
        {
            UINT a = IDX(x, z), b = IDX(x + 1, z), c = IDX(x + 1, z + 1), d = IDX(x, z + 1);
            idxs.insert(idxs.end(), { a, b, c, a, c, d });
        }
    }

    indexCountTerrain_ = (UINT)idxs.size();
    MakeUploadBuffer(verts.data(), verts.size() * sizeof(Vertex), terrainVB_);
    vbViewTerrain_.BufferLocation = terrainVB_->GetGPUVirtualAddress();
    vbViewTerrain_.SizeInBytes = (UINT)(verts.size() * sizeof(Vertex));
    vbViewTerrain_.StrideInBytes = sizeof(Vertex);
    MakeUploadBuffer(idxs.data(), idxs.size() * sizeof(UINT), terrainIB_);
    ibViewTerrain_.BufferLocation = terrainIB_->GetGPUVirtualAddress();
    ibViewTerrain_.SizeInBytes = (UINT)(idxs.size() * sizeof(UINT));
    ibViewTerrain_.Format = DXGI_FORMAT_R32_UINT;
}

void RenderingSystem::UpdateTerrainCB(int tile, const TerrainCB& data)
{
    if (!terrainCBMapped_ || tile < 0 || tile >= MAX_TERRAIN_TILES) return;
    memcpy(terrainCBMapped_ + ((size_t)frameIndex_ * MAX_TERRAIN_TILES + tile) * terrainCBStride_,
        &data, sizeof(data));
}

void RenderingSystem::BindTerrainPass()
{
    if (indexCountTerrain_ == 0 || terrainTileCount_ == 0) return;
    commandList_->SetGraphicsRootSignature(terrainRootSig_.Get());
    commandList_->SetPipelineState(
        (wireframe_ && terrainWirePSO_) ? terrainWirePSO_.Get() : terrainPSO_.Get());
    ID3D12DescriptorHeap* heaps[] = { srvHeap_.Get() };
    commandList_->SetDescriptorHeaps(1, heaps);
    commandList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList_->IASetVertexBuffers(0, 1, &vbViewTerrain_);
    commandList_->IASetIndexBuffer(&ibViewTerrain_);
}

void RenderingSystem::BindTerrainTile(int tile)
{
    if (indexCountTerrain_ == 0 || tile < 0 || tile >= terrainTileCount_) return;
    commandList_->SetGraphicsRootConstantBufferView(0,
        terrainCB_->GetGPUVirtualAddress()
        + ((UINT64)frameIndex_ * MAX_TERRAIN_TILES + tile) * terrainCBStride_);
    commandList_->SetGraphicsRootDescriptorTable(2, GpuHandle(terrainHeightSlot_[tile]));
}

void RenderingSystem::DrawTerrainNode(float x, float z, float size, float level)
{
    if (indexCountTerrain_ == 0 || terrainTileCount_ == 0) return;
    const float values[4] = { x, z, size, level };
    commandList_->SetGraphicsRoot32BitConstants(1, 4, values, 0);
    commandList_->DrawIndexedInstanced(indexCountTerrain_, 1, 0, 0, 0);
}

int RenderingSystem::CreateTerrainHeightmap(const std::vector<float>& heights, int mapSize)
{
    if (terrainTileCount_ >= MAX_TERRAIN_TILES)
        ThrowIfFailed(E_FAIL, "too many terrain tiles");
    if (mapSize <= 0 || (int)heights.size() != mapSize * mapSize)
        ThrowIfFailed(E_FAIL, "CreateTerrainHeightmap: bad size");
    if (srvNextFree_ >= SRV_HEAP_SIZE)
        ThrowIfFailed(E_FAIL, "SRV heap overflow");

    std::vector<uint16_t> texels(heights.size());
    for (size_t i = 0; i < heights.size(); ++i)
    {
        float h = heights[i];
        if (h < 0.0f) h = 0.0f;
        if (h > 1.0f) h = 1.0f;
        texels[i] = (uint16_t)(h * 65535.0f + 0.5f);
    }

    WaitForGPU();

    D3D12_HEAP_PROPERTIES defHP = {}; defHP.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = mapSize; texDesc.Height = mapSize;
    texDesc.DepthOrArraySize = 1; texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R16_UNORM; texDesc.SampleDesc = { 1,0 };

    ComPtr<ID3D12Resource> tex, upload;
    ThrowIfFailed(device_->CreateCommittedResource(&defHP, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex)));

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    UINT64 uploadSize = 0;
    device_->GetCopyableFootprints(&texDesc, 0, 1, 0, &fp, nullptr, nullptr, &uploadSize);

    D3D12_HEAP_PROPERTIES upHP = {}; upHP.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC upDesc = {};
    upDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; upDesc.Width = uploadSize;
    upDesc.Height = upDesc.DepthOrArraySize = upDesc.MipLevels = 1;
    upDesc.Format = DXGI_FORMAT_UNKNOWN; upDesc.SampleDesc = { 1,0 };
    upDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ThrowIfFailed(device_->CreateCommittedResource(&upHP, D3D12_HEAP_FLAG_NONE, &upDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)));

    BYTE* mapped = nullptr;
    ThrowIfFailed(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
    for (int row = 0; row < mapSize; ++row)
        memcpy(mapped + (size_t)row * fp.Footprint.RowPitch,
            texels.data() + (size_t)row * mapSize, (size_t)mapSize * sizeof(uint16_t));
    upload->Unmap(0, nullptr);

    ThrowIfFailed(commandAllocators_[0]->Reset());
    ThrowIfFailed(commandList_->Reset(commandAllocators_[0].Get(), nullptr));

    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = tex.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = upload.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = fp;
    commandList_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = tex.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    commandList_->ResourceBarrier(1, &b);

    ThrowIfFailed(commandList_->Close());
    ID3D12CommandList* lists[] = { commandList_.Get() };
    commandQueue_->ExecuteCommandLists(1, lists);
    WaitForGPU();

    int tile = terrainTileCount_++;
    terrainHeightSlot_[tile] = srvNextFree_++;
    CreateSRVInSlot(tex.Get(), DXGI_FORMAT_R16_UNORM, terrainHeightSlot_[tile]);
    terrainHeightTex_[tile] = tex;
    return tile;
}