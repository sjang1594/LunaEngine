#include "LunaPCH.h"
#include "Logger/Logger.h"
#include "LunaEngine/Graphics/IBuffer.h"
#include "LunaEngine/Graphics/Material.h"
#include "LunaEngine/Renderer/DX12/Public/DX12Backend.h"
#include "LunaEngine/Renderer/DX12/Public/DX12Pipeline.h"
#include "LunaEngine/Renderer/DX12/Public/DX12Device.h"
#include "LunaEngine/Renderer/DX12/Public/DX12AccelStructure.h"
#include "LunaEngine/Renderer/DX12/Public/DX12RTPipeline.h"
#include "Renderer/DX12/Public/DX12Buffer.h"
#include "Renderer/MeshLoader.h"

namespace Luna
{

// ---------------------------------------------------------------------------
// Test geometry — simple triangle kept from Phase 1 as a fallback visual
// ---------------------------------------------------------------------------
struct TriangleVertex
{
    Vec3 position;
    Vec4 color;
};

static const TriangleVertex s_Vertices[] = {
    {{ 0.0f,  0.5f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}}, // top    (red)
    {{ 0.5f, -0.5f, 0.0f}, {0.0f, 1.0f, 0.0f, 1.0f}}, // right  (green)
    {{-0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f, 1.0f}}, // left   (blue)
};

// MVP constant buffer layout — must match cbuffer in constantbuffer.vert.hlsl
struct MVPConstants
{
    XMFLOAT4X4 model;
    XMFLOAT4X4 view;
    XMFLOAT4X4 proj;
};

// Scene constant buffer — must match cbuffer b2 (SceneBuffer) in pbr.frag.hlsl
struct SceneConstants
{
    XMFLOAT4X4 invViewProj;       // inverse(view * proj) for world-space reconstruction
    XMFLOAT4   lightDirection;    // world-space directional light dir (xyz), unused w
    XMFLOAT4   lightColor;        // linear light color (rgb), intensity in w
    XMFLOAT4   viewPosition;      // world-space camera position (xyz), unused w
    XMFLOAT2   viewportSize;      // render target dimensions in pixels
    XMFLOAT2   _scenePad;
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
DX12Backend::DX12Backend() = default;

DX12Backend::~DX12Backend()
{
    Shutdown();
}

void DX12Backend::Shutdown()
{
    if (_fenceEvent)
    {
        WaitAllFrames();
        CloseHandle(_fenceEvent);
        _fenceEvent = nullptr;
    }

    // Release D3D12MA allocations before destroying the allocator.
    // Scene meshes must be released first (their Allocation::Release() needs the allocator alive).
    _sceneMeshes.clear();

    if (_vertexBufferAlloc)    { _vertexBufferAlloc->Release();    _vertexBufferAlloc    = nullptr; }
    if (_shadowUAVAlloc)       { _shadowUAVAlloc->Release();       _shadowUAVAlloc       = nullptr; }
    if (_fallbackShadowAlloc)  { _fallbackShadowAlloc->Release();  _fallbackShadowAlloc  = nullptr; }

    _rtPipeline.reset();
    _accelStructure.reset();

    if (_d3d12maAllocator)
    {
        _d3d12maAllocator->Release();
        _d3d12maAllocator = nullptr;
    }
}

bool DX12Backend::Init(void *windowHandler, uint32_t width, uint32_t height)
{
    _mainWindow     = static_cast<HWND>(windowHandler);
    _screenViewport = {0.0f, 0.0f, static_cast<FLOAT>(width), static_cast<FLOAT>(height), 0.0f, 1.0f};
    _scissorRect    = CD3DX12_RECT(0, 0, static_cast<LONG>(width), static_cast<LONG>(height));
    SetResolution(width, height);

    _dx12Device   = std::make_unique<DX12Device>();
    _device       = _dx12Device->GetDeviceComPtr();
    _mdxgiFactory = _dx12Device->GetDXGIFactory();

    if (!InitD3D12MA())                        return false;
    if (!CreateCommandQueueAndFenceEvent())    return false;
    if (!CreateSwapChain())                    return false;
    if (!CreateRenderTarget())                 return false;
    if (!CreateImGuiRenderTarget())            return false;
    if (!CreateDepthBuffer())                  return false;
    if (!CreateVertexBuffer())                 return false;
    if (!CreateMVPConstantBuffer())            return false;

    // CBV pipeline for the test triangle (TriangleVertex layout)
    _cbvPipeline = std::make_unique<DX12Pipeline>();
    PipelineStateDesc cbvDesc;
    cbvDesc.enableDepthTest = true;
    cbvDesc.vertexLayout    = VertexLayout::Triangle;
    if (!_cbvPipeline->Initialize(_device, L"constantbuffer.vert.hlsl", L"constantbuffer.frag.hlsl", cbvDesc))
    {
        LUNA_LOG_ERROR("Failed to initialize cbv pipeline");
        return false;
    }

    // Mesh preview pipeline — PBRVertex layout, normal-diffuse shading
    _meshPreviewPipeline = std::make_unique<DX12Pipeline>();
    PipelineStateDesc previewDesc;
    previewDesc.enableDepthTest = true;
    previewDesc.vertexLayout    = VertexLayout::PBR;
    if (!_meshPreviewPipeline->Initialize(_device, L"mesh_preview.vert.hlsl", L"mesh_preview.frag.hlsl", previewDesc))
    {
        LUNA_LOG_ERROR("Failed to initialize mesh preview pipeline");
        return false;
    }

    // Full PBR pipeline — Cook-Torrance BRDF, 3 CBVs + SRV table (t0-t3) + static sampler
    _pbrPipeline = std::make_unique<DX12Pipeline>();
    PipelineStateDesc pbrDesc;
    pbrDesc.enableDepthTest = true;
    pbrDesc.vertexLayout    = VertexLayout::PBR;
    pbrDesc.rootSigLayout   = RootSignatureLayout::PBR;
    if (!_pbrPipeline->Initialize(_device, L"pbr.vert.hlsl", L"pbr.frag.hlsl", pbrDesc))
    {
        LUNA_LOG_WARN("PBR pipeline failed to initialize — meshes will use preview shading");
        _pbrPipeline.reset(); // non-fatal: fall back to mesh-preview
    }

    // Per-frame scene constant buffer (b2 — SceneBuffer for PBR pass)
    if (!CreateSceneConstantBuffer())
    {
        LUNA_LOG_ERROR("Failed to create scene constant buffer");
        return false;
    }

    // Fallback shadow SRV — 1x1 white texture (all lit) for when DXR is off
    if (!CreateFallbackShadowSRV())
    {
        LUNA_LOG_WARN("Fallback shadow SRV creation failed — PBR shadow slot may be invalid");
        // non-fatal: PBR rendering still works, shadows just undefined
    }

    // Check DXR support (non-fatal — shadows just disabled)
    _dxrSupported = _dx12Device->SupportsDXR();
    if (_dxrSupported)
    {
        LUNA_LOG_INFO("DXR Tier 1.0+ supported — ray-traced shadows enabled");
        if (!CreateShadowUAV())
            _dxrSupported = false;  // fallback gracefully
    }
    else
    {
        LUNA_LOG_INFO("DXR not supported on this GPU — shadows disabled");
    }

    LUNA_LOG_INFO("DX12 backend initialized (frames-in-flight: %u)", FRAMES_IN_FLIGHT);
    return true;
}

// ---------------------------------------------------------------------------
// D3D12 Memory Allocator
// ---------------------------------------------------------------------------
bool DX12Backend::InitD3D12MA()
{
    D3D12MA::ALLOCATOR_DESC desc = {};
    desc.pDevice  = _device.Get();
    desc.pAdapter = _dx12Device->GetAdapter();

    HRESULT hr = D3D12MA::CreateAllocator(&desc, &_d3d12maAllocator);
    if (FAILED(hr))
    {
        LUNA_LOG_ERROR("D3D12MA::CreateAllocator failed: 0x%08lX", (unsigned long)hr);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Synchronization — ring buffer
// ---------------------------------------------------------------------------
bool DX12Backend::CreateCommandQueueAndFenceEvent()
{
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type     = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDesc.Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE;

    if (FAILED(_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&_commandQueue))))
    {
        LUNA_LOG_ERROR("Failed to create D3D12 command queue");
        return false;
    }

    // Create per-frame command allocators
    for (UINT i = 0; i < FRAMES_IN_FLIGHT; ++i)
    {
        if (FAILED(_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                   IID_PPV_ARGS(&_frames[i].cmdAllocator))))
        {
            LUNA_LOG_ERROR("Failed to create command allocator for frame %u", i);
            return false;
        }
    }

