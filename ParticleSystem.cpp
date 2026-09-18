#include "ParticleSystem.h"
#include "Utils.h"
#include "GBuffer.h"
#include <d3dcompiler.h>
#include <cstring>

#pragma comment(lib, "d3dcompiler.lib")

// Счётчик UAV обязан лежать по адресу, кратному 4096
static const UINT64 COUNTER_STRIDE = D3D12_UAV_COUNTER_PLACEMENT_ALIGNMENT; // 4096

// Раскладка буфера аргументов (по 4 байта на элемент):
//   [0..2] Dispatch: ThreadGroupCountX/Y/Z
//   [3]    число живых частиц - страховка для последней группы Update
//   [4..7] Draw: VertexCountPerInstance, InstanceCount, StartVertex, StartInstance
static const UINT64 ARGS_DISPATCH_OFFSET = 0;
static const UINT64 ARGS_DRAW_OFFSET = 16;
static const UINT64 ARGS_SIZE = 32;

// Локальные номера дескрипторов внутри выделенного блока.
// Первые три - тот же приём, что и с пинг-понгом матриц: биндя таблицу
// со смещением 0 получаем (src=A, dst=B), со смещением 1 - (src=B, dst=A).
enum : int
{
    D_UAV_A0 = 0,   // UAV буфера A
    D_UAV_B = 1,   // UAV буфера B
    D_UAV_A1 = 2,   // UAV буфера A ещё раз
    D_UAV_ARGS = 3,
    D_UAV_COUNTERS = 4,
    D_SRV_A = 5,
    D_SRV_B = 6,
};

// ============================================================
//  Шейдеры. Только ASCII: исходник уходит прямо в D3DCompile.
// ============================================================
static const char* kParticleComputeSrc = R"HLSL(
struct Particle
{
    float3 Position; float Life;
    float3 Velocity; float Size;
    float4 Color;
    float MaxLife; float Kind; float Rotation; float Seed;
};

#define KIND_FIRE  0.0f
#define KIND_SMOKE 1.0f

cbuffer ParticleCB : register(b0)
{
    matrix View; matrix Proj;
    float4 CameraRight; float4 CameraUp; float4 CameraForward;
    float4 EmitOrigin;  float4 Gravity;
    float DeltaTime; float TotalTime; uint EmitCount; float EmitSpeed;
    float LifeMin;   float LifeSpan;  float SizeMin;  float SizeSpan;
    uint  SrcCounterIndex; float Turbulence; float Buoyancy; float SmokeLife;
    float4 SmokeParams;
};

ConsumeStructuredBuffer<Particle> SrcParticles : register(u0);
AppendStructuredBuffer<Particle>  DstParticles : register(u1);

RWBuffer<uint> Args     : register(u2);
RWBuffer<uint> Counters : register(u3);

#define SAVED_COUNT_INDEX 2048

