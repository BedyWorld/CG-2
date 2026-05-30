#include "GBuffer.h"
#include "Utils.h"

void GBuffer::Create(ID3D12Device* device,
    int width, int height,
    UINT rtvDescSize,
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHeapStart,
    ID3D12DescriptorHeap* srvHeap,
    UINT srvBaseSlot,
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle)
{
    width_ = width;
    height_ = height;
    dsvHandle_ = dsvHandle;

    // Освобождаем старые ресурсы перед пересозданием
    for (auto& rt : rt_) rt.Reset();

    CreateTextures(device);
    CreateRTVs(device, rtvDescSize, rtvHeapStart);
    CreateSRVs(device, srvHeap, srvBaseSlot);
}

void GBuffer::CreateTextures(ID3D12Device* device)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    for (UINT i = 0; i < GBUFFER_RT_COUNT; ++i)
    {
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = static_cast<UINT>(width_);
        desc.Height = static_cast<UINT>(height_);
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = kFormats[i];
        desc.SampleDesc = { 1, 0 };
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE clear = {};
        clear.Format = kFormats[i];
        for (int c = 0; c < 4; ++c)
            clear.Color[c] = kClearColors[i][c];

        ThrowIfFailed(device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE,
            &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            &clear, IID_PPV_ARGS(&rt_[i])));

        // Debug names
        const wchar_t* names[] = { L"GBuffer_Albedo", L"GBuffer_Normal", L"GBuffer_PBR", L"GBuffer_WorldPos" };
        rt_[i]->SetName(names[i]);
    }
}

void GBuffer::CreateRTVs(ID3D12Device* device, UINT rtvDescSize,
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHeapStart)
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = rtvHeapStart;

    for (UINT i = 0; i < GBUFFER_RT_COUNT; ++i)
    {
        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
        rtvDesc.Format = kFormats[i];
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

        device->CreateRenderTargetView(rt_[i].Get(), &rtvDesc, handle);
        rtvHandles_[i] = handle;
        handle.ptr += rtvDescSize;
    }
}

void GBuffer::CreateSRVs(ID3D12Device* device,
    ID3D12DescriptorHeap* srvHeap, UINT srvBaseSlot)
{
    UINT srvDescSize = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle =
        srvHeap->GetCPUDescriptorHandleForHeapStart();
    cpuHandle.ptr += static_cast<SIZE_T>(srvBaseSlot) * srvDescSize;

    srvGpuStart_ = srvHeap->GetGPUDescriptorHandleForHeapStart();
    srvGpuStart_.ptr += static_cast<UINT64>(srvBaseSlot) * srvDescSize;

    for (UINT i = 0; i < GBUFFER_RT_COUNT; ++i)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = kFormats[i];
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;

        device->CreateShaderResourceView(rt_[i].Get(), &srvDesc, cpuHandle);
        cpuHandle.ptr += srvDescSize;
    }
}

void GBuffer::TransitionTo(ID3D12GraphicsCommandList* cmd,
    D3D12_RESOURCE_STATES state)
{
    D3D12_RESOURCE_BARRIER barriers[GBUFFER_RT_COUNT] = {};
    for (UINT i = 0; i < GBUFFER_RT_COUNT; ++i)
    {
        barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[i].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barriers[i].Transition.pResource = rt_[i].Get();
        barriers[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[i].Transition.StateBefore = (state == D3D12_RESOURCE_STATE_RENDER_TARGET)
            ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
            : D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[i].Transition.StateAfter = state;
    }
    cmd->ResourceBarrier(GBUFFER_RT_COUNT, barriers);
}

void GBuffer::Clear(ID3D12GraphicsCommandList* cmd)
{
    for (UINT i = 0; i < GBUFFER_RT_COUNT; ++i)
        cmd->ClearRenderTargetView(rtvHandles_[i], kClearColors[i], 0, nullptr);
}