    // Single reusable command list — reset each frame with the current allocator
    if (FAILED(_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                          _frames[0].cmdAllocator.Get(), nullptr,
                                          IID_PPV_ARGS(&_commandList))))
    {
        LUNA_LOG_ERROR("Failed to create command list");
        return false;
    }
    _commandList->Close();

    if (FAILED(_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_fence))))
    {
        LUNA_LOG_ERROR("Failed to create fence");
        return false;
    }

    _fenceEvent = ::CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!_fenceEvent)
    {
        LUNA_LOG_ERROR("Failed to create fence event");
        return false;
    }

    return true;
}

// Wait until frame i's GPU work is complete
void DX12Backend::WaitForFrame(UINT i)
{
    if (_fence->GetCompletedValue() < _frames[i].fenceValue)
    {
        _fence->SetEventOnCompletion(_frames[i].fenceValue, _fenceEvent);
        WaitForSingleObject(_fenceEvent, INFINITE);
    }
}

// Wait for ALL frames (used in Shutdown and Resize)
void DX12Backend::WaitAllFrames()
{
    for (UINT i = 0; i < FRAMES_IN_FLIGHT; ++i)
        WaitForFrame(i);
}

// ---------------------------------------------------------------------------
// Swap chain & render targets
// ---------------------------------------------------------------------------
bool DX12Backend::CreateSwapChain()
{
    _swapChain.Reset();

    DXGI_SWAP_CHAIN_DESC1 scDesc = {};
    scDesc.Width       = static_cast<UINT>(_screenWidth);
    scDesc.Height      = static_cast<UINT>(_screenHeight);
    scDesc.Format      = _backBufferFormat;
    scDesc.Stereo      = FALSE;
    scDesc.SampleDesc.Count   = 1;
    scDesc.SampleDesc.Quality = 0;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.BufferCount = SWAP_CHAIN_BUFFER_COUNT;
    scDesc.Scaling     = DXGI_SCALING_STRETCH;
    scDesc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scDesc.AlphaMode   = DXGI_ALPHA_MODE_UNSPECIFIED;
    scDesc.Flags       = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fsDesc = {};
    fsDesc.RefreshRate.Numerator   = 60;
    fsDesc.RefreshRate.Denominator = 1;
    fsDesc.ScanlineOrdering        = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
    fsDesc.Scaling                 = DXGI_MODE_SCALING_UNSPECIFIED;
    fsDesc.Windowed                = TRUE;

    ComPtr<IDXGISwapChain1> tempSwapChain;
    HRESULT hr = _mdxgiFactory->CreateSwapChainForHwnd(
        _commandQueue.Get(), _mainWindow, &scDesc, &fsDesc, nullptr, &tempSwapChain);

    if (FAILED(hr))
    {
        LUNA_LOG_ERROR("CreateSwapChainForHwnd failed: 0x%08lX", (unsigned long)hr);
        return false;
    }

    hr = tempSwapChain.As(&_swapChain);
    if (FAILED(hr))
    {
        LUNA_LOG_ERROR("SwapChain1 -> SwapChain4 cast failed: 0x%08lX", (unsigned long)hr);
        return false;
    }

    // P2-05: suppress DXGI's built-in Alt+Enter fullscreen toggle — we handle resize ourselves.
    _mdxgiFactory->MakeWindowAssociation(_mainWindow, DXGI_MWA_NO_ALT_ENTER);

    return true;
}