uint Hash(uint x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
float Rand01(uint seed) { return (Hash(seed) & 0x00ffffffu) / 16777215.0f; }

// Blackbody-ish ramp: white hot at the base, then yellow, orange, deep red.
// Values go above 1 on purpose - the lighting pass treats these particles as
// emissive and tone maps them, so the core reads as genuinely glowing.
float3 FireRamp(float t)
{
    float3 white  = float3(1.60f, 1.45f, 1.10f);
    float3 yellow = float3(1.50f, 1.00f, 0.28f);
    float3 orange = float3(1.20f, 0.42f, 0.08f);
    float3 red    = float3(0.55f, 0.10f, 0.02f);
    if (t < 0.20f) return lerp(white,  yellow, t / 0.20f);
    if (t < 0.55f) return lerp(yellow, orange, (t - 0.20f) / 0.35f);
    return lerp(orange, red, saturate((t - 0.55f) / 0.45f));
}

// Cheap swirling field. Not real curl noise, but it is stable, costs a few
// sines and reads as convective motion once particles rise.
float3 Turbulate(float3 pos, float seed)
{
    float t = TotalTime * 1.6f + seed * 6.283f;
    return float3(
        sin(pos.y * 2.3f + t) + 0.5f * sin(pos.z * 3.7f - t * 1.3f),
        0.35f * sin(pos.x * 2.9f + t * 0.7f),
        cos(pos.x * 2.1f - t) + 0.5f * cos(pos.y * 3.1f + t * 1.1f));
}

[numthreads(1,1,1)]
void CSPrepare(uint3 id : SV_DispatchThreadID)
{
    uint alive = Counters[SrcCounterIndex];
    Args[0] = (alive + 63u) / 64u;
    Args[1] = 1u;
    Args[2] = 1u;
    Args[3] = 0u;
    Args[5] = 1u;   // InstanceCount
    Args[6] = 0u;   // StartVertexLocation
    Args[7] = 0u;   // StartInstanceLocation
    Counters[SAVED_COUNT_INDEX] = alive;
}

[numthreads(64,1,1)]
void CSUpdate(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Counters[SAVED_COUNT_INDEX]) return;

    Particle p = SrcParticles.Consume();
    p.Life -= DeltaTime;

    float age = saturate(1.0f - p.Life / max(p.MaxLife, 0.0001f));

    if (p.Life <= 0.0f)
    {
        // A dying flame turns into smoke instead of just vanishing. Appending
        // a different particle from the update pass costs nothing extra and
        // gives the plume its shape.
        if (p.Kind == KIND_FIRE && Rand01(asuint(p.Seed) + 7919u) < SmokeParams.z)
        {
            Particle s;
            s.Position = p.Position;
            s.Velocity = p.Velocity * 0.35f + float3(0.0f, 0.35f, 0.0f);
            s.MaxLife  = SmokeLife * (0.7f + 0.6f * Rand01(asuint(p.Seed) + 13u));
            s.Life     = s.MaxLife;
            s.Size     = p.Size * 2.0f;
            s.Kind     = KIND_SMOKE;
            s.Rotation = Rand01(asuint(p.Seed) + 29u) * 6.283f;
            s.Seed     = p.Seed;
            s.Color    = float4(0.16f, 0.15f, 0.15f, 1.0f);
            DstParticles.Append(s);
        }
        return;
    }

    float3 turb = Turbulate(p.Position, p.Seed) * Turbulence;

    if (p.Kind == KIND_FIRE)
    {
        // Hot gas accelerates upwards; buoyancy beats gravity near the base
        p.Velocity += (Gravity.xyz + float3(0.0f, Buoyancy, 0.0f)) * DeltaTime;
        p.Velocity += turb * DeltaTime;
        p.Velocity *= (1.0f - 0.6f * DeltaTime);      // drag
        p.Color.rgb = FireRamp(age);
        p.Size = lerp(SizeMin + SizeSpan, SizeMin * 0.35f, age * age);
    }
    else
    {
        // Smoke keeps rising but slows down and spreads out
        p.Velocity += (float3(0.0f, Buoyancy * 0.25f, 0.0f) + turb * 0.6f) * DeltaTime;
        p.Velocity *= (1.0f - 1.1f * DeltaTime);
        p.Rotation += (0.35f + p.Seed) * DeltaTime;
        float grey = lerp(0.20f, 0.05f, age);          // cools and darkens
        p.Color.rgb = float3(grey, grey, grey * 1.05f);
        // Growth is exponential, so it needs a ceiling. The previous form
        // compounded at about e^(1.26*t) with no cap, which reached 80x over
        // the smoke lifetime and swallowed the screen.
        p.Size = min(p.Size * (1.0f + SmokeParams.x * DeltaTime), SmokeParams.y);
    }

    p.Position += p.Velocity * DeltaTime;
    p.Color.a = age;                     // the render pass reads age from here

    DstParticles.Append(p);
}

[numthreads(64,1,1)]
void CSEmit(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= EmitCount) return;

    uint seed = id.x * 747796405u + asuint(TotalTime) * 2891336453u;

    // Spawn on a disc so the base of the fire has width
    float ang = Rand01(seed) * 6.2831853f;
    float rad = sqrt(Rand01(seed + 1u)) * 0.45f;

    Particle p;
    p.Position = EmitOrigin.xyz + float3(cos(ang) * rad,
                                         Rand01(seed + 2u) * 0.15f,
                                         sin(ang) * rad);
    // Narrower cone near the centre, wider at the rim
    float3 dir = normalize(float3(cos(ang) * rad * 0.5f, 1.0f, sin(ang) * rad * 0.5f));
    p.Velocity = dir * (EmitSpeed * (0.55f + 0.9f * Rand01(seed + 3u)));
    p.MaxLife  = LifeMin + LifeSpan * Rand01(seed + 4u);
    p.Life     = p.MaxLife;
    p.Size     = SizeMin + SizeSpan * Rand01(seed + 5u);
    p.Kind     = KIND_FIRE;
    p.Rotation = Rand01(seed + 6u) * 6.283f;
    p.Seed     = Rand01(seed + 8u);
    p.Color    = float4(FireRamp(0.0f), 0.0f);

    DstParticles.Append(p);
}
)HLSL";

