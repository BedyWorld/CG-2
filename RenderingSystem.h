#pragma once
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <vector>
#include <string>
#include <array>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxguid.lib")

#include "Types.h"
#include "GBuffer.h"
#include "TextureLoader.h"
#include "Utils.h"

using Microsoft::WRL::ComPtr;

static const UINT RS_FRAME_COUNT = 2;

// ============================================================
//  RenderingSystem — D3D12-инфраструктура с Deferred Rendering
//
//  Архитектура двух проходов:
//    1. Geometry Pass  — заполняет GBuffer (Albedo / Normal / PBR)
//    2. Lighting Pass  — full-screen quad, читает GBuffer,
//                        вычисляет освещение от 3 типов источников
//
//  Источники света (MAX_LIGHTS = 16):
//    - Directional  — бесконечно далёкий направленный свет
//    - Point        — точечный свет с затуханием по 1/d²
//    - Spot         — конический свет (inner/outer cosine angles)
//
//  SRV-хип (shader-visible, 8 слотов):
//    slot 0 — texture A (t0, geometry pass)
//    slot 1 — texture B (t1, geometry pass)
//    slot 2 — (reserved)
//    slot 3 — (reserved)
//    slot 4 — GBuffer Albedo  (t4, lighting pass)
//    slot 5 — GBuffer Normal  (t5, lighting pass)
//    slot 6 — GBuffer PBR     (t6, lighting pass)
//    slot 7 — (reserved)
//
//  RTV-хип:
//    slot 0,1 — swap chain back buffers
//    slot 2,3,4 — GBuffer RT0/RT1/RT2
// ============================================================
class RenderingSystem
{
public:
    RenderingSystem(HWND hwnd, int width, int height);
    ~RenderingSystem();

    RenderingSystem(const RenderingSystem&) = delete;
    RenderingSystem& operator=(const RenderingSystem&) = delete;

    // ---- Жизненный цикл ----
    void Initialize();
    void BeginResourceUpload();
    void EndResourceUpload();

    // ---- Текстуры ----
    void UploadTexture(const TextureData& td, int slot = 0);
    void ReleaseTextureUploadBuffers();

    // ---- Геометрия ----
    void BuildBuffers(const std::vector<Vertex>& verts,
                      const std::vector<UINT>&   idxs);

    // ---- Константные буферы ----
    void CreateConstantBuffers();
    void UpdateGeometryPassCB(const ConstantBufferData& data);
    void UpdateLightingPassCB(const LightingPassCB& data);

    // ---- Управление источниками света ----
    void ClearLights();
    void AddDirectionalLight(XMFLOAT3 direction, XMFLOAT3 color, float intensity);
    void AddPointLight(XMFLOAT3 position, XMFLOAT3 color, float intensity, float range);
    void AddSpotLight(XMFLOAT3 position, XMFLOAT3 direction,
                      XMFLOAT3 color, float intensity, float range,
                      float innerAngleDeg, float outerAngleDeg);

    // ---- Кадр ----
    // Возвращает командный список. Вызов включает:
    //   reset аллокатора, барьер, GBuffer geometry pass setup.
    ID3D12GraphicsCommandList* BeginFrame();

    // Привязывает geometry-pass PSO + root signature + текстуры + CB.
    void BindGeometryPass();

    // Рисует меш (VB + IB).
    void DrawMesh();

    // Переключается с Geometry Pass на Lighting Pass:
    //   - GBuffer RT → SRV
    //   - swap chain RT → RT
    //   - привязывает lighting PSO + fullscreen quad
    void BeginLightingPass();

    // Рисует full-screen quad (6 вершин, нет VB — позиции в шейдере).
    void DrawLightingQuad();

    // Закрывает список, ExecuteCommandLists, Present, MoveToNextFrame.
    void EndFrame();

    // ---- Resize ----
    void Resize(int width, int height);

    // ---- Геттеры ----
    ID3D12Device*              GetDevice()      const { return device_.Get(); }
    ID3D12GraphicsCommandList* GetCommandList() const { return commandList_.Get(); }
    const LightingPassCB&      GetLightingCB()  const { return lightingCB_; }
    int  GetWidth()  const { return width_; }
    int  GetHeight() const { return height_; }

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
    void CreateGeometryPassRootSignature();
    void CreateLightingPassRootSignature();
    void CreateGeometryPassPSO();
    void CreateLightingPassPSO();
    void CreateFullscreenQuadBuffers();