bool DX12Backend::CreateRenderTarget()
{
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDesc.NumDescriptors = SWAP_CHAIN_BUFFER_COUNT;
    heapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    if (FAILED(_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&_rtvHeap))))
    {
        LUNA_LOG_ERROR("Failed to create RTV descriptor heap");
        return false;
    }

    UINT rtvSize = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE heapStart = _rtvHeap->GetCPUDescriptorHandleForHeapStart();

    for (UINT i = 0; i < SWAP_CHAIN_BUFFER_COUNT; ++i)
    {
        if (FAILED(_swapChain->GetBuffer(i, IID_PPV_ARGS(&_rtvBuffer[i]))))
        {
            LUNA_LOG_ERROR("Failed to get swap chain buffer %u", i);
            return false;
        }
        _rtvHandle[i].ptr = heapStart.ptr + i * rtvSize;
        _device->CreateRenderTargetView(_rtvBuffer[i].Get(), nullptr, _rtvHandle[i]);
    }
    return true;
}

bool DX12Backend::CreateImGuiRenderTarget()
{
    // Expanded to SRV_HEAP_SIZE slots: [0]=ImGui font, [1..N]=textures, [N+1..]=DXR UAV
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = SRV_HEAP_SIZE;
    heapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    if (FAILED(_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&_imGuiSrvHeap))))
    {
        LUNA_LOG_ERROR("Failed to create SRV/UAV descriptor heap");
        return false;
    }

    _srvDescriptorSize = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    _srvAllocIndex     = 1; // slot 0 is reserved for ImGui font SRV
    return true;
}

bool DX12Backend::CreateDepthBuffer()
{
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    heapDesc.NumDescriptors = 1;
    heapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    if (FAILED(_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&_dsvHeap))))
    {
        LUNA_LOG_ERROR("Failed to create DSV descriptor heap");
        return false;
    }

    D3D12_HEAP_PROPERTIES heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_RESOURCE_DESC depthDesc   = {};
    depthDesc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDesc.Width              = static_cast<UINT>(_screenWidth);
    depthDesc.Height             = static_cast<UINT>(_screenHeight);
    depthDesc.DepthOrArraySize   = 1;
    depthDesc.MipLevels          = 1;
    depthDesc.Format             = DXGI_FORMAT_D32_FLOAT;
    depthDesc.SampleDesc.Count   = 1;
    depthDesc.Flags              = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clearVal    = {};
    clearVal.Format               = DXGI_FORMAT_D32_FLOAT;
    clearVal.DepthStencil.Depth   = 1.0f;
    clearVal.DepthStencil.Stencil = 0;

    HRESULT hr = _device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &depthDesc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearVal, IID_PPV_ARGS(&_depthBuffer));

    if (FAILED(hr))
    {
        LUNA_LOG_ERROR("Failed to create depth buffer: 0x%08lX", (unsigned long)hr);
        return false;
    }

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format        = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsvDesc.Flags         = D3D12_DSV_FLAG_NONE;

    _dsvHandle = _dsvHeap->GetCPUDescriptorHandleForHeapStart();
    _device->CreateDepthStencilView(_depthBuffer.Get(), &dsvDesc, _dsvHandle);

    return true;
}

// ---------------------------------------------------------------------------
// Vertex buffer — Phase 2C: DEFAULT heap via D3D12MA staging upload
// ---------------------------------------------------------------------------
bool DX12Backend::CreateVertexBuffer()
{
    const UINT vbSize = sizeof(s_Vertices);

    // --- Allocate DEFAULT heap resource (GPU-optimal) via D3D12MA ---
    D3D12MA::ALLOCATION_DESC allocDesc = {};
    allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC bufDesc = CD3DX12_RESOURCE_DESC::Buffer(vbSize);

    HRESULT hr = _d3d12maAllocator->CreateResource(
        &allocDesc, &bufDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        &_vertexBufferAlloc,
        IID_PPV_ARGS(&_vertexBuffer));

    if (FAILED(hr))
    {
        LUNA_LOG_ERROR("D3D12MA: Failed to create DEFAULT vertex buffer: 0x%08lX", (unsigned long)hr);
        return false;
    }

    // --- Allocate UPLOAD staging buffer for the one-time transfer ---
    D3D12MA::ALLOCATION_DESC stagingAllocDesc = {};
    stagingAllocDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;

    ComPtr<ID3D12Resource>  stagingBuf;
    D3D12MA::Allocation*    stagingAlloc = nullptr;

    hr = _d3d12maAllocator->CreateResource(
        &stagingAllocDesc, &bufDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        &stagingAlloc,
        IID_PPV_ARGS(&stagingBuf));

    if (FAILED(hr))
    {
        LUNA_LOG_ERROR("D3D12MA: Failed to create staging buffer: 0x%08lX", (unsigned long)hr);
        return false;
    }

    // CPU → staging
    void* mapped = nullptr;
    D3D12_RANGE readRange = {0, 0};
    stagingBuf->Map(0, &readRange, &mapped);
    memcpy(mapped, s_Vertices, vbSize);
    stagingBuf->Unmap(0, nullptr);

    // Record upload commands using frame 0's allocator
    _frames[0].cmdAllocator->Reset();
    _commandList->Reset(_frames[0].cmdAllocator.Get(), nullptr);

    _commandList->CopyBufferRegion(_vertexBuffer.Get(), 0, stagingBuf.Get(), 0, vbSize);

    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        _vertexBuffer.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    _commandList->ResourceBarrier(1, &barrier);

    _commandList->Close();
    ID3D12CommandList* lists[] = {_commandList.Get()};
    _commandQueue->ExecuteCommandLists(_countof(lists), lists);

    // Block once at startup — acceptable initialization cost
    ++_globalFenceValue;
    _commandQueue->Signal(_fence.Get(), _globalFenceValue);
    _frames[0].fenceValue = _globalFenceValue;
    WaitForFrame(0);

    // Release staging immediately — upload is complete
    stagingBuf.Reset();
    stagingAlloc->Release();

    _vertexBufferView.BufferLocation = _vertexBuffer->GetGPUVirtualAddress();
    _vertexBufferView.SizeInBytes    = vbSize;
    _vertexBufferView.StrideInBytes  = sizeof(TriangleVertex);

    return true;
}

