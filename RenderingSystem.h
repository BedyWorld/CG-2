#pragma once
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <vector>
#include <string>
#include <array>
#include <unordered_map>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxguid.lib")

#include "Types.h"
#include "GBuffer.h"
#include "ShadowMap.h"
#include "ParticleSystem.h"
#include "TextureLoader.h"
#include "Utils.h"

using Microsoft::WRL::ComPtr;

static const UINT RS_FRAME_COUNT = 2;

// ============================================================
//  SubDrawCall — один draw call для geometry pass
// ============================================================
struct SubDrawCall
{
    UINT indexStart = 0;
    UINT indexCount = 0;
    int  srvDiffuse = 0;   // SRV-слот диффузной текстуры  (t0)
    int  srvNormal = 0;   // SRV-слот normal map           (t2)
    int  srvDisp = 0;   // SRV-слот displacement/height  (t3)
};

// ============================================================
//  SRV-хип раскладка (256 слотов достаточно):
//    slot 0          — fallback albedo (серый 200,200,200)
//    slot 1          — fallback normal (flat 128,128,255)
//    slot 2          — fallback disp   (нейтральный 128)
//    slot 3          — albedo2 общий   (для blending)
//    slot 4..7       — GBuffer (Albedo, Normal, PBR, WorldPos)
//    slot 8+         — уникальные текстуры (кэшируются по пути)
//
//  Root Signature geometry pass (5 параметров):
//    param[0] = CBV  b0           — ConstantBufferData
//    param[1] = SRV  t0 (inline)  — diffuse текущего материала
//    param[2] = SRV  t1 (inline)  — albedo2 (общий, slot 3)
//    param[3] = SRV  t2 (inline)  — normal map
//    param[4] = SRV  t3 (inline)  — displacement (общий, slot 2)
//  Inline SRV не требуют блоков подряд — каждый указывается
//  отдельным SetGraphicsRootShaderResourceView(slot, gpuVA).
//  НО: inline SRV требует буфер (не текстуру) либо DescriptorTable.
//  Используем DescriptorTable по 1 дескриптору каждый:
//    param[1] = DescriptorTable { range: 1 SRV t0 }
//    param[2] = DescriptorTable { range: 1 SRV t1 }
//    param[3] = DescriptorTable { range: 1 SRV t2 }
//    param[4] = DescriptorTable { range: 1 SRV t3 }
//  Тогда SetGraphicsRootDescriptorTable(N, gpuHandle) на каждый слот.
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
    void ReleaseTextureUploadBuffers();

    // ---- Геометрия ----
    void BuildMesh1(const std::vector<Vertex>& verts,
        const std::vector<UINT>& idxs,
        const std::vector<UINT>& subIndexStarts,
        const std::vector<UINT>& subIndexCounts,
        const std::vector<std::wstring>& subDiffuse,
        const std::vector<std::wstring>& subNormal,
        const std::vector<std::wstring>& subDisp);

    void BuildBuffers2(const std::vector<Vertex>& verts, const std::vector<UINT>& idxs);

    // ---- Текстуры mesh2 (slot 0..3) ----
    void UploadTextureMesh2(const TextureData& td, int slot);

    // ---- Константные буферы ----
    void CreateConstantBuffers();
    void UpdateGeometryPassCB(const ConstantBufferData& data);
    void UpdateGeometryPassCB2(const ConstantBufferData& data);
    void UpdateLightingPassCB(const LightingPassCB& data);

    // ---- Источники света ----
    void ClearLights();
    void AddDirectionalLight(XMFLOAT3 direction, XMFLOAT3 color, float intensity);
    void AddPointLight(XMFLOAT3 position, XMFLOAT3 color, float intensity, float range);
    void AddSpotLight(XMFLOAT3 position, XMFLOAT3 direction,
        XMFLOAT3 color, float intensity, float range,
        float innerAngleDeg, float outerAngleDeg);

    // ---- Кадр ----
    ID3D12GraphicsCommandList* BeginFrame();

    // ---- ДЗ №4: инстансинг разбросанных объектов ----
    // Все видимые копии mesh2 рисуются одним DrawIndexedInstanced.
    // Матрицы приходят структурированным буфером и читаются в VS по
    // SV_InstanceID, поэтому число объектов не увеличивает число draw call.
    static const UINT RS_MAX_INSTANCES = 20000;
    void UpdateInstances(const std::vector<XMFLOAT4X4>& visible,
        const std::vector<XMFLOAT4X4>& all);
    void BindInstancedPass();
    void DrawInstances(UINT instanceCount);

    // ---- ДЗ №5: каскадные карты теней ----
    CascadedShadowMap& Shadows() { return shadows_; }
    void BeginShadowCascade(int cascade, const XMFLOAT4X4& lightViewProj);
    void DrawShadowMesh1();                       // Sponza одним вызовом
    // Только перечисленные субмеши: дороже по draw call'ам, но дешевле
    // по геометрии, когда каскад покрывает малую часть сцены.
    void DrawShadowMesh1Sub(const std::vector<int>& visible);
    void DrawShadowInstances(UINT instanceCount); // разбросанные объекты
    void EndShadowPass();

    // ---- ДЗ №6: система частиц ----
    ParticleSystem& Particles() { return particles_; }
    void SimulateParticles(const ParticleCB& cb);
    void DrawParticles();

    // Каркасный режим: видно результат тесселяции
    void SetWireframe(bool on) { wireframe_ = on; }
    bool IsWireframe() const { return wireframe_; }

    void BindGeometryPass();        // биндит PSO, CB1, общие текстуры
    // visible == nullptr -> рисуем все субмеши (culling выключен).
    // Иначе — только перечисленные, прошедшие frustum culling.
    void DrawMesh1SubMeshes(const std::vector<int>* visible = nullptr);
    int  SubMeshCount() const { return static_cast<int>(subDrawCalls_.size()); }
    void BindGeometryPassMesh2();
    void DrawMesh2();
    void BeginLightingPass();
    void DrawLightingQuad();
    void EndFrame();

    // ---- Resize ----
    void Resize(int width, int height);

    // ---- Геттеры ----
    ID3D12Device* GetDevice()      const { return device_.Get(); }
    ID3D12GraphicsCommandList* GetCommandList() const { return commandList_.Get(); }
    const LightingPassCB& GetLightingCB()  const { return lightingCB_; }
    int  GetWidth()  const { return width_; }
    int  GetHeight() const { return height_; }
    bool HasMesh2()  const { return indexCount2_ > 0; }