static const char* kParticleRenderSrc = R"HLSL(
struct Particle
{
    float3 Position; float Life;
    float3 Velocity; float Size;
    float4 Color;
    float MaxLife; float Kind; float Rotation; float Seed;
};

cbuffer ParticleCB : register(b0)
{
    matrix View; matrix Proj;
    float4 CameraRight; float4 CameraUp; float4 CameraForward;
    float4 EmitOrigin;  float4 Gravity;
    float DeltaTime; float TotalTime; uint EmitCount; float EmitSpeed;
    float LifeMin;   float LifeSpan;  float SizeMin;  float SizeSpan;
    uint  SrcCounterIndex; float Turbulence; float Buoyancy; float SmokeLife;
    float4 SmokeParams;
};

StructuredBuffer<Particle> Particles : register(t0);

struct VSOut { float3 PosWS:POSITION; float Size:TEXCOORD0; float4 Color:COLOR;
               float Kind:TEXCOORD1; float Rot:TEXCOORD2; };
struct GSOut { float4 Pos:SV_POSITION; float2 UV:TEXCOORD0; float3 PosWS:TEXCOORD1;
               float4 Color:COLOR; float Kind:TEXCOORD2; };

VSOut VSMain(uint vid : SV_VertexID)
{
    Particle p = Particles[vid];
    VSOut o;
    o.PosWS = p.Position;
    o.Size  = p.Size;
    o.Color = p.Color;
    o.Kind  = p.Kind;
    o.Rot   = p.Rotation;
    return o;
}

[maxvertexcount(4)]
void GSMain(point VSOut input[1], inout TriangleStream<GSOut> stream)
{
    float3 c = input[0].PosWS;
    float  h = input[0].Size * 0.5f;

    // Rotate the billboard basis around the view axis. Without this every
    // smoke puff is the same square and the plume looks like a grid.
    float sn, cs;
    sincos(input[0].Rot, sn, cs);
    float3 r = (CameraRight.xyz * cs + CameraUp.xyz * sn) * h;
    float3 u = (CameraUp.xyz * cs - CameraRight.xyz * sn) * h;

    float3 corner[4] = { c - r - u, c - r + u, c + r - u, c + r + u };
    float2 uvs[4] = { float2(0,1), float2(0,0), float2(1,1), float2(1,0) };

    GSOut o;
    o.Color = input[0].Color;
    o.Kind  = input[0].Kind;
    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        o.PosWS = corner[i];
        o.Pos   = mul(mul(float4(corner[i], 1.0f), View), Proj);
        o.UV    = uvs[i];
        stream.Append(o);
    }
}

struct PSOut { float4 Albedo:SV_Target0; float4 Normal:SV_Target1;
               float4 PBR:SV_Target2;    float4 WorldPos:SV_Target3; };

PSOut PSMain(GSOut i)
{
    float2 d  = i.UV * 2.0f - 1.0f;
    float  r2 = dot(d, d);
    float  age = i.Color.a;

    // Particles are opaque, so there is no alpha to fade with. The silhouette
    // shrinks with age instead: the cutout radius closes as the particle dies,
    // which reads as thinning out without any blending.
    float radius = (i.Kind < 0.5f) ? lerp(0.98f, 0.30f, age * age)
                                   : lerp(0.55f, 0.98f, age);
    clip(radius - r2);

    PSOut o;

    if (i.Kind < 0.5f)
    {
        // Fire: emissive. Metallic channel is unused by the lighting maths,
        // so it carries the flag; the lighting pass outputs albedo directly
        // instead of shading it.
        o.Albedo = float4(i.Color.rgb, 1.0f);
        o.PBR    = float4(0.9f, 1.0f, 1.0f, 1.0f);
        o.Normal = float4(-CameraForward.xyz, 0.0f);
    }
    else
    {
        // Smoke is lit normally, so it picks up the sun and the scene lights
        float z = sqrt(saturate(1.0f - r2));
        float3 n = normalize(CameraRight.xyz * d.x + CameraUp.xyz * (-d.y)
                           - CameraForward.xyz * z);
        o.Albedo = float4(i.Color.rgb, 1.0f);
        o.PBR    = float4(0.95f, 0.0f, 1.0f, 1.0f);
        o.Normal = float4(n, 0.0f);
    }

    o.WorldPos = float4(i.PosWS, 1.0f);
    return o;
}
)HLSL";

