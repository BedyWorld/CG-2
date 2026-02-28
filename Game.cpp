#include "Game.h"
#include "Utils.h"
#include <cassert>

// ============================================================
//  Constructor / Destructor
// ============================================================
Game::Game(HWND hwnd, int width, int height)
    : hwnd_(hwnd), width_(width), height_(height)
{
    for (UINT i = 0; i < FRAME_COUNT; ++i)
        fenceValues_[i] = 0;
}

Game::~Game()
{
    // БАГ #1 ИСПРАВЛЕН: проверяем что все объекты синхронизации созданы,
    // прежде чем вызывать WaitForGPU(). Если Initialize() упал на полпути,
    // любой из них может быть nullptr — вызов через nullptr = краш в ntdll.
    if (commandQueue_ && fence_ && fenceEvent_)
    {
        try { WaitForGPU(); }
        catch (...) {} // деструктор никогда не должен бросать исключения
    }

    if (constantBuffer_ && cbMapped_)
        constantBuffer_->Unmap(0, nullptr);

    if (fenceEvent_)
        CloseHandle(fenceEvent_);
}

// ============================================================
//  Initialize
// ============================================================
bool Game::Initialize()
{
    try
    {
        CreateDevice();
        CreateCommandQueue();
        CreateSwapChain();
        CreateDescriptorHeaps();
        CreateRenderTargetViews();
        CreateDepthStencilBuffer();
        CreateCommandObjects();
        CreateFence();
        CreateRootSignature();
        CreatePSO();

        // ---- Open command list for geometry + texture uploads ----
        ThrowIfFailed(commandAllocators_[0]->Reset());
        ThrowIfFailed(commandList_->Reset(commandAllocators_[0].Get(), nullptr));

        // Try to load OBJ from exe directory, fall back to generated cube
        {
            wchar_t exePath[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            std::wstring dir(exePath);
            dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);

            ObjResult obj = LoadObj(dir + L"Oleg.obj");
            if (obj.valid)
                BuildBuffers(obj.vertices, obj.indices);
            else
                CreateGeometry();

            // ---- Текстура A (слот 0) ----
            TextureData td1 = LoadTextureWIC(dir + L"texture1.png");
            if (!td1.valid) td1 = LoadTextureWIC(dir + L"texture1.jpg");
            if (!td1.valid)
            {
                MessageBoxW(hwnd_,
                    (L"Не найден файл текстуры!\n\nОжидается:\n" + dir + L"texture1.png  (или .jpg)\n\n"
                     L"Скопируйте оба файла текстур рядом с .exe").c_str(),
                    L"Texture Not Found", MB_OK | MB_ICONERROR);
                ThrowIfFailed(E_FAIL, "texture1.png/.jpg not found next to .exe");
            }
            UploadTexture(td1, 0);

            // ---- Текстура B (слот 1) ----
            TextureData td2 = LoadTextureWIC(dir + L"texture2.png");
            if (!td2.valid) td2 = LoadTextureWIC(dir + L"texture2.jpg");
            if (!td2.valid)
            {
                MessageBoxW(hwnd_,
                    (L"Не найден файл текстуры!\n\nОжидается:\n" + dir + L"texture2.png  (или .jpg)\n\n"
                     L"Скопируйте оба файла текстур рядом с .exe").c_str(),
                    L"Texture Not Found", MB_OK | MB_ICONERROR);
                ThrowIfFailed(E_FAIL, "texture2.png/.jpg not found next to .exe");
            }
            UploadTexture(td2, 1);
        }

        // ---- Execute upload commands and wait ----
        ThrowIfFailed(commandList_->Close());
        ID3D12CommandList* lists[] = { commandList_.Get() };
        commandQueue_->ExecuteCommandLists(1, lists);
        WaitForGPU();

        // Safe to release both upload buffers now that GPU is done
        textureUpload_.Reset();
        textureUpload2_.Reset();

        CreateConstantBuffer();
    }
    catch (const std::exception& e)
    {
        MessageBoxA(hwnd_, e.what(), "D3D12 Error", MB_OK | MB_ICONERROR);
        return false;
    }
    return true;
}