// ---------------------------------------------------------------------------
// MVP constant buffer — one per frame (persistent UPLOAD heap mapping)
// ---------------------------------------------------------------------------
bool DX12Backend::CreateMVPConstantBuffer()
{
    const UINT cbSize = (sizeof(MVPConstants) + 255) & ~255; // 256-byte aligned

    for (UINT i = 0; i < FRAMES_IN_FLIGHT; ++i)
    {
        D3D12_HEAP_PROPERTIES heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        D3D12_RESOURCE_DESC   desc      = CD3DX12_RESOURCE_DESC::Buffer(cbSize);

        HRESULT hr = _device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&_frames[i].mvpCB));

        if (FAILED(hr))
        {
            LUNA_LOG_ERROR("Failed to create MVP CB for frame %u: 0x%08lX", i, (unsigned long)hr);
            return false;
        }

        D3D12_RANGE range = {0, 0};
        _frames[i].mvpCB->Map(0, &range, &_frames[i].mvpCBMapped);
        _frames[i].mvpCBGPUAddr = _frames[i].mvpCB->GetGPUVirtualAddress();

        // Default: identity matrices
        MVPConstants constants = {};
        XMStoreFloat4x4(&constants.model, XMMatrixIdentity());
        XMStoreFloat4x4(&constants.view,  XMMatrixIdentity());
        XMStoreFloat4x4(&constants.proj,  XMMatrixIdentity());
        memcpy(_frames[i].mvpCBMapped, &constants, sizeof(constants));
    }
    return true;
}

// ---------------------------------------------------------------------------
// Scene constant buffer — one per frame (persistent UPLOAD heap mapping)
// Bound to root param 2 (b2) of the PBR root signature.
// ---------------------------------------------------------------------------
bool DX12Backend::CreateSceneConstantBuffer()
{
    const UINT cbSize = (sizeof(SceneConstants) + 255) & ~255; // 256-byte aligned

    for (UINT i = 0; i < FRAMES_IN_FLIGHT; ++i)
    {
        D3D12_HEAP_PROPERTIES heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        D3D12_RESOURCE_DESC   desc      = CD3DX12_RESOURCE_DESC::Buffer(cbSize);

        HRESULT hr = _device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&_frames[i].sceneCB));

        if (FAILED(hr))
        {
            LUNA_LOG_ERROR("Failed to create Scene CB for frame %u: 0x%08lX", i, (unsigned long)hr);
            return false;
        }

        D3D12_RANGE range = {0, 0};
        _frames[i].sceneCB->Map(0, &range, &_frames[i].sceneCBMapped);
        _frames[i].sceneCBGPUAddr = _frames[i].sceneCB->GetGPUVirtualAddress();

        // Default values: sun light coming from above, white 1-lux
        SceneConstants sc = {};
        XMStoreFloat4x4(&sc.invViewProj, XMMatrixIdentity());
        sc.lightDirection = {0.0f, -1.0f, 0.0f, 0.0f};
        sc.lightColor     = {1.0f,  1.0f, 1.0f, 1.0f};
        sc.viewPosition   = {0.0f,  0.0f, 0.0f, 0.0f};
        sc.viewportSize   = {static_cast<float>(_screenWidth), static_cast<float>(_screenHeight)};
        memcpy(_frames[i].sceneCBMapped, &sc, sizeof(sc));
    }
    return true;
}