// ============================================================
D3D12_GPU_DESCRIPTOR_HANDLE ParticleSystem::GpuAt(int localSlot) const
{
    D3D12_GPU_DESCRIPTOR_HANDLE h = gpuBase_;
    h.ptr += static_cast<UINT64>(localSlot) * srvDescSize_;
    return h;
}

void ParticleSystem::Create(ID3D12Device* device, ID3D12DescriptorHeap* srvHeap,
    UINT srvBaseSlot, UINT srvDescSize)
{
    srvDescSize_ = srvDescSize;
    gpuBase_ = srvHeap->GetGPUDescriptorHandleForHeapStart();
    gpuBase_.ptr += static_cast<UINT64>(srvBaseSlot) * srvDescSize;

    CreateBuffers(device);
    CreateDescriptors(device, srvHeap, srvBaseSlot, srvDescSize);
    CreateComputePipeline(device);
    CreateRenderPipeline(device);
}

void ParticleSystem::CreateBuffers(ID3D12Device* device)
{
    D3D12_HEAP_PROPERTIES def = {}; def.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_HEAP_PROPERTIES upl = {}; upl.Type = D3D12_HEAP_TYPE_UPLOAD;

    auto MakeBuffer = [&](UINT64 bytes, D3D12_RESOURCE_FLAGS flags,
        D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource>& out)
        {
            D3D12_RESOURCE_DESC rd = {};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width = bytes; rd.Height = 1; rd.DepthOrArraySize = 1;
            rd.MipLevels = 1; rd.Format = DXGI_FORMAT_UNKNOWN;
            rd.SampleDesc = { 1,0 };
            rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            rd.Flags = flags;
            ThrowIfFailed(device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE,
                &rd, state, nullptr, IID_PPV_ARGS(&out)), "particle buffer");
        };

    const UINT64 particleBytes = sizeof(ParticleGPU) * MAX_PARTICLES;
    for (int i = 0; i < 2; ++i)
        MakeBuffer(particleBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, buffer_[i]);

    // Два счётчика, разнесённые на 4096 байт из-за требования выравнивания
    // Три слота: два счётчика пинг-понга + сохранённое число живых
    MakeBuffer(COUNTER_STRIDE * 3, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, counters_);

    // Создаём сразу в INDIRECT_ARGUMENT: Simulate начинает и заканчивает
    // кадр этим состоянием, иначе первый переход был бы из неверного.
    MakeBuffer(ARGS_SIZE, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, indirectArgs_);

    // Маленький upload-буфер с нулём: им обнуляем счётчик приёмника
    {
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = 4; rd.Height = 1; rd.DepthOrArraySize = 1;
        rd.MipLevels = 1; rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc = { 1,0 }; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ThrowIfFailed(device->CreateCommittedResource(&upl, D3D12_HEAP_FLAG_NONE,
            &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&counterReset_)), "counter reset");
        UINT zero = 0; void* p = nullptr;
        D3D12_RANGE none = { 0,0 };
        ThrowIfFailed(counterReset_->Map(0, &none, &p));
        memcpy(p, &zero, sizeof(zero));
        counterReset_->Unmap(0, nullptr);
    }

    // Константный буфер: кольцо на несколько кадров, CPU опережает GPU
    {
        cbStride_ = (sizeof(ParticleCB) + 255) & ~255ull;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = cbStride_ * 3; rd.Height = 1; rd.DepthOrArraySize = 1;
        rd.MipLevels = 1; rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc = { 1,0 }; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ThrowIfFailed(device->CreateCommittedResource(&upl, D3D12_HEAP_FLAG_NONE,
            &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&cbRes_)), "particle cb");
        ThrowIfFailed(cbRes_->Map(0, nullptr, reinterpret_cast<void**>(&cbMapped_)));
    }
}