// ============================================================
//  1. Device
// ============================================================
void Game::CreateDevice()
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
            IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND;
        ++i)
    {
        HRESULT hr = D3D12CreateDevice(adapter.Get(),
            D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_));
        if (SUCCEEDED(hr)) return;
    }
    ThrowIfFailed(E_FAIL, "No D3D12 device found");
}

// ============================================================
//  2. Command Queue
// ============================================================
void Game::CreateCommandQueue()
{
    D3D12_COMMAND_QUEUE_DESC desc = {};
    desc.Type  = D3D12_COMMAND_LIST_TYPE_DIRECT;
    desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    ThrowIfFailed(device_->CreateCommandQueue(&desc, IID_PPV_ARGS(&commandQueue_)));
}

// ============================================================
//  3. SwapChain
// ============================================================
void Game::CreateSwapChain()
{
    ComPtr<IDXGIFactory4> factory;
    ThrowIfFailed(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.BufferCount  = FRAME_COUNT;
    desc.Width        = static_cast<UINT>(width_);
    desc.Height       = static_cast<UINT>(height_);
    desc.Format       = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage  = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.SwapEffect   = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.SampleDesc   = { 1, 0 };

    ComPtr<IDXGISwapChain1> sc1;
    ThrowIfFailed(factory->CreateSwapChainForHwnd(
        commandQueue_.Get(), hwnd_, &desc, nullptr, nullptr, &sc1));
    factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
    ThrowIfFailed(sc1.As(&swapChain_));
    frameIndex_ = swapChain_->GetCurrentBackBufferIndex();
}

// ============================================================
//  4. Descriptor Heaps  (RTV + DSV + SRV)
// ============================================================
void Game::CreateDescriptorHeaps()
{
    // RTV
    {
        D3D12_DESCRIPTOR_HEAP_DESC d = {};
        d.NumDescriptors = FRAME_COUNT;
        d.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        ThrowIfFailed(device_->CreateDescriptorHeap(&d, IID_PPV_ARGS(&rtvHeap_)));
        rtvDescSize_ = device_->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }
    // DSV
    {
        D3D12_DESCRIPTOR_HEAP_DESC d = {};
        d.NumDescriptors = 1;
        d.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        ThrowIfFailed(device_->CreateDescriptorHeap(&d, IID_PPV_ARGS(&dsvHeap_)));
    }
    // SRV (shader-visible — 2 слота: текстура A (t0) и текстура B (t1))
    {
        D3D12_DESCRIPTOR_HEAP_DESC d = {};
        d.NumDescriptors = 2;   // ← два SRV для двух текстур
        d.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        d.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(device_->CreateDescriptorHeap(&d, IID_PPV_ARGS(&srvHeap_)));
    }
}

// ============================================================
//  5. Render Target Views
// ============================================================
void Game::CreateRenderTargetViews()
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle =
        rtvHeap_->GetCPUDescriptorHandleForHeapStart();

    for (UINT i = 0; i < FRAME_COUNT; ++i)
    {
        ThrowIfFailed(swapChain_->GetBuffer(i, IID_PPV_ARGS(&renderTargets_[i])));
        device_->CreateRenderTargetView(renderTargets_[i].Get(), nullptr, handle);
        handle.ptr += rtvDescSize_;
    }
}

// ============================================================
//  6. Depth-Stencil
// ============================================================
void Game::CreateDepthStencilBuffer()
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
    clear.Format                = DXGI_FORMAT_D32_FLOAT;
    clear.DepthStencil.Depth    = 1.0f;

    ThrowIfFailed(device_->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE,
        &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &clear, IID_PPV_ARGS(&depthStencil_)));

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format        = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device_->CreateDepthStencilView(depthStencil_.Get(),
        &dsvDesc, dsvHeap_->GetCPUDescriptorHandleForHeapStart());
}

// ============================================================
//  7. Command Allocators + Command List
// ============================================================
void Game::CreateCommandObjects()
{
    for (UINT i = 0; i < FRAME_COUNT; ++i)
        ThrowIfFailed(device_->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&commandAllocators_[i])));

    ThrowIfFailed(device_->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        commandAllocators_[0].Get(), nullptr,
        IID_PPV_ARGS(&commandList_)));

    commandList_->Close(); // will be Reset before use
}