// ---------------------------------------------------------------------------
// Fallback shadow texture — 1x1 R32_FLOAT = 1.0f (fully lit, no DXR shadows)
// Bound to root param 4 (t3) when DXR is unavailable.
// ---------------------------------------------------------------------------
bool DX12Backend::CreateFallbackShadowSRV()
{
    // Tex2D 1x1 R32_FLOAT, DEFAULT heap
    D3D12MA::ALLOCATION_DESC allocDesc = {};
    allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width              = 1;
    texDesc.Height             = 1;
    texDesc.DepthOrArraySize   = 1;
    texDesc.MipLevels          = 1;
    texDesc.Format             = DXGI_FORMAT_R32_FLOAT;
    texDesc.SampleDesc.Count   = 1;
    texDesc.Flags              = D3D12_RESOURCE_FLAG_NONE;

    HRESULT hr = _d3d12maAllocator->CreateResource(
        &allocDesc, &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, &_fallbackShadowAlloc, IID_PPV_ARGS(&_fallbackShadowTex));

    if (FAILED(hr))
    {
        LUNA_LOG_ERROR("Failed to create fallback shadow texture: 0x%08lX", (unsigned long)hr);
        return false;
    }

    // Upload 1.0f via a staging buffer
    const float one = 1.0f;
    D3D12_SUBRESOURCE_DATA subData = {};
    subData.pData      = &one;
    subData.RowPitch   = sizeof(float);
    subData.SlicePitch = sizeof(float);

    // Use UpdateSubresources helper via staging
    UINT64 stagingSize = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout = {};
    _device->GetCopyableFootprints(&texDesc, 0, 1, 0, &layout, nullptr, nullptr, &stagingSize);

    D3D12MA::ALLOCATION_DESC stagingAllocDesc = {};
    stagingAllocDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC stagingBufDesc = CD3DX12_RESOURCE_DESC::Buffer(stagingSize);

    ComPtr<ID3D12Resource> stagingBuf;
    D3D12MA::Allocation*   stagingAlloc = nullptr;
    hr = _d3d12maAllocator->CreateResource(
        &stagingAllocDesc, &stagingBufDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, &stagingAlloc, IID_PPV_ARGS(&stagingBuf));

    if (FAILED(hr))
    {
        LUNA_LOG_ERROR("Failed to create fallback shadow staging buffer: 0x%08lX", (unsigned long)hr);
        return false;
    }

    // Map and write 1.0f
    void* mapped = nullptr;
    D3D12_RANGE noRead = {0, 0};
    stagingBuf->Map(0, &noRead, &mapped);
    *static_cast<float*>(mapped) = 1.0f;
    stagingBuf->Unmap(0, nullptr);

    // Copy staging → texture
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource        = _fallbackShadowTex.Get();
    dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource       = stagingBuf.Get();
    src.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = layout;

    // We need a command list — reuse frame 0's allocator for this one-time upload
    _frames[0].cmdAllocator->Reset();
    _commandList->Reset(_frames[0].cmdAllocator.Get(), nullptr);

    _commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        _fallbackShadowTex.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    _commandList->ResourceBarrier(1, &barrier);
    _commandList->Close();

    ID3D12CommandList* lists[] = {_commandList.Get()};
    _commandQueue->ExecuteCommandLists(_countof(lists), lists);
    ++_globalFenceValue;
    _commandQueue->Signal(_fence.Get(), _globalFenceValue);
    _frames[0].fenceValue = _globalFenceValue;
    WaitForFrame(0);

    stagingBuf.Reset();
    stagingAlloc->Release();

    // Create SRV
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle;
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle;
    _fallbackShadowSRVIdx = AllocateSRVSlot(cpuHandle, gpuHandle);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format                  = DXGI_FORMAT_R32_FLOAT;
    srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels     = 1;
    _device->CreateShaderResourceView(_fallbackShadowTex.Get(), &srvDesc, cpuHandle);

    LUNA_LOG_INFO("Fallback shadow SRV created at slot %u", _fallbackShadowSRVIdx);
    return true;
}

// ---------------------------------------------------------------------------
// DXR shadow UAV — R32_FLOAT texture, viewport-sized
// ---------------------------------------------------------------------------
bool DX12Backend::CreateShadowUAV()
{
    if (_shadowUAVAlloc) { _shadowUAVAlloc->Release(); _shadowUAVAlloc = nullptr; }
    _shadowUAV.Reset();

    D3D12MA::ALLOCATION_DESC allocDesc = {};
    allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width              = static_cast<UINT>(_screenWidth);
    texDesc.Height             = static_cast<UINT>(_screenHeight);
    texDesc.DepthOrArraySize   = 1;
    texDesc.MipLevels          = 1;
    texDesc.Format             = DXGI_FORMAT_R32_FLOAT;
    texDesc.SampleDesc.Count   = 1;
    texDesc.Flags              = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    HRESULT hr = _d3d12maAllocator->CreateResource(
        &allocDesc, &texDesc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr,
        &_shadowUAVAlloc,
        IID_PPV_ARGS(&_shadowUAV));

    if (FAILED(hr))
    {
        LUNA_LOG_ERROR("Failed to create shadow UAV texture: 0x%08lX", (unsigned long)hr);
        return false;
    }

    // New texture always starts in UAV state — ensure the cross-frame flag is clear.
    _shadowInSRVState = false;

    // Allocate a SRV/UAV heap slot — reuse existing slot on resize
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle;
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle;
    if (_shadowUAVSRVIndex == 0)
        _shadowUAVSRVIndex = AllocateSRVSlot(cpuHandle, gpuHandle);
    else
    {
        cpuHandle.ptr = _imGuiSrvHeap->GetCPUDescriptorHandleForHeapStart().ptr
                        + _shadowUAVSRVIndex * _srvDescriptorSize;
        gpuHandle.ptr = _imGuiSrvHeap->GetGPUDescriptorHandleForHeapStart().ptr
                        + _shadowUAVSRVIndex * _srvDescriptorSize;
    }

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format        = DXGI_FORMAT_R32_FLOAT;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    _device->CreateUnorderedAccessView(_shadowUAV.Get(), nullptr, &uavDesc, cpuHandle);
    (void)gpuHandle; // stored by index; retrieved on demand

    return true;
}

// ---------------------------------------------------------------------------
// SRV slot allocator
// ---------------------------------------------------------------------------
UINT DX12Backend::AllocateSRVSlot(D3D12_CPU_DESCRIPTOR_HANDLE& outCPU,
                                   D3D12_GPU_DESCRIPTOR_HANDLE& outGPU)
{
    UINT index = _srvAllocIndex++;
    outCPU.ptr = _imGuiSrvHeap->GetCPUDescriptorHandleForHeapStart().ptr
                 + index * _srvDescriptorSize;
    outGPU.ptr = _imGuiSrvHeap->GetGPUDescriptorHandleForHeapStart().ptr
                 + index * _srvDescriptorSize;
    return index;
}

