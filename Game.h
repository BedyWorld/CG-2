#pragma once
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <vector>
#include <string>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxguid.lib")

#include "Types.h"
#include "TextureLoader.h"
#include "ObjLoader.h"

using Microsoft::WRL::ComPtr;

static const UINT FRAME_COUNT = 2;

class Game
{
public:
    Game(HWND hwnd, int width, int height);
    ~Game();

    bool Initialize();
    void Update(float deltaTime);
    void Render();
    void Resize(int width, int height);

private:
    // ---- Init helpers ----
    void CreateDevice();
    void CreateCommandQueue();
    void CreateSwapChain();
    void CreateDescriptorHeaps();
    void CreateRenderTargetViews();
    void CreateDepthStencilBuffer();
    void CreateCommandObjects();
    void CreateFence();
    void CreateRootSignature();
    void CreatePSO();
    void CreateGeometry();
    void LoadModel(const std::wstring& objPath);
    void UploadTexture(const TextureData& td, int slot = 0);
    void CreateConstantBuffer();

    // ---- Sync ----
    void WaitForGPU();
    void MoveToNextFrame();

    // ---- Render ----
    void PopulateCommandList();

    // ---- Geometry helpers ----
    std::vector<Vertex> GenerateCube();
    std::vector<UINT>   GenerateCubeIndices();
    void BuildBuffers(const std::vector<Vertex>& verts,
                      const std::vector<UINT>&   idxs);
    void MakeUploadBuffer(const void* data, UINT64 byteSize,
                          ComPtr<ID3D12Resource>& buf);

    ComPtr<ID3DBlob> CompileShader(const std::wstring& filename,
                                   const std::string& entry,
                                   const std::string& target);

    // Window
    HWND hwnd_;
    int  width_;
    int  height_;

    // Animation
    float rotationAngle_ = 0.0f;
    float uvOffsetX_     = 0.0f;
    float uvOffsetY_     = 0.0f;

    // D3D12 core
    ComPtr<ID3D12Device>              device_;
    ComPtr<ID3D12CommandQueue>        commandQueue_;
    ComPtr<IDXGISwapChain3>           swapChain_;

    // Descriptor heaps
    ComPtr<ID3D12DescriptorHeap>      rtvHeap_;
    ComPtr<ID3D12DescriptorHeap>      dsvHeap_;
    ComPtr<ID3D12DescriptorHeap>      srvHeap_;   // shader-visible, for texture
    UINT                              rtvDescSize_ = 0;

    // Render targets + depth
    ComPtr<ID3D12Resource>            renderTargets_[FRAME_COUNT];
    ComPtr<ID3D12Resource>            depthStencil_;

    // Command objects
    ComPtr<ID3D12CommandAllocator>    commandAllocators_[FRAME_COUNT];
    ComPtr<ID3D12GraphicsCommandList> commandList_;

    // Pipeline
    ComPtr<ID3D12RootSignature>       rootSignature_;
    ComPtr<ID3D12PipelineState>       pso_;

    // Geometry
    ComPtr<ID3D12Resource>            vertexBuffer_;
    ComPtr<ID3D12Resource>            indexBuffer_;
    D3D12_VERTEX_BUFFER_VIEW          vbView_ = {};
    D3D12_INDEX_BUFFER_VIEW           ibView_ = {};
    UINT                              indexCount_ = 0;

    // Textures  (slot 0 = A, slot 1 = B)
    ComPtr<ID3D12Resource>            texture_;
    ComPtr<ID3D12Resource>            textureUpload_;
    ComPtr<ID3D12Resource>            texture2_;
    ComPtr<ID3D12Resource>            textureUpload2_;

    // Constant buffer (persistently mapped)
    ComPtr<ID3D12Resource>            constantBuffer_;
    ConstantBufferData*               cbMapped_ = nullptr;

    // Fence
    ComPtr<ID3D12Fence>               fence_;
    UINT64                            fenceValues_[FRAME_COUNT] = {};
    HANDLE                            fenceEvent_ = nullptr;
    UINT                              frameIndex_ = 0;
};