// ============================================================
//  8. Fence
// ============================================================
void Game::CreateFence()
{
    ThrowIfFailed(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE,
        IID_PPV_ARGS(&fence_)));
    fenceValues_[0] = 1;
    fenceValues_[1] = 1;
    fenceEvent_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!fenceEvent_)
        ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
}

// ============================================================
//  9. Root Signature  (CBV at b0 + SRV table at t0 + static sampler s0)
// ============================================================
void Game::CreateRootSignature()
{
    // param[0]: CBV inline descriptor (constant buffer)
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].Descriptor.RegisterSpace  = 0;
    params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    // param[1]: descriptor table с 2 SRV (текстура A — t0, текстура B — t1)
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors     = 2;   // ← оба слота текстур
    srvRange.BaseShaderRegister = 0;
    srvRange.RegisterSpace      = 0;
    srvRange.OffsetInDescriptorsFromTableStart = 0;

    params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges   = &srvRange;
    params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    // Static sampler (wrap + linear filter)
    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MaxLOD           = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister   = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters     = 2;
    desc.pParameters       = params;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers   = &sampler;
    desc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> sig, err;
    ThrowIfFailed(D3D12SerializeRootSignature(&desc,
        D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err));
    ThrowIfFailed(device_->CreateRootSignature(
        0, sig->GetBufferPointer(), sig->GetBufferSize(),
        IID_PPV_ARGS(&rootSignature_)));
}

// ============================================================
//  10. PSO
// ============================================================
ComPtr<ID3DBlob> Game::CompileShader(const std::wstring& filename,
    const std::string& entry, const std::string& target)
{
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
    // БАГ #2 ИСПРАВЛЕН: D3DCompileFromFile возвращает E_FAIL (не HRESULT с кодом)
    // если файл не найден, и errors может быть nullptr — нужна явная проверка
    // существования файла, иначе пользователь не поймёт в чём проблема.
    if (GetFileAttributesW(filename.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        std::string msg = "Shader file not found!\nExpected path:\n";
        // конвертируем wstring -> string для MessageBoxA
        int len = WideCharToMultiByte(CP_UTF8, 0, filename.c_str(), -1,
                                      nullptr, 0, nullptr, nullptr);
        std::string narrow(len, 0);
        WideCharToMultiByte(CP_UTF8, 0, filename.c_str(), -1,
                            &narrow[0], len, nullptr, nullptr);
        msg += narrow;
        msg += "\n\nСкопируйте Shaders.hlsl рядом с .exe файлом.";
        MessageBoxA(hwnd_, msg.c_str(), "Shader Not Found", MB_OK | MB_ICONERROR);
        ThrowIfFailed(E_FAIL, "Shaders.hlsl not found next to .exe");
    }

    ComPtr<ID3DBlob> blob, errors;
    HRESULT hr = D3DCompileFromFile(
        filename.c_str(), nullptr, nullptr,
        entry.c_str(), target.c_str(), flags, 0, &blob, &errors);

    if (FAILED(hr))
    {
        if (errors)
            MessageBoxA(hwnd_,
                static_cast<char*>(errors->GetBufferPointer()),
                "Shader Compile Error", MB_OK | MB_ICONERROR);
        ThrowIfFailed(hr, "CompileShader failed");
    }
    return blob;
}

void Game::CreatePSO()
{
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring path(exePath);
    path = path.substr(0, path.find_last_of(L"\\/") + 1) + L"Shaders.hlsl";

    auto vs = CompileShader(path, "VSMain", "vs_5_0");
    auto ps = CompileShader(path, "PSMain", "ps_5_0");

    // Input layout now includes TEXCOORD
    D3D12_INPUT_ELEMENT_DESC layout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT,  0, 24,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,        0, 40,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_RASTERIZER_DESC raster = {};
    raster.FillMode        = D3D12_FILL_MODE_SOLID;
    raster.CullMode        = D3D12_CULL_MODE_BACK;
    raster.DepthClipEnable = TRUE;

    D3D12_DEPTH_STENCIL_DESC ds = {};
    ds.DepthEnable    = TRUE;
    ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc      = D3D12_COMPARISON_FUNC_LESS;

    D3D12_BLEND_DESC blend = {};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout           = { layout, _countof(layout) };
    psoDesc.pRootSignature        = rootSignature_.Get();
    psoDesc.VS                    = { vs->GetBufferPointer(), vs->GetBufferSize() };
    psoDesc.PS                    = { ps->GetBufferPointer(), ps->GetBufferSize() };
    psoDesc.RasterizerState       = raster;
    psoDesc.BlendState            = blend;
    psoDesc.DepthStencilState     = ds;
    psoDesc.SampleMask            = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets      = 1;
    psoDesc.RTVFormats[0]         = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.DSVFormat             = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc            = { 1, 0 };

    ThrowIfFailed(device_->CreateGraphicsPipelineState(
        &psoDesc, IID_PPV_ARGS(&pso_)));
}

// ============================================================
//  11. Geometry helpers
// ============================================================
void Game::MakeUploadBuffer(const void* data, UINT64 byteSize,
                             ComPtr<ID3D12Resource>& buf)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width            = byteSize;
    rd.Height           = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.SampleDesc       = { 1, 0 };
    rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ThrowIfFailed(device_->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&buf)));

    void* mapped = nullptr;
    D3D12_RANGE range = { 0, 0 };
    buf->Map(0, &range, &mapped);
    memcpy(mapped, data, static_cast<size_t>(byteSize));
    buf->Unmap(0, nullptr);
}