void ParticleSystem::CreateDescriptors(ID3D12Device* device,
    ID3D12DescriptorHeap* srvHeap, UINT srvBaseSlot, UINT srvDescSize)
{
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = srvHeap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(srvBaseSlot) * srvDescSize;
    auto At = [&](int local) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = cpu;
        h.ptr += static_cast<SIZE_T>(local) * srvDescSize;
        return h;
        };

    // UAV буфера частиц со счётчиком
    auto MakeParticleUAV = [&](int bufIndex, UINT64 counterOffset, int local)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
            uav.Format = DXGI_FORMAT_UNKNOWN;
            uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            uav.Buffer.NumElements = MAX_PARTICLES;
            uav.Buffer.StructureByteStride = sizeof(ParticleGPU);
            uav.Buffer.CounterOffsetInBytes = counterOffset;
            device->CreateUnorderedAccessView(buffer_[bufIndex].Get(),
                counters_.Get(), &uav, At(local));
        };
    MakeParticleUAV(0, 0, D_UAV_A0);
    MakeParticleUAV(1, COUNTER_STRIDE, D_UAV_B);
    MakeParticleUAV(0, 0, D_UAV_A1);

    // UAV аргументов и счётчиков как обычных uint-буферов
    auto MakeRawUAV = [&](ID3D12Resource* res, UINT numElements, int local)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
            uav.Format = DXGI_FORMAT_R32_UINT;
            uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            uav.Buffer.NumElements = numElements;
            device->CreateUnorderedAccessView(res, nullptr, &uav, At(local));
        };
    MakeRawUAV(indirectArgs_.Get(), static_cast<UINT>(ARGS_SIZE / 4), D_UAV_ARGS);
    MakeRawUAV(counters_.Get(), static_cast<UINT>(COUNTER_STRIDE * 3 / 4), D_UAV_COUNTERS);

    // SRV для чтения при отрисовке
    for (int i = 0; i < 2; ++i)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Format = DXGI_FORMAT_UNKNOWN;
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.NumElements = MAX_PARTICLES;
        srv.Buffer.StructureByteStride = sizeof(ParticleGPU);
        device->CreateShaderResourceView(buffer_[i].Get(), &srv,
            At(i == 0 ? D_SRV_A : D_SRV_B));
    }
}

void ParticleSystem::CreateComputePipeline(ID3D12Device* device)
{
    // param0: CBV b0
    // param1: таблица u0..u1 - источник (Consume) и приёмник (Append)
    // param2: таблица u2..u3 - аргументы и счётчики
    D3D12_DESCRIPTOR_RANGE rPingPong = {};
    rPingPong.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    rPingPong.NumDescriptors = 2; rPingPong.BaseShaderRegister = 0;

    D3D12_DESCRIPTOR_RANGE rAux = {};
    rAux.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    rAux.NumDescriptors = 2; rAux.BaseShaderRegister = 2;

    D3D12_ROOT_PARAMETER params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &rPingPong;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &rAux;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 3; desc.pParameters = params;
    ComPtr<ID3DBlob> sig, err;
    ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
        &sig, &err), "particle compute RS");
    ThrowIfFailed(device->CreateRootSignature(0, sig->GetBufferPointer(),
        sig->GetBufferSize(), IID_PPV_ARGS(&computeRS_)));

    UINT flags = 0;
#ifdef _DEBUG
    flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    auto Build = [&](const char* entry, ComPtr<ID3D12PipelineState>& out)
        {
            ComPtr<ID3DBlob> cs, e;
            HRESULT hr = D3DCompile(kParticleComputeSrc, strlen(kParticleComputeSrc),
                nullptr, nullptr, nullptr, entry, "cs_5_0", flags, 0, &cs, &e);
            if (FAILED(hr))
            {
                if (e) OutputDebugStringA((char*)e->GetBufferPointer());
                ThrowIfFailed(hr, entry);
            }
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
            pd.pRootSignature = computeRS_.Get();
            pd.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
            ThrowIfFailed(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&out)));
        };
    Build("CSPrepare", psoPrepare_);
    Build("CSUpdate", psoUpdate_);
    Build("CSEmit", psoEmit_);

    // Сигнатуры косвенных команд
    D3D12_INDIRECT_ARGUMENT_DESC dispatchArg = {};
    dispatchArg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC csd = {};
    csd.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
    csd.NumArgumentDescs = 1; csd.pArgumentDescs = &dispatchArg;
    ThrowIfFailed(device->CreateCommandSignature(&csd, nullptr,
        IID_PPV_ARGS(&dispatchSignature_)), "dispatch signature");

    D3D12_INDIRECT_ARGUMENT_DESC drawArg = {};
    drawArg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    csd.ByteStride = sizeof(D3D12_DRAW_ARGUMENTS);
    csd.pArgumentDescs = &drawArg;
    ThrowIfFailed(device->CreateCommandSignature(&csd, nullptr,
        IID_PPV_ARGS(&drawSignature_)), "draw signature");
}