// ---------------------------------------------------------------------------
// Frame loop — frames-in-flight ring buffer
// ---------------------------------------------------------------------------
void DX12Backend::BeginFrame()
{
    // Wait until the GPU has finished with this frame slot's resources
    WaitForFrame(_frameIndex);

    FrameResource& fr = _frames[_frameIndex];
    fr.cmdAllocator->Reset();
    _commandList->Reset(fr.cmdAllocator.Get(), nullptr);

    UINT backIdx = _swapChain->GetCurrentBackBufferIndex();
    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        _rtvBuffer[backIdx].Get(),
        D3D12_RESOURCE_STATE_PRESENT,
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    _commandList->ResourceBarrier(1, &barrier);

    _commandList->RSSetViewports(1, &_screenViewport);
    _commandList->RSSetScissorRects(1, &_scissorRect);

    const FLOAT clearColor[] = {0.1f, 0.1f, 0.1f, 1.0f};
    _commandList->ClearRenderTargetView(_rtvHandle[backIdx], clearColor, 0, nullptr);
    _commandList->ClearDepthStencilView(_dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    _commandList->OMSetRenderTargets(1, &_rtvHandle[backIdx], FALSE, &_dsvHandle);
}

void DX12Backend::DrawFrame()
{
    // -----------------------------------------------------------------------
    // Phase 6: Render Graph — rebuilt every frame.
    // -----------------------------------------------------------------------
    _renderGraph.Reset();

    // Restore shadow texture from PSR→UAV if the previous frame left it exposed.
    // Must happen before the new DXR dispatch which writes to the UAV.
    if (_shadowInSRVState && _shadowUAV)
    {
        D3D12_RESOURCE_BARRIER toUAV = CD3DX12_RESOURCE_BARRIER::Transition(
            _shadowUAV.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        _commandList->ResourceBarrier(1, &toUAV);
        _shadowInSRVState = false;
    }

    // -----------------------------------------------------------------------
    // DXR Shadow pass — skipped when hardware ray-tracing is unavailable.
    // -----------------------------------------------------------------------
    if (_dxrSupported && _rtPipeline && _accelStructure)
    {
        RGResourceHandle hDepth  = _renderGraph.ImportResource(
            _depthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
        RGResourceHandle hShadow = _renderGraph.ImportResource(
            _shadowUAV.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        // Pass 1: transition depth to NON_PIXEL_SHADER_RESOURCE and dispatch rays.
        // Shadow UAV stays as UAV — the ray-gen shader writes to it directly.
        _renderGraph.AddPass("DXR Shadows",
            {
                {hDepth,  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
                {hShadow, D3D12_RESOURCE_STATE_UNORDERED_ACCESS},
            },
            [this](ID3D12GraphicsCommandList* /*cmd*/)
            {
                ComPtr<ID3D12GraphicsCommandList4> cmdList4;
                _commandList.As(&cmdList4);
                if (cmdList4)
                    _rtPipeline->DispatchShadows(cmdList4.Get(),
                                                 _accelStructure->GetTLASAddress(),
                                                 _shadowUAV.Get(),
                                                 _depthBuffer.Get(),
                                                 static_cast<UINT>(_screenWidth),
                                                 static_cast<UINT>(_screenHeight),
                                                 _frames[_frameIndex].mvpCBGPUAddr);
            });

        // Pass 2 (barrier-only): restore depth → DEPTH_WRITE, expose shadow → PSR.
        // No execute body — the graph emits the barriers and continues.
        _renderGraph.AddPass("Expose Shadow SRV",
            {
                {hDepth,  D3D12_RESOURCE_STATE_DEPTH_WRITE},
                {hShadow, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE},
            },
            nullptr);

        _renderGraph.Execute(_commandList.Get());

        // Shadow remains in PIXEL_SHADER_RESOURCE state for DrawMesh() calls
        // that follow in SceneManager::Update(). Restored at the top of the
        // next DrawFrame() via the _shadowInSRVState flag.
        _shadowInSRVState = true;
    }

    // -----------------------------------------------------------------------
    // Rasterization pass — triangle fallback only when no scene meshes loaded.
    // Scene meshes are drawn by MeshRenderer::Render() via DrawMesh(), which is
    // called from SceneManager::Update() in Application::Run() after DrawFrame().
    // -----------------------------------------------------------------------
    if (_sceneMeshes.empty())
    {
        BindPipeline(_cbvPipeline.get());
        SetPipelineState(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, &_screenViewport, &_scissorRect);
        _commandList->IASetVertexBuffers(0, 1, &_vertexBufferView);
        _commandList->SetGraphicsRootConstantBufferView(0, _frames[_frameIndex].mvpCBGPUAddr);
        Draw(3);
    }
}

void DX12Backend::EndFrame()
{
    UINT backIdx = _swapChain->GetCurrentBackBufferIndex();
    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        _rtvBuffer[backIdx].Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PRESENT);
    _commandList->ResourceBarrier(1, &barrier);
    _commandList->Close();

    ID3D12CommandList* lists[] = {_commandList.Get()};
    _commandQueue->ExecuteCommandLists(_countof(lists), lists);
    // P2-04: interval=1 → vsync on (wait for one VBlank), interval=0 → tearing/uncapped
    _swapChain->Present(_vsync ? 1u : 0u, 0);

    // Signal the fence for the frame we just submitted
    ++_globalFenceValue;
    _commandQueue->Signal(_fence.Get(), _globalFenceValue);
    _frames[_frameIndex].fenceValue = _globalFenceValue;

    // Advance ring buffer index
    _frameIndex = (_frameIndex + 1) % FRAMES_IN_FLIGHT;
}

void DX12Backend::Resize(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0) return;

    WaitAllFrames();
    SetResolution(width, height);

    _screenViewport = {0.0f, 0.0f, static_cast<FLOAT>(width), static_cast<FLOAT>(height), 0.0f, 1.0f};
    _scissorRect    = CD3DX12_RECT(0, 0, static_cast<LONG>(width), static_cast<LONG>(height));

    for (UINT i = 0; i < SWAP_CHAIN_BUFFER_COUNT; ++i)
        _rtvBuffer[i].Reset();
    _depthBuffer.Reset();

    _swapChain->ResizeBuffers(SWAP_CHAIN_BUFFER_COUNT, width, height, _backBufferFormat, 0);
    CreateRenderTarget();
    CreateDepthBuffer();

    // Resize shadow UAV if DXR is active
    if (_dxrSupported)
        CreateShadowUAV();
}

// ---------------------------------------------------------------------------
// MVP update — per-frame, safe because ring buffer guarantees no GPU overlap
// ---------------------------------------------------------------------------
void DX12Backend::UpdateMVP(const XMFLOAT4X4& model, const XMFLOAT4X4& view,
                            const XMFLOAT4X4& proj)
{
    // Cache camera matrices so DrawMesh() can rebuild MVP per object
    _lastView = view;
    _lastProj = proj;

    MVPConstants mvp;
    mvp.model = model;
    mvp.view  = view;
    mvp.proj  = proj;
    memcpy(_frames[_frameIndex].mvpCBMapped, &mvp, sizeof(MVPConstants));

    // Update scene CB — recompute invViewProj and extract camera world position from view matrix
    if (_frames[_frameIndex].sceneCBMapped)
    {
        SceneConstants sc = {};

        XMMATRIX mView = XMLoadFloat4x4(&view);
        XMMATRIX mProj = XMLoadFloat4x4(&proj);
        XMMATRIX mVP   = mView * mProj;
        XMStoreFloat4x4(&sc.invViewProj, XMMatrixInverse(nullptr, mVP));

        // Camera world position is the last column of the inverse view matrix
        XMMATRIX mViewInv = XMMatrixInverse(nullptr, mView);
        sc.viewPosition = {mViewInv.r[3].m128_f32[0],
                           mViewInv.r[3].m128_f32[1],
                           mViewInv.r[3].m128_f32[2], 0.0f};

        // Light / viewport stay at previously-set values — read back to preserve them
        SceneConstants* prev = static_cast<SceneConstants*>(_frames[_frameIndex].sceneCBMapped);
        sc.lightDirection = prev->lightDirection;
        sc.lightColor     = prev->lightColor;
        sc.viewportSize   = {static_cast<float>(_screenWidth), static_cast<float>(_screenHeight)};

        memcpy(_frames[_frameIndex].sceneCBMapped, &sc, sizeof(SceneConstants));
    }
}

// ---------------------------------------------------------------------------
// Scene mesh loading — one-shot GPU upload, blocks until complete
// ---------------------------------------------------------------------------
std::vector<std::shared_ptr<Mesh>> DX12Backend::LoadMeshes(const std::string& path)
{
    WaitAllFrames();

    // Open a fresh command list using frame-0's allocator
    _frames[0].cmdAllocator->Reset();
    _commandList->Reset(_frames[0].cmdAllocator.Get(), nullptr);

    auto rawMeshes = MeshLoader::LoadGLTF(path, _device.Get(), _d3d12maAllocator, _commandList.Get(),
                                           _imGuiSrvHeap.Get(), _srvDescriptorSize, &_srvAllocIndex);

    _commandList->Close();
    ID3D12CommandList* lists[] = {_commandList.Get()};
    _commandQueue->ExecuteCommandLists(_countof(lists), lists);

    // Block until upload is complete (startup cost — acceptable)
    ++_globalFenceValue;
    _commandQueue->Signal(_fence.Get(), _globalFenceValue);
    _frames[0].fenceValue = _globalFenceValue;
    WaitForFrame(0);

    // Convert unique_ptr → shared_ptr and store for lifetime management
    _sceneMeshes.clear();
    _sceneMeshes.reserve(rawMeshes.size());
    for (auto& m : rawMeshes)
        _sceneMeshes.push_back(std::shared_ptr<Mesh>(std::move(m)));

    LUNA_LOG_INFO("Loaded %zu mesh(es) from %s", _sceneMeshes.size(), path.c_str());
    return _sceneMeshes;  // caller gets shared_ptrs to distribute to MeshRenderers
}

// ---------------------------------------------------------------------------
// Per-object mesh draw — must be called between BeginFrame() and EndFrame()
// ---------------------------------------------------------------------------
void DX12Backend::DrawMesh(const Mesh* mesh, const XMFLOAT4X4& model)
{
    if (!mesh) return;

    // Update MVP CB with this object's model matrix + cached camera matrices
    MVPConstants mvp;
    mvp.model = model;
    mvp.view  = _lastView;
    mvp.proj  = _lastProj;
    memcpy(_frames[_frameIndex].mvpCBMapped, &mvp, sizeof(MVPConstants));

    const bool usePBR = (mesh->material != nullptr) && (_pbrPipeline != nullptr);

    if (usePBR)
    {
        // -----------------------------------------------------------------
        // PBR path: bind _pbrPipeline, material CB (b1), scene CB (b2),
        // and the per-material SRV descriptor table (t0-t3).
        // Root parameter layout (matches PBR root signature):
        //   [0] CBV b0 — MVP
        //   [1] CBV b1 — MaterialBuffer
        //   [2] CBV b2 — SceneBuffer
        //   [3] Descriptor table — albedo(t0), normal(t1), metalRough(t2), shadow(t3)
        // -----------------------------------------------------------------
        BindPipeline(_pbrPipeline.get());
        SetPipelineState(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, &_screenViewport, &_scissorRect);

        _commandList->IASetVertexBuffers(0, 1, &mesh->vbView);
        _commandList->IASetIndexBuffer(&mesh->ibView);

        // Bind the shader-visible SRV heap (required for descriptor tables)
        ID3D12DescriptorHeap* heaps[] = {_imGuiSrvHeap.Get()};
        _commandList->SetDescriptorHeaps(_countof(heaps), heaps);

        // b0 — MVP
        _commandList->SetGraphicsRootConstantBufferView(0, _frames[_frameIndex].mvpCBGPUAddr);
        // b1 — Material constants
        _commandList->SetGraphicsRootConstantBufferView(1, mesh->material->cbGPUAddr);
        // b2 — Scene constants
        _commandList->SetGraphicsRootConstantBufferView(2, _frames[_frameIndex].sceneCBGPUAddr);

        // [3] Material SRV descriptor table — albedo(t0), normal(t1), metalRough(t2)
        D3D12_GPU_DESCRIPTOR_HANDLE matSrvTable;
        matSrvTable.ptr = _imGuiSrvHeap->GetGPUDescriptorHandleForHeapStart().ptr
                          + static_cast<UINT64>(mesh->material->srvHeapStartIndex)
                          * _srvDescriptorSize;
        _commandList->SetGraphicsRootDescriptorTable(3, matSrvTable);

        // [4] Shadow SRV (t3) — use DXR shadow if available, otherwise fallback (1x1 white = fully lit)
        UINT shadowIdx = (_dxrSupported && _shadowUAVSRVIndex > 0)
                         ? _shadowUAVSRVIndex
                         : _fallbackShadowSRVIdx;
        D3D12_GPU_DESCRIPTOR_HANDLE shadowSrvTable;
        shadowSrvTable.ptr = _imGuiSrvHeap->GetGPUDescriptorHandleForHeapStart().ptr
                             + static_cast<UINT64>(shadowIdx) * _srvDescriptorSize;
        _commandList->SetGraphicsRootDescriptorTable(4, shadowSrvTable);

        _commandList->DrawIndexedInstanced(mesh->indexCount, 1, 0, 0, 0);
    }
    else
    {
        // -----------------------------------------------------------------
        // Fallback path: mesh-preview pipeline (normal-diffuse shading).
        // Only root param 0 (b0 MVP) is bound.
        // -----------------------------------------------------------------
        if (!_meshPreviewPipeline) return;

        BindPipeline(_meshPreviewPipeline.get());
        SetPipelineState(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, &_screenViewport, &_scissorRect);
        _commandList->IASetVertexBuffers(0, 1, &mesh->vbView);
        _commandList->IASetIndexBuffer(&mesh->ibView);
        _commandList->SetGraphicsRootConstantBufferView(0, _frames[_frameIndex].mvpCBGPUAddr);
        _commandList->DrawIndexedInstanced(mesh->indexCount, 1, 0, 0, 0);
    }
}

// ---------------------------------------------------------------------------
// ImGui
// ---------------------------------------------------------------------------
void DX12Backend::InitImGui(void *windowHandler)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    ImGui::StyleColorsDark();

    auto *window = static_cast<GLFWwindow *>(windowHandler);
    ImGui_ImplGlfw_InitForOther(window, true);

    // ImGui uses slot 0 of the expanded SRV heap
    if (!ImGui_ImplDX12_Init(_device.Get(), SWAP_CHAIN_BUFFER_COUNT, _backBufferFormat,
                              _imGuiSrvHeap.Get(),
                              _imGuiSrvHeap->GetCPUDescriptorHandleForHeapStart(),
                              _imGuiSrvHeap->GetGPUDescriptorHandleForHeapStart()))
    {
        LUNA_LOG_ERROR("ImGui_ImplDX12_Init failed");
        return;
    }
    ImGui_ImplDX12_CreateDeviceObjects();
}

void DX12Backend::StartImGui()
{
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void DX12Backend::RenderImGui()
{
    ImGui::Render();

#if defined(_DEBUG)
    if (!CheckIfImGuiData()) return;
#endif

    ID3D12DescriptorHeap *heaps[] = {_imGuiSrvHeap.Get()};
    _commandList->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), _commandList.Get());

    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }
}