    // ---- Sync ----
    void WaitForGPU();
    void MoveToNextFrame();

    // ---- Helpers ----
    void MakeUploadBuffer(const void* data, UINT64 byteSize,
                          ComPtr<ID3D12Resource>& buf);
    ComPtr<ID3DBlob> CompileShader(const std::string& source,
                                   const std::string& entry,
                                   const std::string& target);

    // ---- Window ----
    HWND hwnd_;
    int  width_;
    int  height_;

    // ---- D3D12 core ----
    ComPtr<ID3D12Device>              device_;
    ComPtr<ID3D12CommandQueue>        commandQueue_;
    ComPtr<IDXGISwapChain3>           swapChain_;

    // ---- Descriptor heaps ----
    // RTV: 2 swapchain + 3 GBuffer = 5 total
    ComPtr<ID3D12DescriptorHeap>      rtvHeap_;
    ComPtr<ID3D12DescriptorHeap>      dsvHeap_;
    // SRV shader-visible: slot0-1 textures, slot4-6 GBuffer = 8 total
    ComPtr<ID3D12DescriptorHeap>      srvHeap_;
    UINT                              rtvDescSize_ = 0;
    UINT                              srvDescSize_ = 0;

    // ---- Swap chain RTs + depth ----
    ComPtr<ID3D12Resource>            renderTargets_[RS_FRAME_COUNT];
    D3D12_CPU_DESCRIPTOR_HANDLE       rtvHandles_[RS_FRAME_COUNT] = {};
    ComPtr<ID3D12Resource>            depthStencil_;
    D3D12_CPU_DESCRIPTOR_HANDLE       dsvHandle_ = {};

    // ---- GBuffer ----
    GBuffer gbuffer_;

    // ---- Command objects ----
    ComPtr<ID3D12CommandAllocator>    commandAllocators_[RS_FRAME_COUNT];
    ComPtr<ID3D12GraphicsCommandList> commandList_;

    // ---- Geometry Pass pipeline ----
    ComPtr<ID3D12RootSignature>       geometryRootSig_;
    ComPtr<ID3D12PipelineState>       geometryPSO_;

    // ---- Lighting Pass pipeline ----
    ComPtr<ID3D12RootSignature>       lightingRootSig_;
    ComPtr<ID3D12PipelineState>       lightingPSO_;

    // ---- Fullscreen quad (для lighting pass) ----
    // Нет VB — позиции генерируются из SV_VertexID в шейдере.
    // Но держим заглушку для будущей расширяемости.

    // ---- Геометрия сцены ----
    ComPtr<ID3D12Resource>            vertexBuffer_;
    ComPtr<ID3D12Resource>            indexBuffer_;
    D3D12_VERTEX_BUFFER_VIEW          vbView_ = {};
    D3D12_INDEX_BUFFER_VIEW           ibView_ = {};
    UINT                              indexCount_ = 0;

    // ---- Текстуры (slot 0 и 1) ----
    ComPtr<ID3D12Resource>            texture_;
    ComPtr<ID3D12Resource>            textureUpload_;
    ComPtr<ID3D12Resource>            texture2_;
    ComPtr<ID3D12Resource>            textureUpload2_;

    // ---- Константные буферы ----
    // Geometry Pass CB
    ComPtr<ID3D12Resource>            geometryCB_;
    ConstantBufferData*               geometryCBMapped_ = nullptr;

    // Lighting Pass CB
    ComPtr<ID3D12Resource>            lightingCBRes_;
    LightingPassCB*                   lightingCBMapped_ = nullptr;
    LightingPassCB                    lightingCB_ = {};  // CPU-копия

    // ---- Fence ----
    ComPtr<ID3D12Fence>               fence_;
    UINT64                            fenceValues_[RS_FRAME_COUNT] = {};
    HANDLE                            fenceEvent_ = nullptr;
    UINT                              frameIndex_ = 0;
};