void ParticleSystem::CreateRenderPipeline(ID3D12Device* device)
{
    D3D12_DESCRIPTOR_RANGE rSrv = {};
    rSrv.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    rSrv.NumDescriptors = 1; rSrv.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &rSrv;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 2; desc.pParameters = params;
    ComPtr<ID3DBlob> sig, err;
    ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
        &sig, &err), "particle render RS");
    ThrowIfFailed(device->CreateRootSignature(0, sig->GetBufferPointer(),
        sig->GetBufferSize(), IID_PPV_ARGS(&renderRS_)));

    UINT flags = 0;
#ifdef _DEBUG
    flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    ComPtr<ID3DBlob> vs, gs, ps, e;
    auto Compile = [&](const char* entry, const char* target, ComPtr<ID3DBlob>& out)
        {
            HRESULT hr = D3DCompile(kParticleRenderSrc, strlen(kParticleRenderSrc),
                nullptr, nullptr, nullptr, entry, target, flags, 0, &out, &e);
            if (FAILED(hr))
            {
                if (e) OutputDebugStringA((char*)e->GetBufferPointer());
                ThrowIfFailed(hr, entry);
            }
        };
    Compile("VSMain", "vs_5_0", vs);
    Compile("GSMain", "gs_5_0", gs);
    Compile("PSMain", "ps_5_0", ps);

    D3D12_RASTERIZER_DESC raster = {};
    raster.FillMode = D3D12_FILL_MODE_SOLID;
    raster.CullMode = D3D12_CULL_MODE_NONE;   // билборд всегда лицом к камере
    raster.DepthClipEnable = TRUE;

    D3D12_DEPTH_STENCIL_DESC dss = {};
    dss.DepthEnable = TRUE;                   // частицы непрозрачные
    dss.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    dss.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

    D3D12_BLEND_DESC blend = {};
    for (UINT i = 0; i < GBUFFER_RT_COUNT; ++i)
        blend.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = renderRS_.Get();
    pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pso.GS = { gs->GetBufferPointer(), gs->GetBufferSize() };
    pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pso.InputLayout = { nullptr, 0 };         // вершинного буфера нет
    pso.RasterizerState = raster;
    pso.DepthStencilState = dss;
    pso.BlendState = blend;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    pso.NumRenderTargets = GBUFFER_RT_COUNT;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pso.RTVFormats[2] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.RTVFormats[3] = DXGI_FORMAT_R32G32B32A32_FLOAT;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc = { 1,0 };
    ThrowIfFailed(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&renderPSO_)));
}