void Game::BuildBuffers(const std::vector<Vertex>& verts,
                         const std::vector<UINT>&   idxs)
{
    indexCount_ = static_cast<UINT>(idxs.size());

    UINT64 vbSize = verts.size() * sizeof(Vertex);
    UINT64 ibSize = idxs.size()  * sizeof(UINT);

    MakeUploadBuffer(verts.data(), vbSize, vertexBuffer_);
    MakeUploadBuffer(idxs.data(),  ibSize, indexBuffer_);

    vbView_.BufferLocation = vertexBuffer_->GetGPUVirtualAddress();
    vbView_.SizeInBytes    = static_cast<UINT>(vbSize);
    vbView_.StrideInBytes  = sizeof(Vertex);

    ibView_.BufferLocation = indexBuffer_->GetGPUVirtualAddress();
    ibView_.SizeInBytes    = static_cast<UINT>(ibSize);
    ibView_.Format         = DXGI_FORMAT_R32_UINT;
}

std::vector<Vertex> Game::GenerateCube()
{
    float s = 0.5f;
    // UV coordinates: each face gets its own 0-1 UV square
    Vertex vData[] =
    {
        // Front (+Z) red
        { {-s,-s, s}, {0,0,1}, {0.9f,0.2f,0.2f,1}, {0,1} },
        { { s,-s, s}, {0,0,1}, {0.9f,0.2f,0.2f,1}, {1,1} },
        { { s, s, s}, {0,0,1}, {0.9f,0.2f,0.2f,1}, {1,0} },
        { {-s, s, s}, {0,0,1}, {0.9f,0.2f,0.2f,1}, {0,0} },
        // Back (-Z) green
        { { s,-s,-s}, {0,0,-1}, {0.2f,0.8f,0.2f,1}, {0,1} },
        { {-s,-s,-s}, {0,0,-1}, {0.2f,0.8f,0.2f,1}, {1,1} },
        { {-s, s,-s}, {0,0,-1}, {0.2f,0.8f,0.2f,1}, {1,0} },
        { { s, s,-s}, {0,0,-1}, {0.2f,0.8f,0.2f,1}, {0,0} },
        // Left (-X) blue
        { {-s,-s,-s}, {-1,0,0}, {0.2f,0.2f,0.9f,1}, {0,1} },
        { {-s,-s, s}, {-1,0,0}, {0.2f,0.2f,0.9f,1}, {1,1} },
        { {-s, s, s}, {-1,0,0}, {0.2f,0.2f,0.9f,1}, {1,0} },
        { {-s, s,-s}, {-1,0,0}, {0.2f,0.2f,0.9f,1}, {0,0} },
        // Right (+X) yellow
        { { s,-s, s}, {1,0,0}, {0.9f,0.9f,0.1f,1}, {0,1} },
        { { s,-s,-s}, {1,0,0}, {0.9f,0.9f,0.1f,1}, {1,1} },
        { { s, s,-s}, {1,0,0}, {0.9f,0.9f,0.1f,1}, {1,0} },
        { { s, s, s}, {1,0,0}, {0.9f,0.9f,0.1f,1}, {0,0} },
        // Top (+Y) cyan
        { {-s, s, s}, {0,1,0}, {0.1f,0.9f,0.9f,1}, {0,1} },
        { { s, s, s}, {0,1,0}, {0.1f,0.9f,0.9f,1}, {1,1} },
        { { s, s,-s}, {0,1,0}, {0.1f,0.9f,0.9f,1}, {1,0} },
        { {-s, s,-s}, {0,1,0}, {0.1f,0.9f,0.9f,1}, {0,0} },
        // Bottom (-Y) magenta
        { {-s,-s,-s}, {0,-1,0}, {0.9f,0.1f,0.9f,1}, {0,1} },
        { { s,-s,-s}, {0,-1,0}, {0.9f,0.1f,0.9f,1}, {1,1} },
        { { s,-s, s}, {0,-1,0}, {0.9f,0.1f,0.9f,1}, {1,0} },
        { {-s,-s, s}, {0,-1,0}, {0.9f,0.1f,0.9f,1}, {0,0} },
    };
    return std::vector<Vertex>(vData, vData + _countof(vData));
}

