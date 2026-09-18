#pragma once
#include <d3d12.h>
#include <DirectXMath.h>
#include <wrl/client.h>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

// ============================================================
//  Система частиц на GPU (ДЗ №6)
//
//  Позиции обновляются ТОЛЬКО в compute шейдере. CPU не знает и не
//  может знать, сколько частиц живо: счётчик живёт на GPU, отрисовка
//  и диспетчеризация идут через ExecuteIndirect.
//
//  Два буфера в пинг-понге. За кадр:
//    1. обнулить счётчик приёмника
//    2. Prepare CS: прочитать счётчик источника -> аргументы Dispatch
//    3. DispatchIndirect Update CS: Consume из источника, Append в
//       приёмник, если частица ещё жива
//    4. Dispatch Emit CS: Append новых частиц в приёмник
//    5. скопировать счётчик приёмника в аргументы Draw
//    6. поменять буферы местами
//
//  Мёртвая частица просто не попадает в Append — отдельного списка
//  свободных слотов не нужно, счётчик приёмника и есть число живых.
// ============================================================

struct ParticleGPU
{
    XMFLOAT3 Position; float Life;
    XMFLOAT3 Velocity; float Size;
    XMFLOAT4 Color;
    float MaxLife;   // чтобы считать возраст 0..1 и вести по нему цвет и размер
    float Kind;      // 0 = пламя, 1 = дым
    float Rotation;  // поворот билборда, чтобы клубы не были одинаковыми
    float Seed;      // индивидуальная фаза турбулентности
};
static_assert(sizeof(ParticleGPU) == 64, "должно совпадать со структурой в HLSL");

// Константы для compute и отрисовки. Порядок обязан совпадать с cbuffer в HLSL.
struct ParticleCB
{
    XMFLOAT4X4 View;
    XMFLOAT4X4 Proj;
    XMFLOAT4   CameraRight;    // базис билборда, считается на CPU:
    XMFLOAT4   CameraUp;       // разбирать транспонированную матрицу в GS
    XMFLOAT4   CameraForward;  // легко ошибиться
    XMFLOAT4   EmitOrigin;
    XMFLOAT4   Gravity;
    float DeltaTime; float TotalTime; unsigned EmitCount; float EmitSpeed;
    float LifeMin;   float LifeSpan;  float SizeMin;      float SizeSpan;
    unsigned SrcCounterIndex; float Turbulence; float Buoyancy; float SmokeLife;
    // x — скорость роста в 1/с, y — ЖЁСТКИЙ потолок размера,
    // z — вероятность рождения дыма из умирающего пламени
    XMFLOAT4 SmokeParams;
};

// Те же проверки для констант частиц: раскладка дублируется в двух
// шейдерах сразу, и рассинхрон здесь особенно легко не заметить.
static_assert(offsetof(ParticleCB, CameraRight) == 128, "ParticleCB: CameraRight");
static_assert(offsetof(ParticleCB, EmitOrigin) == 176, "ParticleCB: EmitOrigin");
static_assert(offsetof(ParticleCB, SmokeParams) == 256, "ParticleCB: SmokeParams");
static_assert(sizeof(ParticleCB) == 272, "изменился размер ParticleCB");

class ParticleSystem
{
public:
    static const unsigned MAX_PARTICLES = 65536;
    static const unsigned THREADS_PER_GROUP = 64;
    static const unsigned MAX_EMIT_PER_FRAME = 512;
    static const int      DESCRIPTOR_COUNT = 7;   // сколько слотов нужно в SRV-хипе

    void Create(ID3D12Device* device, ID3D12DescriptorHeap* srvHeap,
        UINT srvBaseSlot, UINT srvDescSize);

    // Шаг симуляции: целиком на GPU
    void Simulate(ID3D12GraphicsCommandList* cl,
        ID3D12DescriptorHeap* srvHeap, const ParticleCB& cb);
    // Отрисовка в G-buffer: точки разворачиваются в билборды геометрическим шейдером
    void Render(ID3D12GraphicsCommandList* cl, ID3D12DescriptorHeap* srvHeap);

    bool Ready() const { return buffer_[0] != nullptr; }
    void Reset() { needsClear_ = true; }

private:
    void CreateBuffers(ID3D12Device* device);
    void CreateDescriptors(ID3D12Device* device, ID3D12DescriptorHeap* srvHeap,
        UINT srvBaseSlot, UINT srvDescSize);
    void CreateComputePipeline(ID3D12Device* device);
    void CreateRenderPipeline(ID3D12Device* device);

    D3D12_GPU_DESCRIPTOR_HANDLE GpuAt(int localSlot) const;

    ComPtr<ID3D12Resource> buffer_[2];      // частицы, пинг-понг
    ComPtr<ID3D12Resource> counters_;       // два счётчика UAV
    ComPtr<ID3D12Resource> counterReset_;   // upload-буфер с нулём
    ComPtr<ID3D12Resource> indirectArgs_;   // Dispatch + Draw аргументы

    ComPtr<ID3D12RootSignature> computeRS_;
    ComPtr<ID3D12PipelineState> psoPrepare_;
    ComPtr<ID3D12PipelineState> psoUpdate_;
    ComPtr<ID3D12PipelineState> psoEmit_;

    ComPtr<ID3D12RootSignature> renderRS_;
    ComPtr<ID3D12PipelineState> renderPSO_;
    ComPtr<ID3D12CommandSignature> drawSignature_;
    ComPtr<ID3D12CommandSignature> dispatchSignature_;

    ComPtr<ID3D12Resource> cbRes_;
    BYTE* cbMapped_ = nullptr;
    UINT64 cbStride_ = 0;
    int    cbSlot_ = 0;

    D3D12_GPU_DESCRIPTOR_HANDLE gpuBase_ = {};
    UINT srvDescSize_ = 0;
    int  src_ = 0;             // индекс буфера-источника
    bool needsClear_ = true;
};