void DX12Backend::ShutdownImGui()
{
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

// ---------------------------------------------------------------------------
// Draw calls
// ---------------------------------------------------------------------------
void DX12Backend::Draw(uint32_t vertexCount)
{
    _commandList->DrawInstanced(vertexCount, 1, 0, 0);
}

void DX12Backend::SetVertexBuffer(IBuffer *buffer)
{
    auto *dxBuffer = static_cast<DX12Buffer *>(buffer);
    _commandList->IASetVertexBuffers(0, 1, dxBuffer->GetVBView());
}

void DX12Backend::BindPipeline(IPipeline *pipeline)
{
    auto *dx = static_cast<DX12Pipeline *>(pipeline);
    _commandList->SetGraphicsRootSignature(dx->GetRootSignature().Get());
    _commandList->SetPipelineState(dx->GetPipelineState().Get());
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
bool DX12Backend::CheckIfImGuiData()
{
    auto *drawData = ImGui::GetDrawData();
    if (!drawData)
    {
        LUNA_LOG_WARN("[ImGui] DrawData is null — nothing to render");
        return false;
    }
    return true;
}

void DX12Backend::SetResolution(const uint32_t &width, const uint32_t &height)
{
    _screenWidth  = static_cast<int>(width);
    _screenHeight = static_cast<int>(height);
}

void DX12Backend::SetPipelineState(D3D12_PRIMITIVE_TOPOLOGY topology,
                                    const D3D12_VIEWPORT *viewport,
                                    const D3D12_RECT *scissorRect)
{
    if (viewport)    _commandList->RSSetViewports(1, viewport);
    if (scissorRect) _commandList->RSSetScissorRects(1, scissorRect);
    _commandList->IASetPrimitiveTopology(topology);
}

D3D12_CPU_DESCRIPTOR_HANDLE DX12Backend::GetBackBufferView()
{
    return _rtvHandle[_swapChain->GetCurrentBackBufferIndex()];
}

ComPtr<ID3D12Resource> DX12Backend::GetCurrentBackBufferResource() const
{
    return _rtvBuffer[_swapChain->GetCurrentBackBufferIndex()];
}

} // namespace Luna