std::vector<UINT> Game::GenerateCubeIndices()
{
    std::vector<UINT> idx;
    for (UINT f = 0; f < 6; ++f)
    {
        UINT b = f * 4;
        idx.push_back(b+0); idx.push_back(b+1); idx.push_back(b+2);
        idx.push_back(b+0); idx.push_back(b+2); idx.push_back(b+3);
    }
    return idx;
}

void Game::CreateGeometry()
{
    auto verts = GenerateCube();
    auto idxs  = GenerateCubeIndices();
    BuildBuffers(verts, idxs);
}

void Game::LoadModel(const std::wstring& objPath)
{
    ObjResult obj = LoadObj(objPath);
    if (!obj.valid) { CreateGeometry(); return; }
    BuildBuffers(obj.vertices, obj.indices);
}

// ============================================================
//  12. Texture Upload (requires open command list)
//      slot 0 = gTexture (t0), slot 1 = gTexture2 (t1)
// ============================================================
void Game::UploadTexture(const TextureData& td, int slot)
{
    // Защита: если TextureData невалидна — текстура не загружена с диска.
    // Автогенерация текстур отключена: программа должна получить реальные файлы.
    if (!td.valid || td.width == 0 || td.height == 0)
        ThrowIfFailed(E_INVALIDARG,
            "UploadTexture: TextureData is invalid (width/height == 0). "
            "Texture file was not loaded from disk.");

    // Выбираем нужные ComPtr-члены в зависимости от слота
    ComPtr<ID3D12Resource>& texRes    = (slot == 0) ? texture_       : texture2_;
    ComPtr<ID3D12Resource>& uploadRes = (slot == 0) ? textureUpload_ : textureUpload2_;

    // --- Create default-heap texture resource (COPY_DEST) ---
    D3D12_HEAP_PROPERTIES defaultHeap = {};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width            = td.width;
    texDesc.Height           = td.height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels        = 1;
    texDesc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc       = { 1, 0 };

    ThrowIfFailed(device_->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE,
        &texDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&texRes)));

    // --- Figure out upload buffer size ---
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT   numRows = 0;
    UINT64 rowSize = 0, uploadSize = 0;
    device_->GetCopyableFootprints(
        &texDesc, 0, 1, 0, &footprint, &numRows, &rowSize, &uploadSize);

    // --- Create upload heap buffer ---
    D3D12_HEAP_PROPERTIES uploadHeap = {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width            = uploadSize;
    bufDesc.Height           = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels        = 1;
    bufDesc.SampleDesc       = { 1, 0 };
    bufDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ThrowIfFailed(device_->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE,
        &bufDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&uploadRes)));

    // --- Copy pixel rows into upload buffer ---
    uint8_t* mapped = nullptr;
    uploadRes->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
    UINT srcRowPitch = td.width * 4; // 4 bytes per RGBA pixel
    for (UINT row = 0; row < td.height; ++row)
    {
        memcpy(mapped + (UINT64)footprint.Footprint.RowPitch * row,
               td.pixels.data() + (UINT64)srcRowPitch * row,
               srcRowPitch);
    }
    uploadRes->Unmap(0, nullptr);

    // --- Record CopyTextureRegion into open command list ---
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource       = uploadRes.Get();
    src.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = footprint;

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource        = texRes.Get();
    dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    commandList_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    // --- Transition texture: COPY_DEST -> PIXEL_SHADER_RESOURCE ---
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = texRes.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList_->ResourceBarrier(1, &barrier);

    // --- Create SRV в нужном слоте heap'а ---
    UINT srvDescSize = device_->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_CPU_DESCRIPTOR_HANDLE srvHandle =
        srvHeap_->GetCPUDescriptorHandleForHeapStart();
    srvHandle.ptr += slot * srvDescSize;   // слот 0 или 1

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels     = 1;
    device_->CreateShaderResourceView(texRes.Get(), &srvDesc, srvHandle);
}

