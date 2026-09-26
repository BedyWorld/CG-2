#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <array>

using Microsoft::WRL::ComPtr;


static const UINT GBUFFER_RT_COUNT = 4;

class GBuffer
{
public:
    GBuffer() = default;
    ~GBuffer() = default;

    GBuffer(const GBuffer&) = delete;
    GBuffer& operator=(const GBuffer&) = delete;

    void Create(ID3D12Device* device,
        int width, int height,
        UINT rtvDescSize,
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHeapStart,  // слот 2 (после 0 и 1 swapchain)
        ID3D12DescriptorHeap* srvHeap,             // shader-visible heap
        UINT srvBaseSlot,                          // первый свободный SRV-слот (4)
        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle);    // shared depth

    void TransitionTo(ID3D12GraphicsCommandList* cmd,
        D3D12_RESOURCE_STATES state);

    // Очищает все RT-буферы.
    void Clear(ID3D12GraphicsCommandList* cmd);

    // Возвращает массив RTV CPU-хэндлов (для OMSetRenderTargets на geometry pass).
    const D3D12_CPU_DESCRIPTOR_HANDLE* GetRTVHandles() const { return rtvHandles_.data(); }
    UINT GetRTVCount() const { return GBUFFER_RT_COUNT; }

    // DSV-хэндл общего depth buffer (передаётся снаружи).
    D3D12_CPU_DESCRIPTOR_HANDLE GetDSVHandle() const { return dsvHandle_; }

    // GPU-хэндл на начало SRV-записей в shader-visible heap (для DescriptorTable).
    D3D12_GPU_DESCRIPTOR_HANDLE GetSRVGpuStart() const { return srvGpuStart_; }

    int GetWidth()  const { return width_; }
    int GetHeight() const { return height_; }

private:
    void CreateTextures(ID3D12Device* device);
    void CreateRTVs(ID3D12Device* device, UINT rtvDescSize,
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHeapStart);
    void CreateSRVs(ID3D12Device* device,
        ID3D12DescriptorHeap* srvHeap, UINT srvBaseSlot);

    int  width_ = 0;
    int  height_ = 0;

    // Render target resources
    ComPtr<ID3D12Resource> rt_[GBUFFER_RT_COUNT];   // 0=Albedo, 1=Normal, 2=PBR

    // RTV CPU-хэндлы
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, GBUFFER_RT_COUNT> rtvHandles_ = {};

    // Shared DSV (снаружи)
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle_ = {};

    // SRV GPU-хэндл (начало блока из 3 записей)
    D3D12_GPU_DESCRIPTOR_HANDLE srvGpuStart_ = {};

    // Форматы RT
    static constexpr DXGI_FORMAT kFormats[GBUFFER_RT_COUNT] = {
        DXGI_FORMAT_R8G8B8A8_UNORM,         // Albedo
        DXGI_FORMAT_R16G16B16A16_FLOAT,     // Normal
        DXGI_FORMAT_R8G8B8A8_UNORM,         // PBR (roughness/metallic/ao)
        DXGI_FORMAT_R32G32B32A32_FLOAT,     // WorldPos (xyz) + unused (w)
    };

    // Цвета очистки
    static constexpr float kClearColors[GBUFFER_RT_COUNT][4] = {
        { 0.0f, 0.0f, 0.0f, 1.0f },
        { 0.0f, 0.0f, 0.0f, 0.0f },
        { 0.5f, 0.0f, 1.0f, 0.0f },  // roughness=0.5, metallic=0, ao=1
        { 0.0f, 0.0f, 0.0f, 0.0f },  // WorldPos
    };
};