private:
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
    void CreateInstancedPSO();
    void CreateShadowPassRootSignature();
    void CreateShadowPSOs();
    void CreateLightingPassPSO();
    void WaitForGPU();
    void MoveToNextFrame();
    void MakeUploadBuffer(const void* data, UINT64 byteSize, ComPtr<ID3D12Resource>& buf);
    ComPtr<ID3DBlob> CompileShader(const std::string& source,
        const std::string& entry,
        const std::string& target);

    // Загрузить текстуру и вернуть её SRV-слот (с кэшированием по пути)
    int  LoadAndCacheTexture(const std::wstring& path, bool isNormal);
    // Displacement: пробуем файл, иначе восстанавливаем рельеф из карты нормалей
    int  LoadAndCacheDisplacement(const std::wstring& dispPath,
        const std::wstring& normalPath);
    // Физически загрузить TextureData в хип, занять слот
    int  UploadTextureToHeap(const TextureData& td);
    void CreateSRVInSlot(ID3D12Resource* res, DXGI_FORMAT fmt, int slot);
    // GPU-handle по номеру слота
    D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle(int slot) const;

    HWND hwnd_;
    int  width_, height_;

    ComPtr<ID3D12Device>              device_;
    ComPtr<ID3D12CommandQueue>        commandQueue_;
    ComPtr<IDXGISwapChain3>           swapChain_;

    ComPtr<ID3D12DescriptorHeap>      rtvHeap_;
    ComPtr<ID3D12DescriptorHeap>      dsvHeap_;
    ComPtr<ID3D12DescriptorHeap>      srvHeap_;
    UINT                              rtvDescSize_ = 0;
    UINT                              srvDescSize_ = 0;
    int                               srvNextFree_ = 0;

    // Хип на 256 дескрипторов — достаточно для ~60 уникальных текстур + GBuffer + reserved
    static const int SRV_HEAP_SIZE = 256;
    static const int SRV_GBUF_BASE = 4;   // слоты 4..7 — GBuffer

    // Зарезервированные слоты (заполняются в BuildMesh1/BeginResourceUpload)
    static const int SRV_FALLBACK_ALBEDO = 0;
    static const int SRV_FALLBACK_NORMAL = 1;
    static const int SRV_FALLBACK_DISP = 2;
    static const int SRV_COMMON_ALBEDO2 = 3;

    ComPtr<ID3D12Resource>            renderTargets_[RS_FRAME_COUNT];
    D3D12_CPU_DESCRIPTOR_HANDLE       rtvHandles_[RS_FRAME_COUNT] = {};
    ComPtr<ID3D12Resource>            depthStencil_;
    D3D12_CPU_DESCRIPTOR_HANDLE       dsvHandle_ = {};

    GBuffer gbuffer_;

    ComPtr<ID3D12CommandAllocator>    commandAllocators_[RS_FRAME_COUNT];
    ComPtr<ID3D12GraphicsCommandList> commandList_;

    ComPtr<ID3D12RootSignature>       geometryRootSig_;
    ComPtr<ID3D12PipelineState>       geometryPSO_;
    // Тот же конвейер, отличается только FillMode. Отдельный PSO нужен
    // потому, что режим заливки — часть неизменяемого состояния.
    ComPtr<ID3D12PipelineState>       geometryWirePSO_;
    // Отдельный PSO без тесселяции: гонять несколько тысяч объектов
    // через HS/DS бессмысленно дорого, а рельеф на них не нужен.
    ComPtr<ID3D12PipelineState>       instancedPSO_;

    // ---- ДЗ №5 ----
    CascadedShadowMap                 shadows_;
    ParticleSystem                    particles_;
    static const int SRV_PARTICLES = 9;   // блок из 7 слотов
    ComPtr<ID3D12RootSignature>       shadowRootSig_;
    ComPtr<ID3D12PipelineState>       shadowPSO_;        // Sponza
    ComPtr<ID3D12PipelineState>       shadowInstancedPSO_;
    ComPtr<ID3D12Resource>            shadowCB_;
    BYTE* shadowCBMapped_ = nullptr;
    UINT64                            shadowCBStride_ = 0;
    int                               shadowCBSlot_ = 0;  // блок текущего каскада
    // Слот 4 занимать НЕЛЬЗЯ: 4..7 принадлежат G-buffer, и запись туда
    // затирала дескриптор albedo — проход освещения читал карту теней
    // вместо цвета.
    static const int SRV_SHADOWMAP = 8;   // слот массива теней в SRV-хипе
    ComPtr<ID3D12Resource>            instanceBuffer_;
    BYTE* instanceMapped_ = nullptr;
    UINT64                            instanceStride_ = 0;
    UINT                              instanceCount_ = 0;
    UINT                              shadowInstanceCount_ = 0;
    static const UINT64 SHADOW_INSTANCE_OFFSET =
        (UINT64)RS_MAX_INSTANCES * 64;   // sizeof(XMFLOAT4X4)
    ComPtr<ID3D12RootSignature>       lightingRootSig_;
    ComPtr<ID3D12PipelineState>       lightingPSO_;

    // ---- Mesh1 ----
    ComPtr<ID3D12Resource>            vertexBuffer_;
    ComPtr<ID3D12Resource>            indexBuffer_;
    D3D12_VERTEX_BUFFER_VIEW          vbView_ = {};
    D3D12_INDEX_BUFFER_VIEW           ibView_ = {};
    UINT                              indexCount_ = 0;
    std::vector<SubDrawCall>          subDrawCalls_;

    // ---- Mesh2 ----
    ComPtr<ID3D12Resource>            vertexBuffer2_;
    ComPtr<ID3D12Resource>            indexBuffer2_;
    D3D12_VERTEX_BUFFER_VIEW          vbView2_ = {};
    D3D12_INDEX_BUFFER_VIEW           ibView2_ = {};
    UINT                              indexCount2_ = 0;
    // Mesh2 текстуры: 4 слота начиная с mesh2SrvBase_
    int                               mesh2SrvBase_ = 0;

    // Все загруженные GPU-ресурсы (для lifetime)
    std::vector<ComPtr<ID3D12Resource>> texResources_;
    std::vector<ComPtr<ID3D12Resource>> texUploads_;

    // Кэш путь -> SRV слот
    std::unordered_map<std::wstring, int> texCache_;

    // ---- CB ----
    // ---- Constant buffers ----
    // Каждый CB выделен как кольцо из RS_FRAME_COUNT выровненных блоков.
    // CPU опережает GPU на кадр (см. MoveToNextFrame), поэтому запись в
    // один-единственный буфер затирала данные, которые GPU ещё читает,
    // — отсюда дёрганье матриц и мерцание освещения.
    ComPtr<ID3D12Resource>            geometryCB_;
    BYTE* geometryCBMapped_ = nullptr;
    UINT64                            geometryCBStride_ = 0;
    ComPtr<ID3D12Resource>            geometryCB2_;
    BYTE* geometryCB2Mapped_ = nullptr;
    UINT64                            geometryCB2Stride_ = 0;
    ComPtr<ID3D12Resource>            lightingCBRes_;
    BYTE* lightingCBMapped_ = nullptr;
    UINT64                            lightingCBStride_ = 0;
    LightingPassCB                    lightingCB_ = {};

    ComPtr<ID3D12Fence>               fence_;
    UINT64                            fenceValues_[RS_FRAME_COUNT] = {};
    HANDLE                            fenceEvent_ = nullptr;
    UINT                              frameIndex_ = 0;
    bool                              wireframe_ = false;
};