// ============================================================
//  13. Constant Buffer (persistently mapped)
// ============================================================
void Game::CreateConstantBuffer()
{
    UINT64 cbSize = sizeof(ConstantBufferData); // == 256

    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width            = cbSize;
    rd.Height           = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.SampleDesc       = { 1, 0 };
    rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ThrowIfFailed(device_->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&constantBuffer_)));

    D3D12_RANGE range = { 0, 0 };
    constantBuffer_->Map(0, &range,
        reinterpret_cast<void**>(&cbMapped_));
}

// ============================================================
//  Update  (rotation + UV animation)
// ============================================================
void Game::Update(float deltaTime)
{
    rotationAngle_ += 0.8f * deltaTime;

    // UV scroll animation
    uvOffsetX_ += 0.5f * deltaTime;  // scroll horizontally
    uvOffsetY_ += 0.2f * deltaTime;  // scroll vertically (slower)
    // Wrap to [0, 1) to avoid floating-point drift
    if (uvOffsetX_ > 1.0f) uvOffsetX_ -= 1.0f;
    if (uvOffsetY_ > 1.0f) uvOffsetY_ -= 1.0f;

    XMMATRIX world  = XMMatrixRotationY(rotationAngle_)
                    * XMMatrixRotationX(0.3f);
    XMVECTOR eye    = XMVectorSet(0.0f, 1.0f, -3.0f, 1.0f);
    XMVECTOR target = XMVectorSet(0.0f, 0.0f,  0.0f, 1.0f);
    XMVECTOR up     = XMVectorSet(0.0f, 1.0f,  0.0f, 0.0f);
    XMMATRIX view   = XMMatrixLookAtLH(eye, target, up);

    float aspect = (height_ > 0)
        ? static_cast<float>(width_) / height_ : 1.0f;
    XMMATRIX proj = XMMatrixPerspectiveFovLH(
        XMConvertToRadians(60.0f), aspect, 0.1f, 100.0f);

    ConstantBufferData cb = {};
    cb.World      = XMMatrixTranspose(world);
    cb.View       = XMMatrixTranspose(view);
    cb.Proj       = XMMatrixTranspose(proj);
    cb.LightPos   = XMFLOAT4(2.0f,  3.0f, -2.0f, 0.0f);
    cb.LightColor = XMFLOAT4(1.0f,  1.0f,  1.0f, 1.0f);
    cb.CameraPos  = XMFLOAT4(0.0f,  1.0f, -3.0f, 1.0f);
    cb.Tiling     = XMFLOAT2(8.0f,  8.0f);         // tile texture 2x2
    cb.UVOffset   = XMFLOAT2(uvOffsetX_, uvOffsetY_);

    memcpy(cbMapped_, &cb, sizeof(cb));
}

// ============================================================
//  Render
// ============================================================
void Game::Render()
{
    PopulateCommandList();

    ID3D12CommandList* lists[] = { commandList_.Get() };
    commandQueue_->ExecuteCommandLists(1, lists);

    ThrowIfFailed(swapChain_->Present(1, 0));
    MoveToNextFrame();
}