// ============================================================
//  Кадр симуляции. Всё считается на GPU.
// ============================================================
void ParticleSystem::Simulate(ID3D12GraphicsCommandList* cl,
    ID3D12DescriptorHeap* srvHeap, const ParticleCB& cbIn)
{
    if (!Ready()) return;
    const int dst = 1 - src_;

    ParticleCB cb = cbIn;
    cb.SrcCounterIndex = static_cast<unsigned>(src_) *
        static_cast<unsigned>(COUNTER_STRIDE / 4);

    cbSlot_ = (cbSlot_ + 1) % 3;
    memcpy(cbMapped_ + cbSlot_ * cbStride_, &cb, sizeof(cb));

    auto UavBarrier = [&](ID3D12Resource* r)
        {
            D3D12_RESOURCE_BARRIER b = {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            b.UAV.pResource = r;
            cl->ResourceBarrier(1, &b);
        };
    auto Transition = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES a,
        D3D12_RESOURCE_STATES b)
        {
            D3D12_RESOURCE_BARRIER t = {};
            t.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            t.Transition.pResource = r;
            t.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            t.Transition.StateBefore = a; t.Transition.StateAfter = b;
            cl->ResourceBarrier(1, &t);
        };

    // ---- обнулить счётчик приёмника (и оба на первом кадре) ----
    Transition(counters_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_COPY_DEST);
    cl->CopyBufferRegion(counters_.Get(), COUNTER_STRIDE * dst,
        counterReset_.Get(), 0, 4);
    if (needsClear_)
        cl->CopyBufferRegion(counters_.Get(), COUNTER_STRIDE * src_,
            counterReset_.Get(), 0, 4);
    Transition(counters_.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    needsClear_ = false;

    // Таблицы дескрипторов требуют привязанного хипа
    ID3D12DescriptorHeap* heaps[] = { srvHeap };
    cl->SetDescriptorHeaps(1, heaps);

    // Буфер аргументов приходит из прошлого кадра в INDIRECT_ARGUMENT
    Transition(indirectArgs_.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    cl->SetComputeRootSignature(computeRS_.Get());
    cl->SetComputeRootConstantBufferView(0,
        cbRes_->GetGPUVirtualAddress() + cbSlot_ * cbStride_);
    // Смещение на src_ и даёт пинг-понг: (A,B) при src_=0, (B,A) при src_=1
    cl->SetComputeRootDescriptorTable(1, GpuAt(D_UAV_A0 + src_));
    cl->SetComputeRootDescriptorTable(2, GpuAt(D_UAV_ARGS));

    // ---- 1. счётчик источника -> аргументы Dispatch ----
    cl->SetPipelineState(psoPrepare_.Get());
    cl->Dispatch(1, 1, 1);
    UavBarrier(indirectArgs_.Get());

    // ---- 2. обновление позиций, число групп берётся с GPU ----
    Transition(indirectArgs_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    cl->SetPipelineState(psoUpdate_.Get());
    cl->ExecuteIndirect(dispatchSignature_.Get(), 1, indirectArgs_.Get(),
        ARGS_DISPATCH_OFFSET, nullptr, 0);
    UavBarrier(buffer_[dst].Get());
    UavBarrier(counters_.Get());

    // ---- 3. рождение новых ----
    cl->SetPipelineState(psoEmit_.Get());
    cl->Dispatch((MAX_EMIT_PER_FRAME + THREADS_PER_GROUP - 1) / THREADS_PER_GROUP, 1, 1);
    UavBarrier(buffer_[dst].Get());
    UavBarrier(counters_.Get());

    // ---- 4. счётчик приёмника -> число вершин для Draw ----
    // Простым копированием: тут не нужно ничего вычислять.
    Transition(counters_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(indirectArgs_.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
        D3D12_RESOURCE_STATE_COPY_DEST);
    cl->CopyBufferRegion(indirectArgs_.Get(), ARGS_DRAW_OFFSET,
        counters_.Get(), COUNTER_STRIDE * dst, 4);
    Transition(indirectArgs_.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    Transition(counters_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    src_ = dst;
}

void ParticleSystem::Render(ID3D12GraphicsCommandList* cl, ID3D12DescriptorHeap* srvHeap)
{
    if (!Ready()) return;

    // Буфер, в который только что писал Append, теперь читается вершинным шейдером
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = buffer_[src_].Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    cl->ResourceBarrier(1, &b);

    ID3D12DescriptorHeap* heaps[] = { srvHeap };
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetGraphicsRootSignature(renderRS_.Get());
    cl->SetGraphicsRootConstantBufferView(0,
        cbRes_->GetGPUVirtualAddress() + cbSlot_ * cbStride_);
    cl->SetGraphicsRootDescriptorTable(1,
        GpuAt(src_ == 0 ? D_SRV_A : D_SRV_B));

    cl->SetPipelineState(renderPSO_.Get());
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    cl->IASetVertexBuffers(0, 0, nullptr);
    cl->IASetIndexBuffer(nullptr);

    // Число частиц знает только GPU, поэтому отрисовка косвенная
    cl->ExecuteIndirect(drawSignature_.Get(), 1, indirectArgs_.Get(),
        ARGS_DRAW_OFFSET, nullptr, 0);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    cl->ResourceBarrier(1, &b);
}