void Game::PopulateCommandList()
{
    ThrowIfFailed(commandAllocators_[frameIndex_]->Reset());
    ThrowIfFailed(commandList_->Reset(
        commandAllocators_[frameIndex_].Get(), pso_.Get()));

    D3D12_VIEWPORT vp = {};
    vp.Width    = static_cast<float>(width_);
    vp.Height   = static_cast<float>(height_);
    vp.MaxDepth = 1.0f;

    D3D12_RECT scissor = { 0, 0, width_, height_ };
    commandList_->RSSetViewports(1, &vp);
    commandList_->RSSetScissorRects(1, &scissor);

    // Barrier: Present -> RenderTarget
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = renderTargets_[frameIndex_].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList_->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv =
        rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += frameIndex_ * rtvDescSize_;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv =
        dsvHeap_->GetCPUDescriptorHandleForHeapStart();

    commandList_->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

    const float clearColor[] = { 0.05f, 0.08f, 0.18f, 1.0f };
    commandList_->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
    commandList_->ClearDepthStencilView(
        dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    // Bind SRV heap + root signature
    ID3D12DescriptorHeap* heaps[] = { srvHeap_.Get() };
    commandList_->SetDescriptorHeaps(1, heaps);
    commandList_->SetGraphicsRootSignature(rootSignature_.Get());

    // param[0]: CBV
    commandList_->SetGraphicsRootConstantBufferView(
        0, constantBuffer_->GetGPUVirtualAddress());
    // param[1]: SRV descriptor table (texture)
    commandList_->SetGraphicsRootDescriptorTable(
        1, srvHeap_->GetGPUDescriptorHandleForHeapStart());

    commandList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList_->IASetVertexBuffers(0, 1, &vbView_);
    commandList_->IASetIndexBuffer(&ibView_);
    commandList_->DrawIndexedInstanced(indexCount_, 1, 0, 0, 0);

    // Barrier: RenderTarget -> Present
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    commandList_->ResourceBarrier(1, &barrier);

    ThrowIfFailed(commandList_->Close());
}

// ============================================================
//  Resize
// ============================================================
void Game::Resize(int width, int height)
{
    // БАГ #3 ИСПРАВЛЕН: WM_SIZE может прийти с width=0/height=0 при минимизации
    if (width <= 0 || height <= 0) return;
    // Дополнительная защита: если объекты ещё не созданы — выходим
    if (!swapChain_ || !device_) return;

    WaitForGPU();

    width_  = width;
    height_ = height;

    for (UINT i = 0; i < FRAME_COUNT; ++i)
        renderTargets_[i].Reset();
    depthStencil_.Reset();

    DXGI_SWAP_CHAIN_DESC1 scDesc = {};
    swapChain_->GetDesc1(&scDesc);
    ThrowIfFailed(swapChain_->ResizeBuffers(
        FRAME_COUNT,
        static_cast<UINT>(width_),
        static_cast<UINT>(height_),
        scDesc.Format, 0));

    frameIndex_ = swapChain_->GetCurrentBackBufferIndex();
    CreateRenderTargetViews();
    CreateDepthStencilBuffer();
}

// ============================================================
//  Sync
// ============================================================
void Game::WaitForGPU()
{
    ThrowIfFailed(commandQueue_->Signal(
        fence_.Get(), fenceValues_[frameIndex_]));
    ThrowIfFailed(fence_->SetEventOnCompletion(
        fenceValues_[frameIndex_], fenceEvent_));
    ::WaitForSingleObjectEx(fenceEvent_, INFINITE, FALSE);
    ++fenceValues_[frameIndex_];
}

void Game::MoveToNextFrame()
{
    const UINT64 current = fenceValues_[frameIndex_];
    ThrowIfFailed(commandQueue_->Signal(fence_.Get(), current));
    frameIndex_ = swapChain_->GetCurrentBackBufferIndex();

    if (fence_->GetCompletedValue() < fenceValues_[frameIndex_])
    {
        ThrowIfFailed(fence_->SetEventOnCompletion(
            fenceValues_[frameIndex_], fenceEvent_));
        ::WaitForSingleObjectEx(fenceEvent_, INFINITE, FALSE);
    }
    fenceValues_[frameIndex_] = current + 1;
}
