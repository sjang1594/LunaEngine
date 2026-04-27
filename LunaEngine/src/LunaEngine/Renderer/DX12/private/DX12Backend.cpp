#include "LunaPCH.h"
#include "stb_image.h"
#include "LunaEngine/Graphics/IBuffer.h"
#include "LunaEngine/Graphics/Material.h"
#include "LunaEngine/Renderer/DX12/Public/DX12Backend.h"
#include "LunaEngine/Renderer/DX12/Public/DX12Pipeline.h"
#include "LunaEngine/Renderer/DX12/Public/DX12Device.h"
#include "LunaEngine/Renderer/DX12/Public/DX12AccelStructure.h"
#include "LunaEngine/Renderer/DX12/Public/DX12RTPipeline.h"
#include "Renderer/DX12/Public/DX12Buffer.h"
#include "Renderer/MeshLoader.h"
#include "Renderer/Meshlet.h"
#include "Renderer/RenderGraph.h"
#include "Graphics/Texture.h"
#include <directx/d3dx12_resource_helpers.h>  // GetRequiredIntermediateSize, UpdateSubresources
#include <random>                               // std::mt19937, std::uniform_real_distribution

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

// Phase 7+8: Scene-level constants for the deferred lighting pass.
// Must match cbuffer SceneConstants in deferred_lighting.frag.hlsl.
struct SceneConstants
{
    XMFLOAT4X4 invViewProj;  //  64 B — inverse(view * proj), row-major
    XMFLOAT3   eyePosition;  //  12 B
    float      _padEye;      //   4 B
    XMFLOAT3   lightDir;     //  12 B — toward-light, normalised
    float      _padLight;    //   4 B
    XMFLOAT4   lightColor;   //  16 B — xyz=color, w=intensity
    // Phase 8: CSM
    XMFLOAT4X4 viewMatrix;   //  64 B — camera view matrix (for view-space depth)
    XMFLOAT4X4 lightVP[4];   // 256 B — light-space VP per cascade, row-major
    XMFLOAT4   cascadeSplits;//  16 B — view-space Z far plane per cascade
    // Phase 24: Clustered lighting
    uint32_t   numPointLights; //  4 B
    uint32_t   _pad2[3];       // 12 B
};  // 464 B → round up to 512-byte aligned CB

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
// Constructor defined here (not in .h) so MSVC can see the complete types of
// DX12AccelStructure and DX12RTPipeline when it instantiates the unique_ptr
// members' destructors for exception-cleanup paths.
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

    // Phase 22: Shutdown GPU profiler
    _gpuProfiler.Shutdown();

    // Phase 13: clean up async compute
    if (_computeFenceEvent)
    {
        CloseHandle(_computeFenceEvent);
        _computeFenceEvent = nullptr;
    }
    _asyncComputeReady = false;

    // Phase 12: release GPU-driven rendering resources before D3D12MA allocator
    DestroyIndirectResources();
    // Phase 25: release mesh shader resources
    DestroyMeshShaderResources();
    // Phase 24: release clustered lighting resources
    DestroyClusteredLightingResources();
    // Phase 23: release Hi-Z resources
    DestroyHiZResources();
    // Phase 14: release IBL resources before D3D12MA allocator
    DestroyIBLResources();
    // Phase 9: release SSAO resources before CSM / G-buffer (all before D3D12MA allocator)
    DestroyPostProcessResources();  // Phase 10: must be before D3D12MA allocator
    DestroySSAOResources();
    // Phase 8: release CSM resources before G-buffer (both before D3D12MA allocator)
    DestroyCSMResources();
    // Phase 7: release G-buffer before D3D12MA allocator is destroyed
    DestroyGBuffer();

    // Release D3D12MA allocations before destroying the allocator.
    // Scene meshes must be released first (their Allocation::Release() needs the allocator alive).
    _sceneMeshes.clear();

    if (_vertexBufferAlloc) { _vertexBufferAlloc->Release(); _vertexBufferAlloc = nullptr; }
    if (_shadowUAVAlloc)    { _shadowUAVAlloc->Release();    _shadowUAVAlloc    = nullptr; }

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
    if (!CreateSceneCBs())                     return false;

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

    // Mesh preview pipeline — PBRVertex layout, normal-diffuse shading (MVP-only root sig)
    _meshPreviewPipeline = std::make_unique<DX12Pipeline>();
    PipelineStateDesc previewDesc;
    previewDesc.enableDepthTest = true;
    previewDesc.vertexLayout    = VertexLayout::PBR;
    previewDesc.rootLayout      = RootSignatureLayout::MVP;
    if (!_meshPreviewPipeline->Initialize(_device, L"mesh_preview.vert.hlsl", L"mesh_preview.frag.hlsl", previewDesc))
    {
        LUNA_LOG_ERROR("Failed to initialize mesh preview pipeline");
        return false;
    }

    // Check DXR support (non-fatal — shadows just disabled)
    _dxrSupported = _dx12Device->SupportsDXR();
    if (_dxrSupported)
    {
        LUNA_LOG_INFO("DXR Tier 1.0+ supported -- ray-traced shadows enabled");
        if (!CreateShadowUAV())
            _dxrSupported = false;  // fallback gracefully
    }
    else
    {
        LUNA_LOG_INFO("DXR not supported on this GPU — shadows disabled");
    }

    // Phase 25: Check mesh shader support
    _meshShadersSupported = _dx12Device->SupportsMeshShaders();
    if (_meshShadersSupported)
        LUNA_LOG_INFO("Phase 25: Mesh Shader Tier 1 supported");
    else
        LUNA_LOG_INFO("Phase 25: Mesh shaders not supported — using indirect draw fallback");
    // Phase 7: G-buffer pass — pbr.vert writes geometry into 3 MRTs
    if (!CreateGBuffer()) return false;

    // Phase 7: G-buffer fill pipeline (PBR vertex + G-buffer fill pixel shader, 3 MRTs)
    _gbufferPipeline = std::make_unique<DX12Pipeline>();
    {
        PipelineStateDesc gbufDesc;
        gbufDesc.enableDepthTest  = true;
        gbufDesc.vertexLayout     = VertexLayout::PBR;
        gbufDesc.rootLayout       = RootSignatureLayout::PBR;
        gbufDesc.numRenderTargets = 3;
        gbufDesc.rtvFormats[0]    = DXGI_FORMAT_R8G8B8A8_UNORM;
        gbufDesc.rtvFormats[1]    = DXGI_FORMAT_R16G16B16A16_FLOAT;
        gbufDesc.rtvFormats[2]    = DXGI_FORMAT_R8G8B8A8_UNORM;
        if (!_gbufferPipeline->Initialize(_device, L"pbr.vert.hlsl", L"gbuffer.frag.hlsl", gbufDesc))
        {
            LUNA_LOG_WARN("Failed to initialize G-buffer pipeline — will use mesh_preview fallback");
            OutputDebugStringA("[LUNA] G-buffer pipeline init FAILED\n");
            _gbufferPipeline.reset();  // non-fatal
        }
        else
        {
            OutputDebugStringA("[LUNA] G-buffer pipeline init SUCCESS\n");
        }
    }

    // Phase 7: Deferred lighting pipeline (fullscreen triangle, no VB)
    _lightingPipeline = std::make_unique<DX12Pipeline>();
    {
        PipelineStateDesc lightDesc;
        lightDesc.rootLayout       = RootSignatureLayout::DeferredLighting;
        lightDesc.noInputLayout    = true;
        lightDesc.numRenderTargets = 1;
        lightDesc.rtvFormats[0]    = DXGI_FORMAT_R8G8B8A8_UNORM;
        // Prefer Slang for [Differentiable] CookTorrance; fall back to HLSL if Slang fails.
        bool lightOk = _lightingPipeline->Initialize(_device, L"fullscreen.vert.hlsl", L"deferred_lighting.slang", lightDesc);
        if (!lightOk)
        {
            LUNA_LOG_WARN("Slang deferred_lighting failed — retrying with HLSL");
            _lightingPipeline = std::make_unique<DX12Pipeline>();
            lightOk = _lightingPipeline->Initialize(_device, L"fullscreen.vert.hlsl", L"deferred_lighting.frag.hlsl", lightDesc);
        }
        if (!lightOk)
        {
            LUNA_LOG_ERROR("Failed to initialize deferred lighting pipeline");
            _lightingPipeline.reset();  // non-fatal — IBL/HDR pipelines will handle it
        }
        else
        {
            OutputDebugStringA("[LUNA] Lighting pipeline init SUCCESS\n");
        }
    }

    // Phase 8: CSM shadow map array + depth-only pipeline
    if (!CreateCSMResources())
    {
        LUNA_LOG_WARN("Failed to create CSM resources — directional shadows disabled");
    }
    else
    {
        _csmPipeline = std::make_unique<DX12Pipeline>();
        PipelineStateDesc csmDesc;
        csmDesc.vertexLayout  = VertexLayout::PBR;
        csmDesc.rootLayout    = RootSignatureLayout::CSMDepth;
        csmDesc.depthOnlyPass = true;
        csmDesc.dsvFormat     = DXGI_FORMAT_D32_FLOAT;
        if (!_csmPipeline->Initialize(_device, L"csm_depth.vert.hlsl", L"", csmDesc))
        {
            LUNA_LOG_WARN("Failed to initialize CSM pipeline — shadows will use ambient only");
            _csmPipeline.reset();
        }
    }

    // Phase 9: SSAO
    if (!CreateSSAOResources())
        LUNA_LOG_WARN("SSAO init failed — ambient occlusion disabled");

    // Phase 10: post-process stack (TAA + Bloom + ACES tone mapping)
    if (!CreatePostProcessResources())
        LUNA_LOG_WARN("Post-process stack init failed — Phase 9 LDR path active");
    XMStoreFloat4x4(&_ppPrevVP, XMMatrixIdentity());  // safe initial value for TAA

    // Phase 22: GPU profiler
    if (!_gpuProfiler.Init(_device.Get(), _commandQueue.Get()))
        LUNA_LOG_WARN("GPU profiler init failed — timing disabled");

    // Phase 24: Clustered lighting resources
    if (!CreateClusteredLightingResources())
        LUNA_LOG_WARN("Clustered lighting init failed — point lights disabled");

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

    // Phase 13: Async compute queue + per-frame compute allocators
    {
        D3D12_COMMAND_QUEUE_DESC compDesc = {};
        compDesc.Type     = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        compDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        compDesc.Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE;

        if (FAILED(_device->CreateCommandQueue(&compDesc, IID_PPV_ARGS(&_computeQueue))))
        {
            LUNA_LOG_WARN("Phase 13: Failed to create compute queue — async compute disabled");
        }
        else
        {
            bool ok = true;
            for (UINT i = 0; i < FRAMES_IN_FLIGHT; ++i)
            {
                if (FAILED(_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE,
                                                           IID_PPV_ARGS(&_frames[i].computeCmdAllocator))))
                {
                    ok = false; break;
                }
            }
            if (ok && SUCCEEDED(_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE,
                                                           _frames[0].computeCmdAllocator.Get(), nullptr,
                                                           IID_PPV_ARGS(&_computeCommandList))))
            {
                _computeCommandList->Close();
                if (SUCCEEDED(_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_computeFence))))
                {
                    _computeFenceEvent = ::CreateEvent(nullptr, FALSE, FALSE, nullptr);
                    _asyncComputeReady = (_computeFenceEvent != nullptr);
                }
            }
            if (_asyncComputeReady)
                LUNA_LOG_INFO("Phase 13: Async compute queue created");
            else
                LUNA_LOG_WARN("Phase 13: Async compute init failed — fallback to graphics queue");
        }

        // Phase 23/Bug #010: Disable async compute — the current implementation uses
        // _commandQueue->Wait() which is a queue-level wait that blocks the ENTIRE next
        // ExecuteCommandLists call. This means CSM/RT shadows can't overlap with the compute
        // cull at all (zero GPU overlap), while introducing cross-queue race conditions on
        // the Hi-Z texture and shared _objectDataBuffer. Force the single-queue fallback path
        // which is race-free and produces identical GPU behavior.
        // TODO: Redesign async compute to submit cull as a separate command list before the
        //       main frame's command list for true overlap.
        if (_asyncComputeReady)
        {
            LUNA_LOG_INFO("Phase 13: Async compute disabled (Bug #010 — queue-level Wait prevents overlap; use single-queue path)");
            _asyncComputeReady = false;
        }
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

// Phase 13: wait for async compute frame
void DX12Backend::WaitForComputeFrame(UINT i)
{
    if (!_asyncComputeReady) return;
    if (_computeFence->GetCompletedValue() < _frames[i].computeFenceValue)
    {
        _computeFence->SetEventOnCompletion(_frames[i].computeFenceValue, _computeFenceEvent);
        WaitForSingleObject(_computeFenceEvent, INFINITE);
    }
}

// Wait for ALL frames (used in Shutdown and Resize)
void DX12Backend::WaitAllFrames()
{
    for (UINT i = 0; i < FRAMES_IN_FLIGHT; ++i)
    {
        WaitForFrame(i);
        WaitForComputeFrame(i);
    }
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

    // P2-05: Disable DXGI's built-in ALT+ENTER fullscreen handling.
    // Without this, DXGI intercepts ALT+ENTER and enters a borderless-fullscreen mode that
    // conflicts with GLFW window management (GLFW won't receive the key event).
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
    _srvAllocIndex     = 0; // imgui allocates its own slots via SrvDescriptorAllocFn callbacks
    return true;
}

bool DX12Backend::CreateDepthBuffer()
{
    // Create DSV heap on first call only (not on resize)
    if (!_dsvHeap)
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
    }

    // Phase 7: Use R32_TYPELESS so we can create both a DSV (D32_FLOAT) and an SRV (R32_FLOAT)
    // on the same resource for depth reconstruction in the lighting pass.
    D3D12_HEAP_PROPERTIES heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_RESOURCE_DESC depthDesc   = {};
    depthDesc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDesc.Width              = static_cast<UINT>(_screenWidth);
    depthDesc.Height             = static_cast<UINT>(_screenHeight);
    depthDesc.DepthOrArraySize   = 1;
    depthDesc.MipLevels          = 1;
    depthDesc.Format             = DXGI_FORMAT_R32_TYPELESS;  // typeless → dual-purpose DSV+SRV
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

    // DSV: interprets typeless as D32_FLOAT (no functional change to rasterisation)
    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format        = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsvDesc.Flags         = D3D12_DSV_FLAG_NONE;

    _dsvHandle = _dsvHeap->GetCPUDescriptorHandleForHeapStart();
    _device->CreateDepthStencilView(_depthBuffer.Get(), &dsvDesc, _dsvHandle);

    // NOTE: Depth SRV (R32_FLOAT) is created in CreateGBuffer() so that all 5 lighting
    // pass SRV slots (GB0, GB1, GB2, depth, shadow) are allocated consecutively and the
    // descriptor table can be set with a single base pointer.

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
// Phase 7: Scene constants CB — per-frame, persistently mapped UPLOAD heap
// ---------------------------------------------------------------------------
bool DX12Backend::CreateSceneCBs()
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
    }
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
// Phase 7: G-buffer creation — 3 off-screen render targets + SRV heap slots
// ---------------------------------------------------------------------------
bool DX12Backend::CreateGBuffer()
{
    static const DXGI_FORMAT GBUFFER_FORMATS[GBUFFER_COUNT] = {
        DXGI_FORMAT_R8G8B8A8_UNORM,      // GB0: albedo
        DXGI_FORMAT_R16G16B16A16_FLOAT,  // GB1: world-space normal
        DXGI_FORMAT_R8G8B8A8_UNORM,      // GB2: metallic + roughness
    };

    // Create separate RTV heap on first call
    if (!_gbufferRtvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
        rtvHeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.NumDescriptors = GBUFFER_COUNT;
        rtvHeapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

        if (FAILED(_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&_gbufferRtvHeap))))
        {
            LUNA_LOG_ERROR("Failed to create G-buffer RTV heap");
            return false;
        }
    }

    UINT rtvSize = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    for (UINT i = 0; i < GBUFFER_COUNT; ++i)
    {
        // Release previous texture on resize
        if (_gbuffer[i])   { _gbuffer[i].Reset(); }
        if (_gbufferAlloc[i]) { _gbufferAlloc[i]->Release(); _gbufferAlloc[i] = nullptr; }

        D3D12MA::ALLOCATION_DESC allocDesc = {};
        allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC texDesc = {};
        texDesc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width              = static_cast<UINT>(_screenWidth);
        texDesc.Height             = static_cast<UINT>(_screenHeight);
        texDesc.DepthOrArraySize   = 1;
        texDesc.MipLevels          = 1;
        texDesc.Format             = GBUFFER_FORMATS[i];
        texDesc.SampleDesc.Count   = 1;
        texDesc.Flags              = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE clearVal = {};
        clearVal.Format            = GBUFFER_FORMATS[i];
        // Zero clear colour for all G-buffer targets

        HRESULT hr = _d3d12maAllocator->CreateResource(
            &allocDesc, &texDesc,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            &clearVal,
            &_gbufferAlloc[i],
            IID_PPV_ARGS(&_gbuffer[i]));

        if (FAILED(hr))
        {
            LUNA_LOG_ERROR("Failed to create G-buffer[%u]: 0x%08lX", i, (unsigned long)hr);
            return false;
        }

        // RTV descriptor
        _gbufferRTV[i].ptr = _gbufferRtvHeap->GetCPUDescriptorHandleForHeapStart().ptr
                             + i * rtvSize;
        _device->CreateRenderTargetView(_gbuffer[i].Get(), nullptr, _gbufferRTV[i]);

        // SRV descriptor — allocate slot once; on resize re-create descriptor at same index
        D3D12_CPU_DESCRIPTOR_HANDLE srvCPU;
        D3D12_GPU_DESCRIPTOR_HANDLE srvGPU;
        if (_gbufferSRVIndex[i] == UINT_MAX)
            _gbufferSRVIndex[i] = AllocateSRVSlot(srvCPU, srvGPU);
        else
        {
            srvCPU.ptr = _imGuiSrvHeap->GetCPUDescriptorHandleForHeapStart().ptr
                         + _gbufferSRVIndex[i] * _srvDescriptorSize;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format                  = GBUFFER_FORMATS[i];
        srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels     = 1;
        _device->CreateShaderResourceView(_gbuffer[i].Get(), &srvDesc, srvCPU);
    }

    // Depth SRV (slot _gbufferSRVIndex[2]+1) — must be allocated AFTER GB0/1/2 so the
    // five lighting descriptors (GB0, GB1, GB2, Depth, Shadow) are consecutive.
    // The depth resource already exists (CreateDepthBuffer ran before CreateGBuffer).
    {
        D3D12_CPU_DESCRIPTOR_HANDLE depthSrvCPU;
        D3D12_GPU_DESCRIPTOR_HANDLE depthSrvGPU;
        if (_depthSRVIndex == UINT_MAX)
            _depthSRVIndex = AllocateSRVSlot(depthSrvCPU, depthSrvGPU);
        else
        {
            depthSrvCPU.ptr = _imGuiSrvHeap->GetCPUDescriptorHandleForHeapStart().ptr
                              + _depthSRVIndex * _srvDescriptorSize;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC depthSrvDesc = {};
        depthSrvDesc.Format                  = DXGI_FORMAT_R32_FLOAT;
        depthSrvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        depthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        depthSrvDesc.Texture2D.MipLevels     = 1;
        _device->CreateShaderResourceView(_depthBuffer.Get(), &depthSrvDesc, depthSrvCPU);
    }

    // Shadow SRV — read-only R32_FLOAT view of the same resource as the DXR UAV.
    // Allocated last so the lighting descriptor table (GB0..shadow) spans exactly 5
    // consecutive slots: _gbufferSRVIndex[0] through _gbufferSRVIndex[0]+4.
    {
        D3D12_CPU_DESCRIPTOR_HANDLE shadowSrvCPU;
        D3D12_GPU_DESCRIPTOR_HANDLE shadowSrvGPU;
        if (_shadowSRVIndex == UINT_MAX)
            _shadowSRVIndex = AllocateSRVSlot(shadowSrvCPU, shadowSrvGPU);
        else
        {
            shadowSrvCPU.ptr = _imGuiSrvHeap->GetCPUDescriptorHandleForHeapStart().ptr
                               + _shadowSRVIndex * _srvDescriptorSize;
        }

        // If DXR is disabled, point the shadow slot to the depth buffer (R32_TYPELESS → R32_FLOAT
        // SRV is valid).  The shader reads depth as "shadow" which is incorrect, but at least the
        // format is compatible and the descriptor table remains valid.
        ID3D12Resource* shadowResource = _shadowUAV ? _shadowUAV.Get() : _depthBuffer.Get();
        DXGI_FORMAT     shadowFmt      = DXGI_FORMAT_R32_FLOAT;  // valid for both shadow UAV and depth

        D3D12_SHADER_RESOURCE_VIEW_DESC shadowSrvDesc = {};
        shadowSrvDesc.Format                  = shadowFmt;
        shadowSrvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        shadowSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        shadowSrvDesc.Texture2D.MipLevels     = 1;
        _device->CreateShaderResourceView(shadowResource, &shadowSrvDesc, shadowSrvCPU);
    }

    return true;
}

void DX12Backend::DestroyGBuffer()
{
    for (UINT i = 0; i < GBUFFER_COUNT; ++i)
    {
        _gbuffer[i].Reset();
        if (_gbufferAlloc[i]) { _gbufferAlloc[i]->Release(); _gbufferAlloc[i] = nullptr; }
    }
    _gbufferRtvHeap.Reset();
    // SRV index fields are permanent — do not reset them
}

// ---------------------------------------------------------------------------
// Phase 8: CSM shadow map array — fixed 2048×2048 per slice, 4 slices
// ---------------------------------------------------------------------------
bool DX12Backend::CreateCSMResources()
{
    // Release previous texture (re-entrant safe; DSV heap is created once)
    if (_csmShadowMapAlloc) { _csmShadowMapAlloc->Release(); _csmShadowMapAlloc = nullptr; }
    _csmShadowMap.Reset();

    // --- DSV heap: 4 per-slice descriptors (created once) ---
    if (!_csmDsvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        heapDesc.NumDescriptors = CSM_CASCADE_COUNT;
        heapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

        if (FAILED(_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&_csmDsvHeap))))
        {
            LUNA_LOG_ERROR("Failed to create CSM DSV heap");
            return false;
        }
    }

    // --- Allocate R32_TYPELESS Texture2DArray (4 slices, 2048×2048) ---
    D3D12MA::ALLOCATION_DESC allocDesc = {};
    allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width              = CSM_SHADOW_SIZE;
    texDesc.Height             = CSM_SHADOW_SIZE;
    texDesc.DepthOrArraySize   = static_cast<UINT16>(CSM_CASCADE_COUNT);
    texDesc.MipLevels          = 1;
    texDesc.Format             = DXGI_FORMAT_R32_TYPELESS;  // dual DSV+SRV use
    texDesc.SampleDesc.Count   = 1;
    texDesc.Flags              = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clearVal    = {};
    clearVal.Format               = DXGI_FORMAT_D32_FLOAT;
    clearVal.DepthStencil.Depth   = 1.0f;
    clearVal.DepthStencil.Stencil = 0;

    HRESULT hr = _d3d12maAllocator->CreateResource(
        &allocDesc, &texDesc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &clearVal,
        &_csmShadowMapAlloc,
        IID_PPV_ARGS(&_csmShadowMap));

    if (FAILED(hr))
    {
        LUNA_LOG_ERROR("Failed to create CSM shadow map array: 0x%08lX", (unsigned long)hr);
        return false;
    }

    // --- Per-slice DSV descriptors ---
    UINT dsvSize = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    D3D12_CPU_DESCRIPTOR_HANDLE heapStart = _csmDsvHeap->GetCPUDescriptorHandleForHeapStart();

    for (UINT i = 0; i < CSM_CASCADE_COUNT; ++i)
    {
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
        dsvDesc.Format                         = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension                  = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dsvDesc.Flags                          = D3D12_DSV_FLAG_NONE;
        dsvDesc.Texture2DArray.MipSlice        = 0;
        dsvDesc.Texture2DArray.FirstArraySlice = i;
        dsvDesc.Texture2DArray.ArraySize       = 1;

        _csmDsvHandles[i].ptr = heapStart.ptr + i * dsvSize;
        _device->CreateDepthStencilView(_csmShadowMap.Get(), &dsvDesc, _csmDsvHandles[i]);
    }

    // --- Texture2DArray SRV covering all 4 slices ---
    D3D12_CPU_DESCRIPTOR_HANDLE srvCPU;
    D3D12_GPU_DESCRIPTOR_HANDLE srvGPU;
    if (_csmSRVIndex == UINT_MAX)
        _csmSRVIndex = AllocateSRVSlot(srvCPU, srvGPU);
    else
    {
        srvCPU.ptr = _imGuiSrvHeap->GetCPUDescriptorHandleForHeapStart().ptr
                     + _csmSRVIndex * _srvDescriptorSize;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format                            = DXGI_FORMAT_R32_FLOAT;
    srvDesc.ViewDimension                     = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srvDesc.Shader4ComponentMapping           = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2DArray.MipLevels          = 1;
    srvDesc.Texture2DArray.MostDetailedMip    = 0;
    srvDesc.Texture2DArray.FirstArraySlice    = 0;
    srvDesc.Texture2DArray.ArraySize          = CSM_CASCADE_COUNT;
    _device->CreateShaderResourceView(_csmShadowMap.Get(), &srvDesc, srvCPU);

    // --- Redirect _shadowSRVIndex slot to CSM Texture2DArray ---
    // The deferred lighting descriptor table uses 5 consecutive slots: GB0,GB1,GB2,Depth,Shadow.
    // Slot t4 (_shadowSRVIndex) is repurposed from the DXR shadow UAV SRV to the CSM array SRV.
    if (_shadowSRVIndex != UINT_MAX)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE shadowSrvCPU;
        shadowSrvCPU.ptr = _imGuiSrvHeap->GetCPUDescriptorHandleForHeapStart().ptr
                           + _shadowSRVIndex * _srvDescriptorSize;
        _device->CreateShaderResourceView(_csmShadowMap.Get(), &srvDesc, shadowSrvCPU);
    }

    LUNA_LOG_INFO("CSM shadow map created (%ux%u x %u cascades, slot=%u)",
                  CSM_SHADOW_SIZE, CSM_SHADOW_SIZE, CSM_CASCADE_COUNT, _csmSRVIndex);
    return true;
}

void DX12Backend::DestroyCSMResources()
{
    _csmShadowMap.Reset();
    if (_csmShadowMapAlloc) { _csmShadowMapAlloc->Release(); _csmShadowMapAlloc = nullptr; }
    // _csmDsvHeap created once — not reset here (avoid recreating on re-init)
    // _csmSRVIndex is permanent — sentinel preserved
}

// ---------------------------------------------------------------------------
// Phase 8: UpdateCSMMatrices — practical split scheme + orthographic light VP
// ---------------------------------------------------------------------------
void DX12Backend::UpdateCSMMatrices(const XMFLOAT4X4& view, const XMFLOAT4X4& proj)
{
    // --- Practical split scheme (λ=0.5 blending logarithmic and uniform) ---
    const float nearZ  = 0.1f;
    const float farZ   = 100.0f;
    const float lambda = 0.5f;
    const float ratio  = farZ / nearZ;

    float splits[CSM_CASCADE_COUNT];
    for (UINT i = 0; i < CSM_CASCADE_COUNT; ++i)
    {
        float p       = (i + 1) / static_cast<float>(CSM_CASCADE_COUNT);
        float logSpl  = nearZ * std::pow(ratio, p);
        float unifSpl = nearZ + (farZ - nearZ) * p;
        splits[i]     = lambda * (logSpl - unifSpl) + unifSpl;
    }
    _csmCascadeSplits = XMFLOAT4(splits[0], splits[1], splits[2], splits[3]);

    // Light direction matching UpdateMVP and the HLSL shader
    XMVECTOR lightDirV = XMVector3Normalize(XMVectorSet(1.0f, 2.0f, 1.0f, 0.0f));

    XMMATRIX viewMat = XMLoadFloat4x4(&view);
    XMMATRIX projMat = XMLoadFloat4x4(&proj);
    XMMATRIX invView = XMMatrixInverse(nullptr, viewMat);

    // Extract half-FOV tangents from LH perspective projection matrix:
    //   proj._11 = 1 / (aspect * tan(fovY/2))   → tanHalfFovX = 1 / proj._11
    //   proj._22 = 1 / tan(fovY/2)              → tanHalfFovY = 1 / proj._22
    float tanHalfFovX = 1.0f / XMVectorGetX(projMat.r[0]);
    float tanHalfFovY = 1.0f / XMVectorGetY(projMat.r[1]);

    float lastSplit = nearZ;
    for (UINT cascade = 0; cascade < CSM_CASCADE_COUNT; ++cascade)
    {
        float zN = lastSplit;
        float zF = splits[cascade];

        // Frustum corners in view space (LH: +Z into scene, +Y up, +X right)
        XMFLOAT3 vsCorners[8] = {
            {-tanHalfFovX * zN,  tanHalfFovY * zN, zN},  // near TL
            { tanHalfFovX * zN,  tanHalfFovY * zN, zN},  // near TR
            { tanHalfFovX * zN, -tanHalfFovY * zN, zN},  // near BR
            {-tanHalfFovX * zN, -tanHalfFovY * zN, zN},  // near BL
            {-tanHalfFovX * zF,  tanHalfFovY * zF, zF},  // far  TL
            { tanHalfFovX * zF,  tanHalfFovY * zF, zF},  // far  TR
            { tanHalfFovX * zF, -tanHalfFovY * zF, zF},  // far  BR
            {-tanHalfFovX * zF, -tanHalfFovY * zF, zF},  // far  BL
        };

        // Transform corners to world space via inverse view
        XMVECTOR cornersWS[8];
        XMVECTOR center = XMVectorZero();
        for (UINT j = 0; j < 8; ++j)
        {
            XMVECTOR vs = XMVectorSet(vsCorners[j].x, vsCorners[j].y, vsCorners[j].z, 1.0f);
            cornersWS[j] = XMVector4Transform(vs, invView);
            center       = XMVectorAdd(center, cornersWS[j]);
        }
        center = XMVectorScale(center, 1.0f / 8.0f);

        // Build light-space view matrix: look from center along -lightDir
        XMVECTOR up  = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        if (std::abs(XMVectorGetY(lightDirV)) > 0.99f)
            up = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);  // avoid gimbal lock
        XMVECTOR eye    = XMVectorSubtract(center, lightDirV);
        XMMATRIX lightV = XMMatrixLookAtLH(eye, center, up);

        // AABB of all 8 corners in light space
        XMVECTOR minLS = XMVectorReplicate(FLT_MAX);
        XMVECTOR maxLS = XMVectorReplicate(-FLT_MAX);
        for (UINT j = 0; j < 8; ++j)
        {
            XMVECTOR ls = XMVector4Transform(cornersWS[j], lightV);
            minLS = XMVectorMin(minLS, ls);
            maxLS = XMVectorMax(maxLS, ls);
        }

        float minX = XMVectorGetX(minLS), maxX = XMVectorGetX(maxLS);
        float minY = XMVectorGetY(minLS), maxY = XMVectorGetY(maxLS);
        float minZ = XMVectorGetZ(minLS), maxZ = XMVectorGetZ(maxLS);

        // Pull Z near back to catch shadow casters behind the camera frustum
        float zExtent = maxZ - minZ;
        minZ -= zExtent * 0.5f;

        XMMATRIX lightP = XMMatrixOrthographicOffCenterLH(minX, maxX, minY, maxY, minZ, maxZ);
        XMMATRIX lightVP = XMMatrixMultiply(lightV, lightP);
        XMStoreFloat4x4(&_csmLightVP[cascade], lightVP);

        lastSplit = zF;
    }
}

// ---------------------------------------------------------------------------
// Phase 8: DrawCSMPass — render all scene meshes into the CSM shadow map array.
// Called from DrawFrame() RenderGraph CSM pass lambda.
// Uses the previous frame's cached model matrices (_lastMeshModels) — one frame lag,
// acceptable for directional shadow maps.
// ---------------------------------------------------------------------------
void DX12Backend::DrawCSMPass()
{
    if (!_csmPipeline || !_csmShadowMap)  return;
    if (_sceneMeshes.empty())              return;

    D3D12_VIEWPORT csmViewport = {};
    csmViewport.Width    = static_cast<FLOAT>(CSM_SHADOW_SIZE);
    csmViewport.Height   = static_cast<FLOAT>(CSM_SHADOW_SIZE);
    csmViewport.MinDepth = 0.0f;
    csmViewport.MaxDepth = 1.0f;

    D3D12_RECT csmScissor = { 0, 0,
                               static_cast<LONG>(CSM_SHADOW_SIZE),
                               static_cast<LONG>(CSM_SHADOW_SIZE) };

    BindPipeline(_csmPipeline.get());
    _commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    _commandList->RSSetViewports(1, &csmViewport);
    _commandList->RSSetScissorRects(1, &csmScissor);

    for (UINT cascade = 0; cascade < CSM_CASCADE_COUNT; ++cascade)
    {
        _commandList->OMSetRenderTargets(0, nullptr, FALSE, &_csmDsvHandles[cascade]);
        _commandList->ClearDepthStencilView(_csmDsvHandles[cascade],
                                            D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

        XMMATRIX lvp = XMLoadFloat4x4(&_csmLightVP[cascade]);

        for (size_t mi = 0; mi < _sceneMeshes.size(); ++mi)
        {
            const auto& mesh = _sceneMeshes[mi];
            if (!mesh) continue;

            // Use cached model from the previous frame (or identity on the first frame)
            XMMATRIX model = (mi < _lastMeshModels.size())
                             ? XMLoadFloat4x4(&_lastMeshModels[mi])
                             : XMMatrixIdentity();

            XMMATRIX lmvp = XMMatrixMultiply(model, lvp);
            XMFLOAT4X4 lmvpf;
            XMStoreFloat4x4(&lmvpf, lmvp);

            // Set 16 inline root constants (light-space MVP, 64 B) at params[0]
            _commandList->SetGraphicsRoot32BitConstants(0, 16, &lmvpf, 0);

            _commandList->IASetVertexBuffers(0, 1, &mesh->vbView);
            _commandList->IASetIndexBuffer(&mesh->ibView);
            _commandList->DrawIndexedInstanced(mesh->indexCount, 1, 0, 0, 0);
        }
    }
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
    WaitForComputeFrame(_frameIndex);  // Phase 13: also wait for async compute

    // Phase 22: GPU profiler frame start
    _gpuProfiler.BeginFrame();

    FrameResource& fr = _frames[_frameIndex];
    fr.cmdAllocator->Reset();
    _commandList->Reset(fr.cmdAllocator.Get(), nullptr);

    // Phase 13: reset compute allocator
    if (_asyncComputeReady && fr.computeCmdAllocator)
        fr.computeCmdAllocator->Reset();

    // All barriers, clears, and OMSetRenderTargets are issued inside DrawFrame()
    // via the RenderGraph to keep barrier logic centralised and data-driven.

    // Phase 8: reset cached model matrices — repopulated by DrawMesh() for next CSM pass
    _lastMeshModels.clear();
}

void DX12Backend::DrawFrame()
{
    // -----------------------------------------------------------------------
    // Phase 6: data-driven barrier scheduling via RenderGraph.
    //
    // Resources are imported with their current GPU state and their desired
    // final state after Execute().  The graph emits the minimum set of
    // barriers before each pass and restores resources to finalState at the
    // end, keeping barrier logic out of the individual pass lambdas.
    //
    // After Execute() the command list is still open with:
    //   - BackBuffer in RENDER_TARGET state
    //   - RTV + DSV bound (set by the PBR Forward lambda)
    // DrawMesh() calls from SceneManager::Update() and ImGui rendering both
    // rely on this post-Execute() state being intact.
    // -----------------------------------------------------------------------
    UINT backIdx = _swapChain->GetCurrentBackBufferIndex();

    // Phase 14: pass device pointer so the graph can create transient placed resources
    // and perform DAG cull analysis.
    RenderGraph rg(_device.Get(), _commandList.Get());

    // Import backbuffer: PRESENT → RENDER_TARGET (finalState=RENDER_TARGET so CompositeFrame
    // can write to it without an extra transition).
    auto bbHandle = rg.ImportTexture("BackBuffer",
                                     _rtvBuffer[backIdx].Get(),
                                     D3D12_RESOURCE_STATE_PRESENT,
                                     D3D12_RESOURCE_STATE_RENDER_TARGET);

    // Import depth: starts and ends in DEPTH_WRITE (auto-restore).
    auto depthHdl = rg.ImportTexture("Depth",
                                     _depthBuffer.Get(),
                                     D3D12_RESOURCE_STATE_DEPTH_WRITE);

    // Import G-buffer textures: start and end in RENDER_TARGET (auto-restore).
    RGResourceHandle gbHdl[GBUFFER_COUNT] = {};
    for (UINT i = 0; i < GBUFFER_COUNT; ++i)
    {
        char name[16];
        snprintf(name, sizeof(name), "GBuf%u", i);
        gbHdl[i] = rg.ImportTexture(name,
                                     _gbuffer[i].Get(),
                                     D3D12_RESOURCE_STATE_RENDER_TARGET);
    }

    // Import shadow UAV only when DXR is available.
    RGResourceHandle shadowHdl = RG_NULL_HANDLE;
    if (_dxrSupported && _rtPipeline && _accelStructure && _shadowUAV)
    {
        shadowHdl = rg.ImportTexture("ShadowMap",
                                     _shadowUAV.Get(),
                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    // Phase 8: import CSM shadow map array — stays in DEPTH_WRITE through this graph.
    RGResourceHandle csmHdl = RG_NULL_HANDLE;
    if (_csmShadowMap)
    {
        csmHdl = rg.ImportTexture("CSMShadow",
                                   _csmShadowMap.Get(),
                                   D3D12_RESOURCE_STATE_DEPTH_WRITE);
    }

    // --- Pass 0: CSM Shadow depth pre-pass ---
    if (csmHdl != RG_NULL_HANDLE)
    {
        rg.AddPass("CSM Shadows")
          // SideEffect: CSM shadow map is a persistent resource consumed by CompositeFrame's
          // rg2 (separate RenderGraph). Without SideEffect the DAG sees no live consumer in
          // this graph and culls the pass every frame.
          .SideEffect()
          .Write(csmHdl, D3D12_RESOURCE_STATE_DEPTH_WRITE)
          .Execute([this](ID3D12GraphicsCommandList* cmd)
          {
              _gpuProfiler.InsertBeginTimestamp(cmd, "CSM Shadows");
              DrawCSMPass();
              _gpuProfiler.InsertEndTimestamp(cmd);
          });
    }

    // --- Pass 1: DXR Shadows ---
    if (shadowHdl != RG_NULL_HANDLE)
    {
        rg.AddPass("DXR Shadows")
          .Read (depthHdl,  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
          .Write(shadowHdl, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
          .Execute([this](ID3D12GraphicsCommandList* cmd)
          {
              _gpuProfiler.InsertBeginTimestamp(cmd, "DXR Shadows");
              ComPtr<ID3D12GraphicsCommandList4> cmd4;
              _commandList.As(&cmd4);
              if (cmd4)
              {
                  _rtPipeline->DispatchShadows(cmd4.Get(),
                                               _accelStructure->GetTLASAddress(),
                                               _shadowUAV.Get(),
                                               _depthBuffer.Get(),
                                               static_cast<UINT>(_screenWidth),
                                               static_cast<UINT>(_screenHeight),
                                               _frames[_frameIndex].mvpCBGPUAddr);
              }
              _gpuProfiler.InsertEndTimestamp(cmd);
          });
    }

    // --- Pass 2: GBuffer Fill (clears 3 G-buffer RTVs + depth; geometry written via DrawMesh) ---
    {
        auto pb = rg.AddPass("GBuffer Fill");
        // Phase 14: SideEffect — this pass leaves G-buffer RTVs + depth bound so that
        // DrawMesh() calls from SceneManager::Update() can write geometry after Execute().
        // Without SideEffect the DAG cull would leave it uncullable only if its writes
        // feed a subsequent live pass; SideEffect makes the guarantee explicit.
        pb.SideEffect()
          // Touch backbuffer to force PRESENT→RT barrier (consistent state for EndFrame).
          .Write(bbHandle,     D3D12_RESOURCE_STATE_RENDER_TARGET)
          .Write(depthHdl,     D3D12_RESOURCE_STATE_DEPTH_WRITE)
          .Write(gbHdl[0],     D3D12_RESOURCE_STATE_RENDER_TARGET)
          .Write(gbHdl[1],     D3D12_RESOURCE_STATE_RENDER_TARGET)
          .Write(gbHdl[2],     D3D12_RESOURCE_STATE_RENDER_TARGET);

        pb.Execute([this](ID3D12GraphicsCommandList* cmd)
        {
            _gpuProfiler.InsertBeginTimestamp(cmd, "GBuffer Fill");
            cmd->RSSetViewports(1, &_screenViewport);
            cmd->RSSetScissorRects(1, &_scissorRect);

            // Clear G-buffer targets
            const FLOAT gbClear[] = {0.0f, 0.0f, 0.0f, 0.0f};
            for (UINT i = 0; i < GBUFFER_COUNT; ++i)
                cmd->ClearRenderTargetView(_gbufferRTV[i], gbClear, 0, nullptr);

            // Clear depth
            cmd->ClearDepthStencilView(_dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

            // Bind 3 G-buffer RTVs + depth (no back buffer yet — geometry writes to G-buffer only)
            D3D12_CPU_DESCRIPTOR_HANDLE rtvs[GBUFFER_COUNT] = { _gbufferRTV[0], _gbufferRTV[1], _gbufferRTV[2] };
            cmd->OMSetRenderTargets(GBUFFER_COUNT, rtvs, FALSE, &_dsvHandle);
            // DrawMesh() calls write geometry here via _gbufferPipeline after Execute().
            // Note: GBuffer Fill end timestamp deferred to FlushDraws/CompositeFrame
        });
    }

    rg.Compile();
    rg.Execute();
    // Command list remains open.
    // G-buffer RTVs + depth are bound; DrawMesh() writes geometry before CompositeFrame().
}

// ---------------------------------------------------------------------------
// Phase 7: Deferred composite — reads G-buffer, runs Cook-Torrance lighting,
// writes final colour to the back buffer.  Called after SceneManager::Update()
// (which populates the G-buffer via DrawMesh) and before RenderImGui().
// ---------------------------------------------------------------------------
void DX12Backend::CompositeFrame()
{
    if (!_lightingPipeline && !_lightingPipelineHDR && !_lightingPipelineIBL) return;
    if (_gbufferSRVIndex[0] == UINT_MAX) return;  // G-buffer not yet initialized

    UINT backIdx = _swapChain->GetCurrentBackBufferIndex();

    // Phase 14: pass device pointer for DAG cull + transient aliasing support
    RenderGraph rg2(_device.Get(), _commandList.Get());

    // G-buffer textures transition RENDER_TARGET → PIXEL_SHADER_RESOURCE, then restored.
    auto gb0Hdl = rg2.ImportTexture("GBuf0", _gbuffer[0].Get(),
                                    D3D12_RESOURCE_STATE_RENDER_TARGET);
    auto gb1Hdl = rg2.ImportTexture("GBuf1", _gbuffer[1].Get(),
                                    D3D12_RESOURCE_STATE_RENDER_TARGET);
    auto gb2Hdl = rg2.ImportTexture("GBuf2", _gbuffer[2].Get(),
                                    D3D12_RESOURCE_STATE_RENDER_TARGET);

    // Depth: DEPTH_WRITE → PIXEL_SHADER_RESOURCE, then restored.
    auto depthHdl = rg2.ImportTexture("Depth", _depthBuffer.Get(),
                                      D3D12_RESOURCE_STATE_DEPTH_WRITE);

    // Back buffer stays in RENDER_TARGET (for ImGui after this pass).
    auto bbHandle = rg2.ImportTexture("BackBuffer", _rtvBuffer[backIdx].Get(),
                                      D3D12_RESOURCE_STATE_RENDER_TARGET);

    // Shadow UAV SRV (if DXR active).
    RGResourceHandle shadowHdl = RG_NULL_HANDLE;
    if (_dxrSupported && _shadowUAV && _shadowSRVIndex != UINT_MAX)
    {
        shadowHdl = rg2.ImportTexture("ShadowMap", _shadowUAV.Get(),
                                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    // Phase 8: CSM shadow map — transitions DEPTH_WRITE → PIXEL_SHADER_RESOURCE for the
    // lighting pass, then restored to DEPTH_WRITE at the end of the graph.
    RGResourceHandle csmHdl = RG_NULL_HANDLE;
    if (_csmShadowMap && _shadowSRVIndex != UINT_MAX)
    {
        csmHdl = rg2.ImportTexture("CSMShadow", _csmShadowMap.Get(),
                                    D3D12_RESOURCE_STATE_DEPTH_WRITE);
    }

    // Phase 9: SSAO pass — must run after G-buffer is populated and before deferred lighting.
    if (_ssaoPipeline && _ssaoRT)
    {
        rg2.AddPass("SSAO")
           // Phase 14: SideEffect — writes to _ssaoRT and _ssaoBlurRT which are imported
           // into the Deferred Lighting descriptor table; marking as side-effect prevents cull.
           .SideEffect()
           .Read(depthHdl, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
           .Read(gb1Hdl,   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
           .Execute([this](ID3D12GraphicsCommandList* cmd)
           {
               _gpuProfiler.InsertBeginTimestamp(cmd, "SSAO");
               DrawSSAOPass();
               DrawSSAOBlurPass();
               _gpuProfiler.InsertEndTimestamp(cmd);
           });
    }

    // Phase 24: Cluster assignment compute pass (before deferred lighting)
    if (_clusteredLightingReady && !_pointLights.empty())
    {
        rg2.AddPass("Cluster Assign")
           .SideEffect()
           .Execute([this](ID3D12GraphicsCommandList* cmd)
           {
               DispatchClusterAssign();
           });
    }

    // --- Deferred Lighting pass ---
    // Phase 10: if post-process stack is valid, write to _hdrRT (R16G16B16A16_FLOAT) instead
    // of the back buffer. The tone mapping pass will composite to the back buffer afterwards.
    {
        // Import the HDR buffer as a render target (only when Phase 10 is active)
        RGResourceHandle hdrHdl = RG_NULL_HANDLE;
        if (_ppResourcesValid && _hdrRT)
            hdrHdl = rg2.ImportTexture("HDR", _hdrRT.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);

        auto pb = rg2.AddPass("Deferred Lighting");
        // Phase 14: SideEffect — writes final colour to back buffer (or HDR RT) which is
        // consumed by post-process / ImGui / EndFrame that live outside this RenderGraph.
        pb.SideEffect()
          .Read (gb0Hdl,   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
          .Read (gb1Hdl,   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
          .Read (gb2Hdl,   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
          .Read (depthHdl, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        if (hdrHdl != RG_NULL_HANDLE)
            pb.Write(hdrHdl,  D3D12_RESOURCE_STATE_RENDER_TARGET);   // Phase 10: write to HDR
        else
            pb.Write(bbHandle, D3D12_RESOURCE_STATE_RENDER_TARGET);  // Phase 9 fallback: write to BB

        if (csmHdl != RG_NULL_HANDLE)
            pb.Read(csmHdl, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        else if (shadowHdl != RG_NULL_HANDLE)
            pb.Read(shadowHdl, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        pb.Execute([this, backIdx, hdrHdl](ID3D12GraphicsCommandList* cmd)
        {
            _gpuProfiler.InsertBeginTimestamp(cmd, "Deferred Lighting");
            
            // Phase 10: bind HDR RTV; Phase 9 fallback: bind back buffer RTV
            D3D12_CPU_DESCRIPTOR_HANDLE rtv = (hdrHdl != RG_NULL_HANDLE) ? _hdrRTV : _rtvHandle[backIdx];
            cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            cmd->RSSetViewports(1, &_screenViewport);
            cmd->RSSetScissorRects(1, &_scissorRect);

            // Phase 14: IBL pipeline > Phase 10 HDR pipeline > Phase 9 fallback
            DX12Pipeline* pipe = nullptr;
            if (_iblReady && _lightingPipelineIBL)
                pipe = _lightingPipelineIBL.get();
            else if (hdrHdl != RG_NULL_HANDLE && _lightingPipelineHDR)
                pipe = _lightingPipelineHDR.get();
            else
                pipe = _lightingPipeline.get();
            if (!pipe) return;
            BindPipeline(pipe);
            cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            ID3D12DescriptorHeap* heaps[] = {_imGuiSrvHeap.Get()};
            cmd->SetDescriptorHeaps(1, heaps);

            cmd->SetGraphicsRootConstantBufferView(0, _frames[_frameIndex].sceneCBGPUAddr);

            // Phase 24: bind ClusterParams CBV at b1 (params[1]) if using IBL pipeline
            if (_iblReady && _lightingPipelineIBL && pipe == _lightingPipelineIBL.get())
            {
                if (_clusteredLightingReady && _clusterParamsCB)
                    cmd->SetGraphicsRootConstantBufferView(1, _clusterParamsCB->GetGPUVirtualAddress());
            }

            // Root param indices shifted by +1 when using IBL pipeline (b1 inserted)
            UINT gbufferParam = (_iblReady && _lightingPipelineIBL && pipe == _lightingPipelineIBL.get()) ? 2 : 1;
            UINT ssaoParam    = gbufferParam + 1;

            D3D12_GPU_DESCRIPTOR_HANDLE srvBase;
            srvBase.ptr = _imGuiSrvHeap->GetGPUDescriptorHandleForHeapStart().ptr
                          + static_cast<UINT64>(_gbufferSRVIndex[0]) * _srvDescriptorSize;
            cmd->SetGraphicsRootDescriptorTable(gbufferParam, srvBase);

            UINT ssaoIdx = (_ssaoBlurSRVIndex != UINT_MAX) ? _ssaoBlurSRVIndex : _depthSRVIndex;
            D3D12_GPU_DESCRIPTOR_HANDLE ssaoGpu;
            ssaoGpu.ptr = _imGuiSrvHeap->GetGPUDescriptorHandleForHeapStart().ptr
                          + static_cast<UINT64>(ssaoIdx) * _srvDescriptorSize;
            cmd->SetGraphicsRootDescriptorTable(ssaoParam, ssaoGpu);

            // Phase 14+24: bind IBL textures and clustered lighting data if using IBL pipeline
            if (_iblReady && _lightingPipelineIBL && pipe == _lightingPipelineIBL.get())
            {
                auto MakeGpu = [&](UINT idx) -> D3D12_GPU_DESCRIPTOR_HANDLE {
                    D3D12_GPU_DESCRIPTOR_HANDLE g;
                    g.ptr = _imGuiSrvHeap->GetGPUDescriptorHandleForHeapStart().ptr
                            + static_cast<UINT64>(idx) * _srvDescriptorSize;
                    return g;
                };
                cmd->SetGraphicsRootDescriptorTable(4, MakeGpu(_irrCubemapSRVIndex));
                cmd->SetGraphicsRootDescriptorTable(5, MakeGpu(_prefilterCubemapSRVIndex));
                cmd->SetGraphicsRootDescriptorTable(6, MakeGpu(_brdfLUTSRVIndex));

                // Phase 24: bind clustered lighting SRVs
                if (_clusteredLightingReady)
                {
                    cmd->SetGraphicsRootDescriptorTable(7, MakeGpu(_clusterLightSRVIndex));
                    cmd->SetGraphicsRootDescriptorTable(8, MakeGpu(_clusterCountsSRVIndex));
                    cmd->SetGraphicsRootDescriptorTable(9, MakeGpu(_clusterIndicesSRVIndex));
                }
            }

            cmd->DrawInstanced(3, 1, 0, 0);
            _gpuProfiler.InsertEndTimestamp(cmd);
        });
    }

    rg2.Compile();
    rg2.Execute();
    // G-buffer restored to RENDER_TARGET; depth to DEPTH_WRITE; HDR buffer stays in RENDER_TARGET.

    // Phase 10: post-process chain (TAA → Bloom → ACES tone mapping → back buffer)
    if (_ppResourcesValid && _hdrRT)
    {
        // Phase 14: skybox disabled — environment mapping removed
        // DrawSkyboxPass();

        // Phase 16B: SSR compute + additive blend into _hdrRT
        if (_ssrComputePipeline)
        {
            _gpuProfiler.InsertBeginTimestamp(_commandList.Get(), "SSR");
            DrawSSRPass();
            _gpuProfiler.InsertEndTimestamp(_commandList.Get());
        }

        // Phase 18B: motion blur (_hdrRT → _motionBlurRT; TAA reads motionBlur output)
        if (_motionBlurPipeline)
        {
            _gpuProfiler.InsertBeginTimestamp(_commandList.Get(), "Motion Blur");
            DrawMotionBlurPass();
            _gpuProfiler.InsertEndTimestamp(_commandList.Get());
        }

        _gpuProfiler.InsertBeginTimestamp(_commandList.Get(), "TAA");
        DrawTAAPass();
        _gpuProfiler.InsertEndTimestamp(_commandList.Get());

        _gpuProfiler.InsertBeginTimestamp(_commandList.Get(), "Bloom");
        DrawBloomBrightPass();
        DrawBloomBlurPass(true);    // H blur: bloomBright → bloomBlur
        DrawBloomBlurPass(false);   // V blur: bloomBlur → bloomBright (final bloom)
        _gpuProfiler.InsertEndTimestamp(_commandList.Get());

        _gpuProfiler.InsertBeginTimestamp(_commandList.Get(), "Tonemap");
        DrawToneMappingPass(backIdx);
        _gpuProfiler.InsertEndTimestamp(_commandList.Get());

        _taaHistoryIndex = 1 - _taaHistoryIndex;  // flip ping-pong for next frame
    }
    // Command list remains open; back buffer in RENDER_TARGET for ImGui.
}

void DX12Backend::EndFrame()
{
    // Phase 22: Resolve GPU timestamp queries before closing command list
    _gpuProfiler.ResolveQueries(_commandList.Get());

    UINT backIdx = _swapChain->GetCurrentBackBufferIndex();
    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        _rtvBuffer[backIdx].Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PRESENT);
    _commandList->ResourceBarrier(1, &barrier);
    _commandList->Close();

    ID3D12CommandList* lists[] = {_commandList.Get()};
    _commandQueue->ExecuteCommandLists(_countof(lists), lists);
    // P2-04: VSync is runtime-configurable via SetVSync() / ApplicationSpecification::vsync
    _swapChain->Present(_vsync ? 1 : 0, 0);

    // Signal the fence for the frame we just submitted
    ++_globalFenceValue;
    _commandQueue->Signal(_fence.Get(), _globalFenceValue);
    _frames[_frameIndex].fenceValue = _globalFenceValue;

    // Phase 22: Signal profiler fence (must be after ExecuteCommandLists)
    _gpuProfiler.SignalFence(_commandQueue.Get());
    _gpuProfiler.EndFrame();

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

    // Phase 7: recreate G-buffer at new resolution
    DestroyGBuffer();
    if (!CreateGBuffer())
        LUNA_LOG_ERROR("Failed to recreate G-buffer on resize");

    // Phase 8: CreateGBuffer() resets _shadowSRVIndex slot to the DXR UAV SRV (or depth).
    // Re-point it at the CSM Texture2DArray so the lighting descriptor table stays valid.
    if (_csmShadowMap && _shadowSRVIndex != UINT_MAX)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE shadowSrvCPU;
        shadowSrvCPU.ptr = _imGuiSrvHeap->GetCPUDescriptorHandleForHeapStart().ptr
                           + _shadowSRVIndex * _srvDescriptorSize;
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format                            = DXGI_FORMAT_R32_FLOAT;
        srvDesc.ViewDimension                     = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srvDesc.Shader4ComponentMapping           = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2DArray.MipLevels          = 1;
        srvDesc.Texture2DArray.MostDetailedMip    = 0;
        srvDesc.Texture2DArray.FirstArraySlice    = 0;
        srvDesc.Texture2DArray.ArraySize          = CSM_CASCADE_COUNT;
        _device->CreateShaderResourceView(_csmShadowMap.Get(), &srvDesc, shadowSrvCPU);
    }

    // Phase 9: recreate SSAO render targets at new half-resolution
    DestroySSAOResources();
    if (!CreateSSAOResources())
        LUNA_LOG_WARN("Failed to recreate SSAO resources on resize");

    // Phase 10: recreate post-process targets at new resolution
    DestroyPostProcessResources();
    if (!CreatePostProcessResources())
        LUNA_LOG_WARN("Failed to recreate post-process resources on resize");
    _taaHistoryIndex = 0;   // history is stale after resize — reset ping-pong

    // Phase 23: recreate Hi-Z pyramid at new resolution
    DestroyHiZResources();
    if (_gpuDrivenReady && !CreateHiZResources())
        LUNA_LOG_WARN("Failed to recreate Hi-Z resources on resize — frustum-only culling");
}

// ---------------------------------------------------------------------------
// MVP update — per-frame, safe because ring buffer guarantees no GPU overlap
// ---------------------------------------------------------------------------
void DX12Backend::UpdateMVP(const XMFLOAT4X4& model, const XMFLOAT4X4& view,
                            const XMFLOAT4X4& proj)
{
    XMMATRIX viewMat = XMLoadFloat4x4(&view);
    XMMATRIX projMat = XMLoadFloat4x4(&proj);

    // -----------------------------------------------------------------------
    // Phase 10: Halton(2,3) jitter — applies sub-pixel TAA offset to proj matrix.
    // Modifying P._31 and P._32 shifts clip.xy by jitter * view.z, which is a
    // constant NDC offset (since clip.w = view.z for LH perspective).
    // -----------------------------------------------------------------------
    _ppPrevJitter = _ppCurJitter;
    if (_ppResourcesValid)
    {
        auto Halton = [](int i, int b) -> float
        {
            float r = 0.0f, f = 1.0f / float(b);
            for (; i > 0; i /= b, f /= float(b)) r += f * float(i % b);
            return r;
        };
        int idx = int(_ppFrameCount % 16);
        _ppCurJitter.x = (Halton(idx + 1, 2) - 0.5f) * 1.0f / float(_screenWidth);
        _ppCurJitter.y = (Halton(idx + 1, 3) - 0.5f) * 1.0f / float(_screenHeight);
    }
    else
    {
        _ppCurJitter = {0.0f, 0.0f};
    }

    // Unjittered VP — stored as _ppPrevVP for the NEXT frame's TAA reprojection.
    XMMATRIX unjitteredVP = XMMatrixMultiply(viewMat, projMat);

    // Jittered projection (modify P row-3 in row-major DirectXMath)
    XMFLOAT4X4 jitteredProjF;
    XMStoreFloat4x4(&jitteredProjF, projMat);
    jitteredProjF._31 += _ppCurJitter.x;
    jitteredProjF._32 += _ppCurJitter.y;
    XMMATRIX jitteredProjMat = XMLoadFloat4x4(&jitteredProjF);

    // Cache jittered proj so DrawMesh() uses sub-pixel positions for the G-buffer
    _lastView = view;
    XMStoreFloat4x4(&_lastProj, jitteredProjMat);

    // MVP CB uses jittered proj so geometry renders at sub-pixel offsets
    MVPConstants mvp;
    mvp.model = model;
    XMStoreFloat4x4(&mvp.view, viewMat);
    XMStoreFloat4x4(&mvp.proj, jitteredProjMat);
    memcpy(_frames[_frameIndex].mvpCBMapped, &mvp, sizeof(MVPConstants));

    // Phase 7: fill SceneCB (uses unjittered invVP for stable deferred lighting)
    if (_frames[_frameIndex].sceneCBMapped)
    {
        XMMATRIX invVP = XMMatrixInverse(nullptr, unjitteredVP);

        float negTx = -view._41, negTy = -view._42, negTz = -view._43;
        float eyeX = view._11 * negTx + view._12 * negTy + view._13 * negTz;
        float eyeY = view._21 * negTx + view._22 * negTy + view._23 * negTz;
        float eyeZ = view._31 * negTx + view._32 * negTy + view._33 * negTz;

        UpdateCSMMatrices(view, proj);

        SceneConstants sc = {};
        XMStoreFloat4x4(&sc.invViewProj, invVP);
        sc.eyePosition = XMFLOAT3(eyeX, eyeY, eyeZ);
        sc._padEye     = 0.0f;

        XMVECTOR lightDirV = XMVector3Normalize(XMVectorSet(1.0f, 2.0f, 1.0f, 0.0f));
        XMStoreFloat3(&sc.lightDir, lightDirV);
        sc._padLight  = 0.0f;
        sc.lightColor = XMFLOAT4(1.0f, 1.0f, 1.0f, 3.0f);

        XMStoreFloat4x4(&sc.viewMatrix, viewMat);
        for (UINT i = 0; i < CSM_CASCADE_COUNT; ++i)
            sc.lightVP[i] = _csmLightVP[i];
        sc.cascadeSplits = _csmCascadeSplits;

        // Phase 24: Clustered lighting
        sc.numPointLights = _clusteredLightingReady ? static_cast<uint32_t>(_pointLights.size()) : 0u;
        sc._pad2[0] = sc._pad2[1] = sc._pad2[2] = 0;

        memcpy(_frames[_frameIndex].sceneCBMapped, &sc, sizeof(SceneConstants));
    }

    // Phase 10: fill per-frame TAA CB
    if (_ppResourcesValid && _taaCBMapped[_frameIndex])
    {
        TAAConstants tc = {};
        XMMATRIX jitteredVP = XMMatrixMultiply(viewMat, jitteredProjMat);
        XMStoreFloat4x4(&tc.invViewProj,  XMMatrixInverse(nullptr, jitteredVP));
        tc.prevViewProj = _ppPrevVP;       // unjittered VP of the PREVIOUS frame
        tc.jitter       = _ppCurJitter;
        tc.prevJitter   = _ppPrevJitter;
        // Warm-up: use full current-frame weight for the first few frames (history is black)
        tc.alpha = (_ppFrameCount < 8) ? 1.0f : 0.1f;
        memcpy(_taaCBMapped[_frameIndex], &tc, sizeof(TAAConstants));
    }

    // Save current unjittered VP — used as prevVP in the NEXT frame's TAAConstants
    XMStoreFloat4x4(&_ppPrevVP, unjitteredVP);
    ++_ppFrameCount;
}

// ---------------------------------------------------------------------------
// Helper: create a 1×1 solid-colour DEFAULT-heap texture and upload via cmdList
// ---------------------------------------------------------------------------
static std::shared_ptr<Luna::Texture> MakeSolidTexture(
    uint8_t r, uint8_t g, uint8_t b, uint8_t a,
    ID3D12Device* device,
    D3D12MA::Allocator* allocator,
    ID3D12GraphicsCommandList* cmdList,
    std::vector<ComPtr<ID3D12Resource>>& stagingKeepAlive,
    std::vector<D3D12MA::Allocation*>&   stagingAllocs)
{
    auto tex = std::make_shared<Luna::Texture>();
    uint8_t pixels[4] = {r, g, b, a};
    ComPtr<ID3D12Resource> staging;
    D3D12MA::Allocation*   stagingAlloc = nullptr;
    if (tex->LoadFromRGBA8(pixels, 1, 1, device, allocator, cmdList, staging, &stagingAlloc))
    {
        stagingKeepAlive.push_back(staging);
        stagingAllocs.push_back(stagingAlloc);
        return tex;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Scene mesh loading — one-shot GPU upload, blocks until complete.
// Phase 5B: also builds Material GPU resources for each glTF material.
// ---------------------------------------------------------------------------
std::vector<std::shared_ptr<Mesh>> DX12Backend::LoadMeshes(const std::string& path)
{
    WaitAllFrames();

    // --- Pass 1: geometry upload ---
    _frames[0].cmdAllocator->Reset();
    _commandList->Reset(_frames[0].cmdAllocator.Get(), nullptr);

    LoadResult loaded = MeshLoader::LoadGLTF(path, _device.Get(), _d3d12maAllocator, _commandList.Get());

    _commandList->Close();
    ID3D12CommandList* lists[] = {_commandList.Get()};
    _commandQueue->ExecuteCommandLists(_countof(lists), lists);

    ++_globalFenceValue;
    _commandQueue->Signal(_fence.Get(), _globalFenceValue);
    _frames[0].fenceValue = _globalFenceValue;
    WaitForFrame(0);

    // --- Pass 2: material / texture upload (Phase 5B) ---
    // Re-open command list for texture uploads
    _frames[0].cmdAllocator->Reset();
    _commandList->Reset(_frames[0].cmdAllocator.Get(), nullptr);

    // Staging resources that must stay alive until GPU upload is complete
    std::vector<ComPtr<ID3D12Resource>> texStagingBufs;
    std::vector<D3D12MA::Allocation*>   texStagingAllocs;

    // Build one Material per unique glTF material
    std::vector<std::shared_ptr<Material>> gpuMaterials(loaded.materials.size());

    for (size_t mi = 0; mi < loaded.materials.size(); ++mi)
    {
        const MaterialCreateInfo& info = loaded.materials[mi];
        auto mat = std::make_shared<Material>();

        // --- Create material constant buffer (256-byte aligned, UPLOAD heap) ---
        const UINT cbSize = (sizeof(MaterialConstants) + 255) & ~255;
        D3D12_HEAP_PROPERTIES heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        D3D12_RESOURCE_DESC   cbDesc    = CD3DX12_RESOURCE_DESC::Buffer(cbSize);

        if (SUCCEEDED(_device->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &cbDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&mat->constantBuffer))))
        {
            D3D12_RANGE range = {0, 0};
            mat->constantBuffer->Map(0, &range, &mat->cbMapped);
            mat->cbGPUAddr = mat->constantBuffer->GetGPUVirtualAddress();

            MaterialConstants mc;
            mc.albedoFactor    = info.albedoFactor;
            mc.metallicFactor  = info.metallicFactor;
            mc.roughnessFactor = info.roughnessFactor;
            memcpy(mat->cbMapped, &mc, sizeof(mc));
        }

        // --- Load textures: use embedded data if present, otherwise solid defaults ---

        // Albedo
        if (!info.albedoPixels.empty())
        {
            auto tex = std::make_shared<Texture>();
            ComPtr<ID3D12Resource> staging; D3D12MA::Allocation* stagingAlloc = nullptr;
            if (tex->LoadFromRGBA8(info.albedoPixels.data(), info.albedoW, info.albedoH,
                                   _device.Get(), _d3d12maAllocator, _commandList.Get(),
                                   staging, &stagingAlloc))
            {
                mat->albedo = tex;
                texStagingBufs.push_back(staging);
                texStagingAllocs.push_back(stagingAlloc);
            }
        }
        if (!mat->albedo)
            mat->albedo = MakeSolidTexture(255, 255, 255, 255, _device.Get(), _d3d12maAllocator,
                                           _commandList.Get(), texStagingBufs, texStagingAllocs);

        // Normal map
        if (!info.normalPixels.empty())
        {
            auto tex = std::make_shared<Texture>();
            ComPtr<ID3D12Resource> staging; D3D12MA::Allocation* stagingAlloc = nullptr;
            if (tex->LoadFromRGBA8(info.normalPixels.data(), info.normalW, info.normalH,
                                   _device.Get(), _d3d12maAllocator, _commandList.Get(),
                                   staging, &stagingAlloc))
            {
                mat->normalMap = tex;
                texStagingBufs.push_back(staging);
                texStagingAllocs.push_back(stagingAlloc);
            }
        }
        if (!mat->normalMap)
            mat->normalMap = MakeSolidTexture(128, 128, 255, 255, _device.Get(), _d3d12maAllocator,
                                              _commandList.Get(), texStagingBufs, texStagingAllocs);

        // Metallic-roughness (G=roughness, B=metallic)
        if (!info.metalRoughPixels.empty())
        {
            auto tex = std::make_shared<Texture>();
            ComPtr<ID3D12Resource> staging; D3D12MA::Allocation* stagingAlloc = nullptr;
            if (tex->LoadFromRGBA8(info.metalRoughPixels.data(), info.metalRoughW, info.metalRoughH,
                                   _device.Get(), _d3d12maAllocator, _commandList.Get(),
                                   staging, &stagingAlloc))
            {
                mat->metalRough = tex;
                texStagingBufs.push_back(staging);
                texStagingAllocs.push_back(stagingAlloc);
            }
        }
        if (!mat->metalRough)
            mat->metalRough = MakeSolidTexture(0, 128, 0, 255, _device.Get(), _d3d12maAllocator,
                                               _commandList.Get(), texStagingBufs, texStagingAllocs);

        // Emissive map (black default = no emission)
        if (!info.emissivePixels.empty())
        {
            auto tex = std::make_shared<Texture>();
            ComPtr<ID3D12Resource> staging; D3D12MA::Allocation* stagingAlloc = nullptr;
            if (tex->LoadFromRGBA8(info.emissivePixels.data(), info.emissiveW, info.emissiveH,
                                   _device.Get(), _d3d12maAllocator, _commandList.Get(),
                                   staging, &stagingAlloc))
            {
                mat->emissive = tex;
                texStagingBufs.push_back(staging);
                texStagingAllocs.push_back(stagingAlloc);
            }
        }
        if (!mat->emissive)
            mat->emissive = MakeSolidTexture(0, 0, 0, 255, _device.Get(), _d3d12maAllocator,
                                             _commandList.Get(), texStagingBufs, texStagingAllocs);

        // --- Allocate 4 consecutive SRV slots and write descriptors ---
        // Phase 11 bindless: srvTableStart is the base index pushed as a root 32-bit constant
        // per draw. The shader reads gAllTextures[srvTableStart+0/1/2/3] via the unbounded heap table.
        D3D12_CPU_DESCRIPTOR_HANDLE cpuStart;
        D3D12_GPU_DESCRIPTOR_HANDLE gpuStart;
        mat->srvTableStart = AllocateSRVSlot(cpuStart, gpuStart);

        D3D12_CPU_DESCRIPTOR_HANDLE cpuNorm, cpuMR, cpuEmissive;
        D3D12_GPU_DESCRIPTOR_HANDLE gpuDummy;
        AllocateSRVSlot(cpuNorm,     gpuDummy);  // slot: srvTableStart + 1
        AllocateSRVSlot(cpuMR,       gpuDummy);  // slot: srvTableStart + 2
        AllocateSRVSlot(cpuEmissive, gpuDummy);  // slot: srvTableStart + 3

        if (mat->albedo)    mat->albedo->CreateSRV(_device.Get(), cpuStart);
        if (mat->normalMap) mat->normalMap->CreateSRV(_device.Get(), cpuNorm);
        if (mat->metalRough) mat->metalRough->CreateSRV(_device.Get(), cpuMR);
        if (mat->emissive)  mat->emissive->CreateSRV(_device.Get(), cpuEmissive);

        LUNA_LOG_INFO("Material %zu: srvTableStart=%u, albedo=%s, normal=%s, metalRough=%s, emissive=%s",
                      mi, mat->srvTableStart,
                      mat->albedo ? "loaded" : "default",
                      mat->normalMap ? "loaded" : "default",
                      mat->metalRough ? "loaded" : "default",
                      mat->emissive ? "loaded" : "default");

        gpuMaterials[mi] = mat;
    }

    // Execute texture uploads and wait
    _commandList->Close();
    _commandQueue->ExecuteCommandLists(_countof(lists), lists);
    ++_globalFenceValue;
    _commandQueue->Signal(_fence.Get(), _globalFenceValue);
    _frames[0].fenceValue = _globalFenceValue;
    WaitForFrame(0);

    // Release texture staging allocations
    for (auto* a : texStagingAllocs)
        if (a) a->Release();

    // --- Convert unique_ptr → shared_ptr, assign materials ---
    _sceneMeshes.clear();
    _sceneMeshes.reserve(loaded.meshes.size());
    _lastLoadTransforms = std::move(loaded.transforms); // Phase 21
    for (size_t i = 0; i < loaded.meshes.size(); ++i)
    {
        auto sp = std::shared_ptr<Mesh>(std::move(loaded.meshes[i]));
        if (sp->materialIndex < gpuMaterials.size())
            sp->material = gpuMaterials[sp->materialIndex];
        if (i < loaded.meshNames.size())
            sp->name = loaded.meshNames[i];
        _sceneMeshes.push_back(sp);
    }

    LUNA_LOG_INFO("Loaded %zu mesh(es) from %s", _sceneMeshes.size(), path.c_str());

    // Phase 12: build merged geometry + indirect resources for GPU-driven rendering
    if (!_sceneMeshes.empty())
    {
        if (!CreateIndirectResources())
            LUNA_LOG_WARN("Phase 12: GPU-driven rendering init failed — using legacy per-draw path");
        else if (!CreateHiZResources())
            LUNA_LOG_WARN("Phase 23: Hi-Z occlusion culling init failed — frustum-only culling");

        // Phase 25: Create mesh shader pipeline (after meshlets are built in BuildMergedGeometry)
        if (_meshShadersSupported && _meshletBuffer)
        {
            if (!CreateMeshShaderResources())
                LUNA_LOG_WARN("Phase 25: Mesh shader init failed — using indirect draw fallback");
        }
    }

    return _sceneMeshes;
}

// ---------------------------------------------------------------------------
// Per-object mesh draw — must be called between BeginFrame() and EndFrame()
// Phase 7: uses _gbufferPipeline (G-buffer fill) when the mesh has a material, preview otherwise.
// ---------------------------------------------------------------------------
void DX12Backend::DrawMesh(const Mesh* mesh, const XMFLOAT4X4& model)
{
    if (!mesh)
    {
        LUNA_LOG_WARN("DrawMesh: mesh is null");
        return;
    }

    // Phase 8: cache model matrix for the next frame's CSM depth pre-pass
    _lastMeshModels.push_back(model);

    // Phase 12: GPU-driven path — record instance for deferred indirect execution
    if (_gpuDrivenReady && mesh->material && _cpuInstances.size() < MAX_GPU_OBJECTS)
    {
        // Find mesh index in _sceneMeshes
        UINT meshIdx = 0;
        for (UINT i = 0; i < static_cast<UINT>(_sceneMeshes.size()); ++i)
        {
            if (_sceneMeshes[i].get() == mesh) { meshIdx = i; break; }
        }

        GPUObjectData obj;
        obj.model          = model;
        obj.boundingSphere = mesh->boundingSphere;
        obj.meshIndex      = meshIdx;
        obj.materialIndex  = mesh->material->srvTableStart;
        obj.materialCBAddr = mesh->material->cbGPUAddr;
        _cpuInstances.push_back(obj);
        return;  // draw will happen in FlushDraws()
    }

    // Legacy per-draw path (fallback when GPU-driven is not ready)
    // Update MVP CB with this object's model matrix + cached camera matrices
    MVPConstants mvp;
    mvp.model = model;
    mvp.view  = _lastView;
    mvp.proj  = _lastProj;
    memcpy(_frames[_frameIndex].mvpCBMapped, &mvp, sizeof(MVPConstants));

    // Phase 7: use _gbufferPipeline (PBR vertex + G-buffer fill pixel shader)
    const bool usePBR = (mesh->material && _gbufferPipeline);

    if (usePBR)
    {
        BindPipeline(_gbufferPipeline.get());
        SetPipelineState(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, &_screenViewport, &_scissorRect);

        ID3D12DescriptorHeap* heaps[] = {_imGuiSrvHeap.Get()};
        _commandList->SetDescriptorHeaps(1, heaps);

        _commandList->SetGraphicsRootConstantBufferView(0, _frames[_frameIndex].mvpCBGPUAddr);
        _commandList->SetGraphicsRootConstantBufferView(1, mesh->material->cbGPUAddr);
        UINT matIdx = mesh->material->srvTableStart;
        _commandList->SetGraphicsRoot32BitConstant(2, matIdx, 0);
        D3D12_GPU_DESCRIPTOR_HANDLE heapBase = _imGuiSrvHeap->GetGPUDescriptorHandleForHeapStart();
        _commandList->SetGraphicsRootDescriptorTable(3, heapBase);
    }
    else if (_meshPreviewPipeline)
    {
        LUNA_LOG_WARN("DrawMesh: using preview pipeline (mesh->material=%p, _gbufferPipeline=%p)",
                      mesh->material.get(), _gbufferPipeline.get());
        BindPipeline(_meshPreviewPipeline.get());
        SetPipelineState(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, &_screenViewport, &_scissorRect);
        _commandList->SetGraphicsRootConstantBufferView(0, _frames[_frameIndex].mvpCBGPUAddr);
    }
    else
    {
        LUNA_LOG_ERROR("DrawMesh: No usable pipeline!");
        return;
    }

    _commandList->IASetVertexBuffers(0, 1, &mesh->vbView);
    _commandList->IASetIndexBuffer(&mesh->ibView);
    _commandList->DrawIndexedInstanced(mesh->indexCount, 1, 0, 0, 0);
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

    // Use the modern ImGui_ImplDX12_InitInfo API (required for imgui 1.92+).
    // Providing SrvDescriptorAllocFn keeps ImGuiBackendFlags_RendererHasTextures set,
    // which is required by imgui 1.92+ for the dynamic font atlas.
    // The legacy 6-arg ImGui_ImplDX12_Init() explicitly removes that flag (line 1048 of
    // imgui_impl_dx12.cpp) causing an assertion in ImFontAtlasUpdateNewFrame().
    ImGui_ImplDX12_InitInfo initInfo = {};
    initInfo.Device            = _device.Get();
    initInfo.CommandQueue      = _commandQueue.Get();
    initInfo.NumFramesInFlight = SWAP_CHAIN_BUFFER_COUNT;
    initInfo.RTVFormat         = _backBufferFormat;
    initInfo.SrvDescriptorHeap = _imGuiSrvHeap.Get();
    initInfo.UserData          = this;
    initInfo.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* info,
                                       D3D12_CPU_DESCRIPTOR_HANDLE* outCPU,
                                       D3D12_GPU_DESCRIPTOR_HANDLE* outGPU)
    {
        static_cast<DX12Backend*>(info->UserData)->AllocateSRVSlot(*outCPU, *outGPU);
    };
    initInfo.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo*,
                                      D3D12_CPU_DESCRIPTOR_HANDLE,
                                      D3D12_GPU_DESCRIPTOR_HANDLE) {};

    if (!ImGui_ImplDX12_Init(&initInfo))
    {
        LUNA_LOG_ERROR("ImGui_ImplDX12_Init failed");
        return;
    }
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

    // Explicitly bind backbuffer RTV for ImGui rendering
    UINT backIdx = _swapChain->GetCurrentBackBufferIndex();
    _commandList->OMSetRenderTargets(1, &_rtvHandle[backIdx], FALSE, nullptr);
    _commandList->RSSetViewports(1, &_screenViewport);
    _commandList->RSSetScissorRects(1, &_scissorRect);

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
    // Flush all in-flight GPU work before releasing ImGui's DX12 resources
    WaitAllFrames();

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

// ===========================================================================
// Phase 9: SSAO — resource creation / pass drawing
// ===========================================================================

// Generate a hemisphere sample kernel in view space (z >= 0).
// Writes 'sampleCount' XMFLOAT4 entries (w=0) into 'samples'.
static void GenerateSSAOKernel(XMFLOAT4* samples, int sampleCount)
{
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);

    for (int i = 0; i < sampleCount; ++i)
    {
        // Random direction in upper hemisphere
        XMFLOAT3 sample{
            dist(rng) * 2.0f - 1.0f,
            dist(rng) * 2.0f - 1.0f,
            dist(rng)              // z in [0,1] → upper hemisphere
        };
        XMVECTOR v = XMVector3Normalize(XMLoadFloat3(&sample));

        // Accelerating interpolation towards origin (more samples close-in)
        float scale = float(i) / float(sampleCount);
        scale = 0.1f + scale * scale * 0.9f;   // lerp(0.1, 1.0, scale²)
        v = XMVectorScale(v, scale * dist(rng));

        XMFLOAT3 s;
        XMStoreFloat3(&s, v);
        samples[i] = XMFLOAT4(s.x, s.y, s.z, 0.0f);
    }
}

bool DX12Backend::CreateSSAOResources()
{
    const UINT halfW = std::max(1u, (UINT)_screenWidth  / 2);
    const UINT halfH = std::max(1u, (UINT)_screenHeight / 2);

    // ── 1. Raw SSAO render target ────────────────────────────────────────────
    {
        D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R8_UNORM, halfW, halfH, 1, 1,
            1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);

        D3D12_CLEAR_VALUE clearVal = {};
        clearVal.Format = DXGI_FORMAT_R8_UNORM;
        clearVal.Color[0] = 1.0f; // 1 = unoccluded

        D3D12MA::ALLOCATION_DESC allocDesc = {};
        allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

        HRESULT hr = _d3d12maAllocator->CreateResource(
            &allocDesc, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
            &clearVal, &_ssaoRTAlloc, IID_PPV_ARGS(&_ssaoRT));
        if (FAILED(hr)) { LUNA_LOG_ERROR("SSAO RT alloc failed"); return false; }
        _ssaoRT->SetName(L"SSAO_Raw");
    }

    // ── 2. Blurred SSAO render target ───────────────────────────────────────
    {
        D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R8_UNORM, halfW, halfH, 1, 1,
            1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);

        D3D12_CLEAR_VALUE clearVal = {};
        clearVal.Format = DXGI_FORMAT_R8_UNORM;
        clearVal.Color[0] = 1.0f;

        D3D12MA::ALLOCATION_DESC allocDesc = {};
        allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

        HRESULT hr = _d3d12maAllocator->CreateResource(
            &allocDesc, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            &clearVal, &_ssaoBlurRTAlloc, IID_PPV_ARGS(&_ssaoBlurRT));
        if (FAILED(hr)) { LUNA_LOG_ERROR("SSAO Blur RT alloc failed"); return false; }
        _ssaoBlurRT->SetName(L"SSAO_Blur");
    }

    // ── 3. RTV heap for both SSAO targets ───────────────────────────────────
    {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heapDesc.NumDescriptors = 2;
        _device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&_ssaoRtvHeap));

        UINT rtvSize = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        _ssaoRtv     = _ssaoRtvHeap->GetCPUDescriptorHandleForHeapStart();
        _ssaoBlurRtv = { _ssaoRtv.ptr + rtvSize };

        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
        rtvDesc.Format        = DXGI_FORMAT_R8_UNORM;
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        _device->CreateRenderTargetView(_ssaoRT.Get(),     &rtvDesc, _ssaoRtv);
        _device->CreateRenderTargetView(_ssaoBlurRT.Get(), &rtvDesc, _ssaoBlurRtv);
    }

    // ── 4. SRV slots for both SSAO textures ─────────────────────────────────
    {
        D3D12_CPU_DESCRIPTOR_HANDLE cpuH; D3D12_GPU_DESCRIPTOR_HANDLE gpuH;
        _ssaoSRVIndex = AllocateSRVSlot(cpuH, gpuH);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format                    = DXGI_FORMAT_R8_UNORM;
        srvDesc.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels       = 1;
        _device->CreateShaderResourceView(_ssaoRT.Get(), &srvDesc, cpuH);

        _ssaoBlurSRVIndex = AllocateSRVSlot(cpuH, gpuH);
        _device->CreateShaderResourceView(_ssaoBlurRT.Get(), &srvDesc, cpuH);
    }

    // ── 5. 4×4 noise texture (random rotation vectors for kernel jitter) ────
    {
        std::mt19937 rng(1337);
        std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

        // 16 × RG8_UNORM packed random xy
        static const int NOISE_SIZE = 4;
        struct NoisePixel { uint8_t r, g; };
        NoisePixel noiseData[NOISE_SIZE * NOISE_SIZE];
        for (auto& p : noiseData)
        {
            float rx = dist(rng) * 0.5f + 0.5f;
            float ry = dist(rng) * 0.5f + 0.5f;
            p.r = uint8_t(rx * 255.0f);
            p.g = uint8_t(ry * 255.0f);
        }

        D3D12_RESOURCE_DESC texDesc = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R8G8_UNORM, NOISE_SIZE, NOISE_SIZE);

        D3D12MA::ALLOCATION_DESC allocDesc = {};
        allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;
        _d3d12maAllocator->CreateResource(&allocDesc, &texDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            &_ssaoNoiseAlloc, IID_PPV_ARGS(&_ssaoNoiseTex));
        _ssaoNoiseTex->SetName(L"SSAO_Noise");

        // Upload via staging buffer
        const UINT64 uploadSize = GetRequiredIntermediateSize(_ssaoNoiseTex.Get(), 0, 1);
        ComPtr<ID3D12Resource> uploadBuf;
        D3D12MA::Allocation* uploadAlloc = nullptr;
        D3D12_RESOURCE_DESC uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadSize);
        D3D12MA::ALLOCATION_DESC uploadAllocDesc = {};
        uploadAllocDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;
        _d3d12maAllocator->CreateResource(&uploadAllocDesc, &uploadDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            &uploadAlloc, IID_PPV_ARGS(&uploadBuf));

        D3D12_SUBRESOURCE_DATA subData = {};
        subData.pData      = noiseData;
        subData.RowPitch   = NOISE_SIZE * sizeof(NoisePixel);
        subData.SlicePitch = subData.RowPitch * NOISE_SIZE;

        // Use a one-shot command list for the upload
        ComPtr<ID3D12CommandAllocator> tmpAlloc;
        ComPtr<ID3D12GraphicsCommandList> tmpCmd;
        _device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&tmpAlloc));
        _device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, tmpAlloc.Get(), nullptr, IID_PPV_ARGS(&tmpCmd));

        UpdateSubresources(tmpCmd.Get(), _ssaoNoiseTex.Get(), uploadBuf.Get(), 0, 0, 1, &subData);

        D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            _ssaoNoiseTex.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        tmpCmd->ResourceBarrier(1, &barrier);
        tmpCmd->Close();

        ID3D12CommandList* lists[] = { tmpCmd.Get() };
        _commandQueue->ExecuteCommandLists(1, lists);

        // Wait for upload to complete
        ComPtr<ID3D12Fence> fence;
        _device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        _commandQueue->Signal(fence.Get(), 1);
        HANDLE evt = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        fence->SetEventOnCompletion(1, evt);
        WaitForSingleObject(evt, INFINITE);
        CloseHandle(evt);

        if (uploadAlloc) uploadAlloc->Release();

        // SRV for noise
        D3D12_CPU_DESCRIPTOR_HANDLE cpuH; D3D12_GPU_DESCRIPTOR_HANDLE gpuH;
        _ssaoNoiseSRVIndex = AllocateSRVSlot(cpuH, gpuH);
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format                  = DXGI_FORMAT_R8G8_UNORM;
        srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels     = 1;
        _device->CreateShaderResourceView(_ssaoNoiseTex.Get(), &srvDesc, cpuH);
    }

    // ── 6. SSAO constant buffer (CPU-writable, persistently mapped) ─────────
    {
        const UINT64 cbSize = (sizeof(SSAOConstants) + 255) & ~255u;
        D3D12_RESOURCE_DESC cbDesc = CD3DX12_RESOURCE_DESC::Buffer(cbSize);
        D3D12MA::ALLOCATION_DESC allocDesc = {};
        allocDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;
        _d3d12maAllocator->CreateResource(&allocDesc, &cbDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            &_ssaoCBAlloc, IID_PPV_ARGS(&_ssaoCB));
        _ssaoCB->Map(0, nullptr, &_ssaoCBMapped);

        // Generate kernel once
        GenerateSSAOKernel(_ssaoKernel.samples, SSAO_SAMPLE_COUNT);
        _ssaoKernel.radius     = 0.5f;
        _ssaoKernel.bias       = 0.025f;
        _ssaoKernel.noiseScale = XMFLOAT2(float(halfW) / 4.0f, float(halfH) / 4.0f);
    }

    // ── 7. SSAO pipeline ─────────────────────────────────────────────────────
    _ssaoPipeline = std::make_unique<DX12Pipeline>();
    {
        PipelineStateDesc desc;
        desc.rootLayout       = RootSignatureLayout::SSAO;
        desc.noInputLayout    = true;
        desc.numRenderTargets = 1;
        desc.rtvFormats[0]    = DXGI_FORMAT_R8_UNORM;
        if (!_ssaoPipeline->Initialize(_device, L"fullscreen.vert.hlsl", L"ssao.frag.hlsl", desc))
        {
            LUNA_LOG_ERROR("SSAO pipeline init failed");
            return false;
        }
    }

    // ── 8. SSAO blur pipeline ────────────────────────────────────────────────
    _ssaoBlurPipeline = std::make_unique<DX12Pipeline>();
    {
        PipelineStateDesc desc;
        desc.rootLayout       = RootSignatureLayout::SSAOBlur;
        desc.noInputLayout    = true;
        desc.numRenderTargets = 1;
        desc.rtvFormats[0]    = DXGI_FORMAT_R8_UNORM;
        if (!_ssaoBlurPipeline->Initialize(_device, L"fullscreen.vert.hlsl", L"ssao_blur.frag.hlsl", desc))
        {
            LUNA_LOG_ERROR("SSAO blur pipeline init failed");
            return false;
        }
    }

    LUNA_LOG_INFO("SSAO resources created (%ux%u, %d samples)", halfW, halfH, SSAO_SAMPLE_COUNT);
    return true;
}

void DX12Backend::DestroySSAOResources()
{
    if (_ssaoCBMapped) { _ssaoCB->Unmap(0, nullptr); _ssaoCBMapped = nullptr; }
    if (_ssaoCBAlloc)      { _ssaoCBAlloc->Release();      _ssaoCBAlloc      = nullptr; }
    if (_ssaoRTAlloc)      { _ssaoRTAlloc->Release();      _ssaoRTAlloc      = nullptr; }
    if (_ssaoBlurRTAlloc)  { _ssaoBlurRTAlloc->Release();  _ssaoBlurRTAlloc  = nullptr; }
    if (_ssaoNoiseAlloc)   { _ssaoNoiseAlloc->Release();   _ssaoNoiseAlloc   = nullptr; }
    _ssaoRT.Reset(); _ssaoBlurRT.Reset(); _ssaoNoiseTex.Reset(); _ssaoCB.Reset();
    _ssaoPipeline.reset(); _ssaoBlurPipeline.reset();
    _ssaoRtvHeap.Reset();
}

void DX12Backend::DrawSSAOPass()
{
    if (!_ssaoPipeline || !_ssaoRT) return;

    auto& frame = _frames[_frameIndex];
    auto* cmd   = _commandList.Get();

    // Update CB: set current proj/view matrices
    XMMATRIX proj    = XMLoadFloat4x4(&_lastProj);
    XMMATRIX view    = XMLoadFloat4x4(&_lastView);
    XMMATRIX invProj = XMMatrixInverse(nullptr, proj);
    XMStoreFloat4x4(&_ssaoKernel.projection,    proj);
    XMStoreFloat4x4(&_ssaoKernel.invProjection, invProj);
    XMStoreFloat4x4(&_ssaoKernel.view,          view);

    const UINT halfW = std::max(1u, (UINT)_screenWidth  / 2);
    const UINT halfH = std::max(1u, (UINT)_screenHeight / 2);
    _ssaoKernel.noiseScale = XMFLOAT2(float(halfW) / 4.0f, float(halfH) / 4.0f);
    memcpy(_ssaoCBMapped, &_ssaoKernel, sizeof(SSAOConstants));

    // Transition depth/GB1 to SRV (already done in DrawFrame's render graph)
    // _ssaoRT is already in RENDER_TARGET from creation / previous frame transition

    cmd->SetPipelineState(_ssaoPipeline->GetPSO());
    cmd->SetGraphicsRootSignature(_ssaoPipeline->GetRootSignature().Get());
    cmd->SetDescriptorHeaps(1, _imGuiSrvHeap.GetAddressOf());

    D3D12_VIEWPORT vp = {0, 0, float(halfW), float(halfH), 0, 1};
    D3D12_RECT     sc = {0, 0, LONG(halfW),  LONG(halfH)};
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);
    cmd->OMSetRenderTargets(1, &_ssaoRtv, FALSE, nullptr);

    FLOAT clearColor[4] = {1.0f, 0, 0, 0};
    cmd->ClearRenderTargetView(_ssaoRtv, clearColor, 0, nullptr);

    // params[0] = SSAOConstants CBV
    cmd->SetGraphicsRootConstantBufferView(0, _ssaoCB->GetGPUVirtualAddress());

    // params[1] = depth SRV  (t0)
    UINT srvDescSize = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto baseGpu = _imGuiSrvHeap->GetGPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE depthGpu  = { baseGpu.ptr + _depthSRVIndex  * srvDescSize };
    D3D12_GPU_DESCRIPTOR_HANDLE normalGpu = { baseGpu.ptr + _gbufferSRVIndex[1] * srvDescSize };
    D3D12_GPU_DESCRIPTOR_HANDLE noiseGpu  = { baseGpu.ptr + _ssaoNoiseSRVIndex  * srvDescSize };

    cmd->SetGraphicsRootDescriptorTable(1, depthGpu);
    cmd->SetGraphicsRootDescriptorTable(2, normalGpu);
    cmd->SetGraphicsRootDescriptorTable(3, noiseGpu);

    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(3, 1, 0, 0);   // fullscreen triangle
}

void DX12Backend::DrawSSAOBlurPass()
{
    if (!_ssaoBlurPipeline || !_ssaoBlurRT) return;

    auto* cmd = _commandList.Get();

    const UINT halfW = std::max(1u, (UINT)_screenWidth  / 2);
    const UINT halfH = std::max(1u, (UINT)_screenHeight / 2);

    // Transition raw SSAO RT → SRV, blur target → RTV
    D3D12_RESOURCE_BARRIER barriers[2];
    barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(
        _ssaoRT.Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    barriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(
        _ssaoBlurRT.Get(),
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmd->ResourceBarrier(2, barriers);

    cmd->SetPipelineState(_ssaoBlurPipeline->GetPSO());
    cmd->SetGraphicsRootSignature(_ssaoBlurPipeline->GetRootSignature().Get());
    cmd->SetDescriptorHeaps(1, _imGuiSrvHeap.GetAddressOf());

    D3D12_VIEWPORT vp = {0, 0, float(halfW), float(halfH), 0, 1};
    D3D12_RECT     sc = {0, 0, LONG(halfW),  LONG(halfH)};
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);
    cmd->OMSetRenderTargets(1, &_ssaoBlurRtv, FALSE, nullptr);

    FLOAT clearColor[4] = {1.0f, 0, 0, 0};
    cmd->ClearRenderTargetView(_ssaoBlurRtv, clearColor, 0, nullptr);

    // params[0] = raw SSAO SRV (t0)
    UINT srvDescSize = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto baseGpu = _imGuiSrvHeap->GetGPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE ssaoGpu = { baseGpu.ptr + _ssaoSRVIndex * srvDescSize };
    cmd->SetGraphicsRootDescriptorTable(0, ssaoGpu);

    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(3, 1, 0, 0);

    // Transition blur result back to SRV for deferred lighting
    D3D12_RESOURCE_BARRIER toSrv[2];
    toSrv[0] = CD3DX12_RESOURCE_BARRIER::Transition(
        _ssaoRT.Get(),
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    toSrv[1] = CD3DX12_RESOURCE_BARRIER::Transition(
        _ssaoBlurRT.Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(2, toSrv);
}

// ===========================================================================
// Phase 10: Post-process stack (TAA + Bloom + ACES tone mapping)
// ===========================================================================

// ---------------------------------------------------------------------------
// Allocate one DEFAULT-heap RT + RTV + SRV slot.
// Returns false on any failure; caller cleans up via DestroyPostProcessResources.
// ---------------------------------------------------------------------------
static bool AllocPPTarget(
    ID3D12Device*               device,
    D3D12MA::Allocator*         allocator,
    UINT                        width,
    UINT                        height,
    DXGI_FORMAT                 fmt,
    const wchar_t*              name,
    D3D12MA::Allocation**       ppAlloc,
    ComPtr<ID3D12Resource>&     resource,
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle,
    UINT&                       srvIndex,
    DX12Backend*                backend)
{
    D3D12_CLEAR_VALUE cv = {};
    cv.Format = fmt;   // colour[0..3] = 0 (black)

    D3D12MA::ALLOCATION_DESC ad = {};
    ad.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC rd = CD3DX12_RESOURCE_DESC::Tex2D(
        fmt, width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);

    HRESULT hr = allocator->CreateResource(
        &ad, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
        ppAlloc, IID_PPV_ARGS(&resource));
    if (FAILED(hr)) return false;
    resource->SetName(name);

    D3D12_RENDER_TARGET_VIEW_DESC rvd = {};
    rvd.Format        = fmt;
    rvd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(resource.Get(), &rvd, rtvHandle);

    D3D12_CPU_DESCRIPTOR_HANDLE cpuH;
    D3D12_GPU_DESCRIPTOR_HANDLE gpuH;
    srvIndex = backend->AllocateSRVSlot(cpuH, gpuH);

    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format                    = fmt;
    sd.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels       = 1;
    device->CreateShaderResourceView(resource.Get(), &sd, cpuH);
    return true;
}

bool DX12Backend::CreatePostProcessResources()
{
    const UINT W     = (UINT)_screenWidth;
    const UINT H     = (UINT)_screenHeight;
    const UINT halfW = std::max(1u, W / 2);
    const UINT halfH = std::max(1u, H / 2);

    // ── 0. RTV heap: [HDR, hist0, hist1, bloomBright, bloomBlur] ─────────────
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 5;
        if (FAILED(_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&_ppRtvHeap))))
        {
            LUNA_LOG_ERROR("Phase 10: PP RTV heap creation failed");
            return false;
        }
    }
    UINT     rtvSz = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    SIZE_T   base  = _ppRtvHeap->GetCPUDescriptorHandleForHeapStart().ptr;
    _hdrRTV             = { base + 0 * rtvSz };
    _taaHistoryRTV[0]   = { base + 1 * rtvSz };
    _taaHistoryRTV[1]   = { base + 2 * rtvSz };
    _bloomBrightRTV     = { base + 3 * rtvSz };
    _bloomBlurRTV       = { base + 4 * rtvSz };

    // ── 1. HDR intermediate RT (R16G16B16A16_FLOAT, full-res) ────────────────
    if (!AllocPPTarget(_device.Get(), _d3d12maAllocator,
            W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, L"PP_HDR",
            &_hdrRTAlloc, _hdrRT, _hdrRTV, _hdrSRVIndex, this))
    { LUNA_LOG_ERROR("Phase 10: HDR RT alloc failed"); return false; }

    // ── 2. TAA history ping-pong (R16G16B16A16_FLOAT, full-res × 2) ──────────
    const wchar_t* histNames[2] = { L"TAA_Hist0", L"TAA_Hist1" };
    for (int i = 0; i < 2; ++i)
    {
        if (!AllocPPTarget(_device.Get(), _d3d12maAllocator,
                W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, histNames[i],
                &_taaHistoryAlloc[i], _taaHistory[i],
                _taaHistoryRTV[i], _taaHistorySRVIndex[i], this))
        { LUNA_LOG_ERROR("Phase 10: TAA history alloc failed (idx %d)", i); return false; }
    }

    // ── 3. Bloom half-res buffers (R11G11B10_FLOAT) ───────────────────────────
    if (!AllocPPTarget(_device.Get(), _d3d12maAllocator,
            halfW, halfH, DXGI_FORMAT_R11G11B10_FLOAT, L"Bloom_Bright",
            &_bloomBrightAlloc, _bloomBrightRT, _bloomBrightRTV, _bloomBrightSRVIndex, this))
    { LUNA_LOG_ERROR("Phase 10: Bloom bright RT alloc failed"); return false; }

    if (!AllocPPTarget(_device.Get(), _d3d12maAllocator,
            halfW, halfH, DXGI_FORMAT_R11G11B10_FLOAT, L"Bloom_Blur",
            &_bloomBlurAlloc, _bloomBlurRT, _bloomBlurRTV, _bloomBlurSRVIndex, this))
    { LUNA_LOG_ERROR("Phase 10: Bloom blur RT alloc failed"); return false; }

    // ── 4. Per-frame TAA constant buffers ─────────────────────────────────────
    const UINT cbSz = (sizeof(TAAConstants) + 255) & ~255u;
    for (UINT i = 0; i < FRAMES_IN_FLIGHT; ++i)
    {
        D3D12MA::ALLOCATION_DESC ad = {}; ad.HeapType = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC      cd = CD3DX12_RESOURCE_DESC::Buffer(cbSz);
        if (FAILED(_d3d12maAllocator->CreateResource(&ad, &cd,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                &_taaCBAlloc[i], IID_PPV_ARGS(&_taaCB[i]))))
        { LUNA_LOG_ERROR("Phase 10: TAA CB alloc failed (frame %u)", i); return false; }
        _taaCB[i]->Map(0, nullptr, &_taaCBMapped[i]);
    }

    // ── 5. Lighting HDR pipeline (DeferredLighting root sig, float16 output) ──
    _lightingPipelineHDR = std::make_unique<DX12Pipeline>();
    {
        PipelineStateDesc d;
        d.rootLayout       = RootSignatureLayout::DeferredLighting;
        d.noInputLayout    = true;
        d.numRenderTargets = 1;
        d.rtvFormats[0]    = DXGI_FORMAT_R16G16B16A16_FLOAT;
        if (!_lightingPipelineHDR->Initialize(_device,
                L"fullscreen.vert.hlsl", L"deferred_lighting_hdr.frag.hlsl", d))
        { LUNA_LOG_ERROR("Phase 10: lightingPipelineHDR init failed"); return false; }
    }

    // ── 6. TAA pipeline ───────────────────────────────────────────────────────
    _taaPipeline = std::make_unique<DX12Pipeline>();
    {
        PipelineStateDesc d;
        d.rootLayout       = RootSignatureLayout::TAA;
        d.noInputLayout    = true;
        d.numRenderTargets = 1;
        d.rtvFormats[0]    = DXGI_FORMAT_R16G16B16A16_FLOAT;
        if (!_taaPipeline->Initialize(_device, L"fullscreen.vert.hlsl", L"taa.frag.hlsl", d))
        { LUNA_LOG_ERROR("Phase 10: TAA pipeline init failed"); return false; }
    }

    // ── 7. Bloom bright pipeline ──────────────────────────────────────────────
    _bloomBrightPipeline = std::make_unique<DX12Pipeline>();
    {
        PipelineStateDesc d;
        d.rootLayout       = RootSignatureLayout::BloomBright;
        d.noInputLayout    = true;
        d.numRenderTargets = 1;
        d.rtvFormats[0]    = DXGI_FORMAT_R11G11B10_FLOAT;
        if (!_bloomBrightPipeline->Initialize(_device,
                L"fullscreen.vert.hlsl", L"bloom_bright.frag.hlsl", d))
        { LUNA_LOG_ERROR("Phase 10: Bloom bright pipeline init failed"); return false; }
    }

    // ── 8. Bloom blur pipeline (shared for H and V passes) ───────────────────
    _bloomBlurPipeline = std::make_unique<DX12Pipeline>();
    {
        PipelineStateDesc d;
        d.rootLayout       = RootSignatureLayout::BloomBlur;
        d.noInputLayout    = true;
        d.numRenderTargets = 1;
        d.rtvFormats[0]    = DXGI_FORMAT_R11G11B10_FLOAT;
        if (!_bloomBlurPipeline->Initialize(_device,
                L"fullscreen.vert.hlsl", L"bloom_blur.frag.hlsl", d))
        { LUNA_LOG_ERROR("Phase 10: Bloom blur pipeline init failed"); return false; }
    }

    // ── 9. Tone mapping pipeline (LDR back buffer output) ────────────────────
    _toneMappingPipeline = std::make_unique<DX12Pipeline>();
    {
        PipelineStateDesc d;
        d.rootLayout       = RootSignatureLayout::ToneMap;
        d.noInputLayout    = true;
        d.numRenderTargets = 1;
        d.rtvFormats[0]    = DXGI_FORMAT_R8G8B8A8_UNORM;
        if (!_toneMappingPipeline->Initialize(_device,
                L"fullscreen.vert.hlsl", L"tonemapping.frag.hlsl", d))
        { LUNA_LOG_ERROR("Phase 10: Tone mapping pipeline init failed"); return false; }
    }

    _ppResourcesValid = true;
    LUNA_LOG_INFO("Phase 10 post-process stack created (%ux%u HDR, %ux%u bloom)",
                  W, H, halfW, halfH);

    // Phase 16B: SSR (optional — failure doesn't block the rest of PP)
    if (!CreateSSRResources())
        LUNA_LOG_ERROR("Phase 16B: SSR disabled — CreateSSRResources failed");

    // Phase 18B: Motion blur (optional — failure doesn't block SSR/TAA/Bloom)
    if (!CreateMotionBlurResources())
        LUNA_LOG_ERROR("Phase 18B: Motion blur disabled — CreateMotionBlurResources failed");

    return true;
}

void DX12Backend::DestroyPostProcessResources()
{
    _ppResourcesValid = false;
    DestroyMotionBlurResources();  // Phase 18B
    DestroySSRResources();         // Phase 16B

    for (UINT i = 0; i < FRAMES_IN_FLIGHT; ++i)
    {
        if (_taaCBMapped[i]) { _taaCB[i]->Unmap(0, nullptr); _taaCBMapped[i] = nullptr; }
        if (_taaCBAlloc[i])  { _taaCBAlloc[i]->Release();    _taaCBAlloc[i]  = nullptr; }
        _taaCB[i].Reset();
    }
    for (int i = 0; i < 2; ++i)
    {
        if (_taaHistoryAlloc[i]) { _taaHistoryAlloc[i]->Release(); _taaHistoryAlloc[i] = nullptr; }
        _taaHistory[i].Reset();
        _taaHistorySRVIndex[i] = UINT_MAX;
    }
    if (_hdrRTAlloc)       { _hdrRTAlloc->Release();       _hdrRTAlloc       = nullptr; }
    if (_bloomBrightAlloc) { _bloomBrightAlloc->Release(); _bloomBrightAlloc = nullptr; }
    if (_bloomBlurAlloc)   { _bloomBlurAlloc->Release();   _bloomBlurAlloc   = nullptr; }
    _hdrRT.Reset();
    _bloomBrightRT.Reset();
    _bloomBlurRT.Reset();
    _hdrSRVIndex = _bloomBrightSRVIndex = _bloomBlurSRVIndex = UINT_MAX;
    _ppRtvHeap.Reset();

    _lightingPipelineHDR.reset();
    _taaPipeline.reset();
    _bloomBrightPipeline.reset();
    _bloomBlurPipeline.reset();
    _toneMappingPipeline.reset();
}

// ---------------------------------------------------------------------------
// Internal helper: bind a fullscreen pipeline, set viewport/scissor, bind RTV
// ---------------------------------------------------------------------------
static void BindPPPipeline(
    ID3D12GraphicsCommandList*  cmd,
    DX12Pipeline*               pipe,
    ComPtr<ID3D12DescriptorHeap>& heap,
    D3D12_CPU_DESCRIPTOR_HANDLE rtv,
    UINT                        vpW,
    UINT                        vpH)
{
    cmd->SetPipelineState(pipe->GetPSO());
    cmd->SetGraphicsRootSignature(pipe->GetRootSignature().Get());
    cmd->SetDescriptorHeaps(1, heap.GetAddressOf());
    cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT vp = {0, 0, float(vpW), float(vpH), 0.0f, 1.0f};
    D3D12_RECT     sc = {0, 0, LONG(vpW),  LONG(vpH)};
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

// ---------------------------------------------------------------------------
// Internal helper: GPU descriptor handle from a SRV slot index
// ---------------------------------------------------------------------------
static inline D3D12_GPU_DESCRIPTOR_HANDLE PPSRVHandle(
    ComPtr<ID3D12DescriptorHeap>& heap, UINT index, UINT stride)
{
    return { heap->GetGPUDescriptorHandleForHeapStart().ptr + UINT64(index) * stride };
}

// ---------------------------------------------------------------------------
// Phase 10: TAA resolve pass
// Entry:  _hdrRT=RT, _taaHistory[read]=RT, _depthBuffer=DEPTH_WRITE
// Exit:   _hdrRT=RT, _taaHistory[read]=RT, _depthBuffer=DEPTH_WRITE
//         _taaHistory[write]=RT (bloom bright will read it next)
// ---------------------------------------------------------------------------
void DX12Backend::DrawTAAPass()
{
    if (!_taaPipeline || !_hdrRT || !_taaHistory[0] || !_taaHistory[1]) return;

    auto* cmd    = _commandList.Get();
    int   write  = _taaHistoryIndex;
    int   read   = 1 - _taaHistoryIndex;
    // Phase 18B: if motion blur ran, it already transitioned _hdrRT to SRV
    // and its output is in _motionBlurRT (also SRV). Feed motionBlur to TAA as currentFrame.
    bool  hasMB  = (_motionBlurPipeline && _motionBlurSRVIndex != UINT_MAX && _motionBlurRT);
    UINT  curSRV = hasMB ? _motionBlurSRVIndex : _hdrSRVIndex;
    const D3D12_RESOURCE_STATES SRV_BOTH = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                                         | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    D3D12_RESOURCE_BARRIER pre[3];
    UINT nPre = 0;
    if (!hasMB) // hdrRT already in SRV from DrawMotionBlurPass; skip redundant transition
        pre[nPre++] = CD3DX12_RESOURCE_BARRIER::Transition(
            _hdrRT.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, SRV_BOTH);
    pre[nPre++] = CD3DX12_RESOURCE_BARRIER::Transition(
        _taaHistory[read].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, SRV_BOTH);
    pre[nPre++] = CD3DX12_RESOURCE_BARRIER::Transition(
        _depthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, SRV_BOTH);
    cmd->ResourceBarrier(nPre, pre);

    BindPPPipeline(cmd, _taaPipeline.get(), _imGuiSrvHeap,
                   _taaHistoryRTV[write], _screenWidth, _screenHeight);

    cmd->SetGraphicsRootConstantBufferView(0, _taaCB[_frameIndex]->GetGPUVirtualAddress());
    cmd->SetGraphicsRootDescriptorTable(1, PPSRVHandle(_imGuiSrvHeap, curSRV,                    _srvDescriptorSize));
    cmd->SetGraphicsRootDescriptorTable(2, PPSRVHandle(_imGuiSrvHeap, _taaHistorySRVIndex[read], _srvDescriptorSize));
    cmd->SetGraphicsRootDescriptorTable(3, PPSRVHandle(_imGuiSrvHeap, _depthSRVIndex,            _srvDescriptorSize));
    cmd->DrawInstanced(3, 1, 0, 0);

    D3D12_RESOURCE_BARRIER post[3];
    UINT nPost = 0;
    if (!hasMB) // restore hdrRT only when we transitioned it here
        post[nPost++] = CD3DX12_RESOURCE_BARRIER::Transition(
            _hdrRT.Get(), SRV_BOTH, D3D12_RESOURCE_STATE_RENDER_TARGET);
    post[nPost++] = CD3DX12_RESOURCE_BARRIER::Transition(
        _taaHistory[read].Get(), SRV_BOTH, D3D12_RESOURCE_STATE_RENDER_TARGET);
    post[nPost++] = CD3DX12_RESOURCE_BARRIER::Transition(
        _depthBuffer.Get(), SRV_BOTH, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    cmd->ResourceBarrier(nPost, post);
    // _taaHistory[write] stays in RENDER_TARGET — bloom bright reads it next
}

// ---------------------------------------------------------------------------
// Phase 10: Bloom bright-pass (threshold extract, half-res)
// Entry:  _taaHistory[write]=RT
// Exit:   _taaHistory[write]=PSR (tone map will also read it), _bloomBrightRT=RT
// ---------------------------------------------------------------------------
void DX12Backend::DrawBloomBrightPass()
{
    if (!_bloomBrightPipeline || !_bloomBrightRT) return;

    auto* cmd   = _commandList.Get();
    int   write = _taaHistoryIndex;

    D3D12_RESOURCE_BARRIER pre = CD3DX12_RESOURCE_BARRIER::Transition(
        _taaHistory[write].Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(1, &pre);

    const UINT halfW = std::max(1u, (UINT)_screenWidth  / 2);
    const UINT halfH = std::max(1u, (UINT)_screenHeight / 2);
    BindPPPipeline(cmd, _bloomBrightPipeline.get(), _imGuiSrvHeap,
                   _bloomBrightRTV, halfW, halfH);

    float consts[4] = {1.0f, 0.1f, 0.0f, 0.0f};  // threshold=1.0, knee=0.1
    cmd->SetGraphicsRoot32BitConstants(0, 4, consts, 0);
    cmd->SetGraphicsRootDescriptorTable(1, PPSRVHandle(_imGuiSrvHeap,
        _taaHistorySRVIndex[write], _srvDescriptorSize));
    cmd->DrawInstanced(3, 1, 0, 0);
    // _taaHistory[write] stays PSR; _bloomBrightRT stays RT
}

// ---------------------------------------------------------------------------
// Phase 10: Bloom Gaussian blur (H then V pass, same pipeline)
// H (horizontal=true):
//   Entry:  _bloomBrightRT=RT, _bloomBlurRT=RT
//   Exit:   _bloomBrightRT=PSR, _bloomBlurRT=RT
// V (horizontal=false):
//   Entry:  _bloomBlurRT=RT, _bloomBrightRT=PSR
//   Exit:   _bloomBlurRT=PSR, _bloomBrightRT=RT (final blurred bloom)
// ---------------------------------------------------------------------------
void DX12Backend::DrawBloomBlurPass(bool horizontal)
{
    if (!_bloomBlurPipeline) return;

    auto*      cmd   = _commandList.Get();
    const UINT halfW = std::max(1u, (UINT)_screenWidth  / 2);
    const UINT halfH = std::max(1u, (UINT)_screenHeight / 2);

    ID3D12Resource*             inputRes;
    UINT                        inputSRV;
    D3D12_CPU_DESCRIPTOR_HANDLE outputRTV;

    D3D12_RESOURCE_BARRIER bars[2];
    UINT nBars = 0;

    if (horizontal)
    {
        inputRes  = _bloomBrightRT.Get();
        inputSRV  = _bloomBrightSRVIndex;
        outputRTV = _bloomBlurRTV;
        bars[nBars++] = CD3DX12_RESOURCE_BARRIER::Transition(inputRes,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    else
    {
        inputRes  = _bloomBlurRT.Get();
        inputSRV  = _bloomBlurSRVIndex;
        outputRTV = _bloomBrightRTV;
        bars[nBars++] = CD3DX12_RESOURCE_BARRIER::Transition(_bloomBlurRT.Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        bars[nBars++] = CD3DX12_RESOURCE_BARRIER::Transition(_bloomBrightRT.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    cmd->ResourceBarrier(nBars, bars);

    BindPPPipeline(cmd, _bloomBlurPipeline.get(), _imGuiSrvHeap, outputRTV, halfW, halfH);

    float consts[4] = {
        horizontal ? 1.0f / float(halfW) : 0.0f,
        horizontal ? 0.0f                : 1.0f / float(halfH),
        0.0f, 0.0f
    };
    cmd->SetGraphicsRoot32BitConstants(0, 4, consts, 0);
    cmd->SetGraphicsRootDescriptorTable(1, PPSRVHandle(_imGuiSrvHeap, inputSRV, _srvDescriptorSize));
    cmd->DrawInstanced(3, 1, 0, 0);
    // input stays PSR; output stays RT — restored in DrawToneMappingPass
}

// ---------------------------------------------------------------------------
// Phase 10: ACES tone mapping + bloom composite → LDR back buffer
// Entry:  _taaHistory[write]=PSR, _bloomBrightRT=RT (final V-blur result)
// Exit:   back buffer=RT (for ImGui)
//         all PP buffers restored to RENDER_TARGET (REST state for next frame)
// ---------------------------------------------------------------------------
void DX12Backend::DrawToneMappingPass(UINT backIdx)
{
    if (!_toneMappingPipeline) return;

    auto* cmd   = _commandList.Get();
    int   write = _taaHistoryIndex;

    // Final bloom (bloomBrightRT) is in RT — transition to PSR for reading
    D3D12_RESOURCE_BARRIER pre = CD3DX12_RESOURCE_BARRIER::Transition(
        _bloomBrightRT.Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(1, &pre);

    BindPPPipeline(cmd, _toneMappingPipeline.get(), _imGuiSrvHeap,
                   _rtvHandle[backIdx], _screenWidth, _screenHeight);

    float consts[4] = {0.04f, 1.0f, 0.0f, 0.0f};  // bloomStrength=0.04, exposure=1.0
    cmd->SetGraphicsRoot32BitConstants(0, 4, consts, 0);
    // t0: resolved HDR (_taaHistory[write] is still PSR from DrawBloomBrightPass)
    cmd->SetGraphicsRootDescriptorTable(1, PPSRVHandle(_imGuiSrvHeap,
        _taaHistorySRVIndex[write], _srvDescriptorSize));
    // t1: blurred bloom (_bloomBrightRT now PSR, holds final V-blur result)
    cmd->SetGraphicsRootDescriptorTable(2, PPSRVHandle(_imGuiSrvHeap,
        _bloomBrightSRVIndex, _srvDescriptorSize));
    cmd->DrawInstanced(3, 1, 0, 0);

    // Restore all PP buffers to REST (RENDER_TARGET) for next frame
    D3D12_RESOURCE_BARRIER post[3] = {
        CD3DX12_RESOURCE_BARRIER::Transition(_taaHistory[write].Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
        CD3DX12_RESOURCE_BARRIER::Transition(_bloomBrightRT.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
        CD3DX12_RESOURCE_BARRIER::Transition(_bloomBlurRT.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
    };
    cmd->ResourceBarrier(3, post);
    // Back buffer stays RENDER_TARGET — ImGui renders next
}

// ---------------------------------------------------------------------------
// Phase 12: GPU-driven rendering — indirect draw + GPU frustum culling
// ---------------------------------------------------------------------------

// ViewProj CB layout — matches b0 in pbr_indirect.vert.hlsl
struct ViewProjCB
{
    XMFLOAT4X4 view;
    XMFLOAT4X4 proj;
};

void DX12Backend::ExtractFrustumPlanes(const XMFLOAT4X4& view, const XMFLOAT4X4& proj,
                                        XMFLOAT4 planes[6])
{
    // Compute VP = view * proj (row-major DirectXMath convention)
    XMMATRIX V  = XMLoadFloat4x4(&view);
    XMMATRIX P  = XMLoadFloat4x4(&proj);
    XMMATRIX VP = XMMatrixMultiply(V, P);

    // Extract Gribb-Hartmann frustum planes from the composite matrix.
    // For row-major where clip = world * VP, we extract from columns.
    // Column N in XMFLOAT4X4 is (_1N, _2N, _3N, _4N)
    XMFLOAT4X4 m;
    XMStoreFloat4x4(&m, VP);

    // Left:   col3 + col0 (x + w >= 0)
    planes[0] = { m._14 + m._11, m._24 + m._21, m._34 + m._31, m._44 + m._41 };
    // Right:  col3 - col0 (w - x >= 0)
    planes[1] = { m._14 - m._11, m._24 - m._21, m._34 - m._31, m._44 - m._41 };
    // Bottom: col3 + col1 (y + w >= 0)
    planes[2] = { m._14 + m._12, m._24 + m._22, m._34 + m._32, m._44 + m._42 };
    // Top:    col3 - col1 (w - y >= 0)
    planes[3] = { m._14 - m._12, m._24 - m._22, m._34 - m._32, m._44 - m._42 };
    // Near:   col2 (z >= 0 for DX left-handed)
    planes[4] = { m._13, m._23, m._33, m._43 };
    // Far:    col3 - col2 (w - z >= 0)
    planes[5] = { m._14 - m._13, m._24 - m._23, m._34 - m._33, m._44 - m._43 };

    // Normalise each plane
    for (int i = 0; i < 6; ++i)
    {
        XMVECTOR p = XMLoadFloat4(&planes[i]);
        XMVECTOR n = XMVector3Length(p);
        p = XMVectorDivide(p, n);
        XMStoreFloat4(&planes[i], p);
    }
}

void DX12Backend::BuildMergedGeometry()
{
    if (_sceneMeshes.empty()) return;

    WaitAllFrames();

    // Collect all vertices and indices into contiguous arrays
    std::vector<PBRVertex> allVertices;
    std::vector<uint32_t>  allIndices;
    std::vector<MeshDrawInfo> meshInfos;

    for (size_t i = 0; i < _sceneMeshes.size(); ++i)
    {
        const auto& mesh = _sceneMeshes[i];
        MeshDrawInfo mi;
        mi.indexCount   = mesh->indexCount;
        mi.firstIndex   = static_cast<UINT>(allIndices.size());
        mi.vertexOffset = static_cast<INT>(allVertices.size());
        mi._pad         = 0;
        meshInfos.push_back(mi);

        // Read back vertex data from GPU (they're in DEFAULT heap — need readback)
        // For simplicity, re-read from the mesh's existing buffers.
        // Since we know the sizes, create readback buffers.
        UINT vbSize = mesh->vbView.SizeInBytes;
        UINT ibSize = mesh->ibView.SizeInBytes;
        UINT vertCount = vbSize / sizeof(PBRVertex);
        UINT idxCount  = ibSize / sizeof(uint32_t);

        // Create readback buffers
        ComPtr<ID3D12Resource> vbReadback, ibReadback;
        D3D12_HEAP_PROPERTIES readbackHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
        D3D12_RESOURCE_DESC bufDesc = CD3DX12_RESOURCE_DESC::Buffer(vbSize);
        _device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&vbReadback));
        bufDesc = CD3DX12_RESOURCE_DESC::Buffer(ibSize);
        _device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&ibReadback));

        // Copy from DEFAULT→READBACK
        _frames[0].cmdAllocator->Reset();
        _commandList->Reset(_frames[0].cmdAllocator.Get(), nullptr);

        D3D12_RESOURCE_BARRIER pre[2] = {
            CD3DX12_RESOURCE_BARRIER::Transition(mesh->vertexBuffer.Get(),
                D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_COPY_SOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(mesh->indexBuffer.Get(),
                D3D12_RESOURCE_STATE_INDEX_BUFFER, D3D12_RESOURCE_STATE_COPY_SOURCE),
        };
        _commandList->ResourceBarrier(2, pre);
        _commandList->CopyResource(vbReadback.Get(), mesh->vertexBuffer.Get());
        _commandList->CopyResource(ibReadback.Get(), mesh->indexBuffer.Get());
        D3D12_RESOURCE_BARRIER post[2] = {
            CD3DX12_RESOURCE_BARRIER::Transition(mesh->vertexBuffer.Get(),
                D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER),
            CD3DX12_RESOURCE_BARRIER::Transition(mesh->indexBuffer.Get(),
                D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_INDEX_BUFFER),
        };
        _commandList->ResourceBarrier(2, post);
        _commandList->Close();
        ID3D12CommandList* lists[] = { _commandList.Get() };
        _commandQueue->ExecuteCommandLists(1, lists);
        ++_globalFenceValue;
        _commandQueue->Signal(_fence.Get(), _globalFenceValue);
        _frames[0].fenceValue = _globalFenceValue;
        WaitForFrame(0);

        // Map and copy
        void* vbData = nullptr;
        void* ibData = nullptr;
        D3D12_RANGE range = { 0, vbSize };
        vbReadback->Map(0, &range, &vbData);
        range = { 0, ibSize };
        ibReadback->Map(0, &range, &ibData);

        size_t baseVert = allVertices.size();
        allVertices.resize(baseVert + vertCount);
        memcpy(&allVertices[baseVert], vbData, vbSize);
        size_t baseIdx = allIndices.size();
        allIndices.resize(baseIdx + idxCount);
        memcpy(&allIndices[baseIdx], ibData, ibSize);

        D3D12_RANGE noWrite = { 0, 0 };
        vbReadback->Unmap(0, &noWrite);
        ibReadback->Unmap(0, &noWrite);
    }

    // Upload merged VB
    _frames[0].cmdAllocator->Reset();
    _commandList->Reset(_frames[0].cmdAllocator.Get(), nullptr);

    ComPtr<ID3D12Resource> vbStaging, ibStaging;
    D3D12MA::Allocation* vbStagingAlloc = nullptr;
    D3D12MA::Allocation* ibStagingAlloc = nullptr;

    _mergedVB = MeshLoader::UploadBuffer(
        _d3d12maAllocator, _commandList.Get(),
        allVertices.data(), allVertices.size() * sizeof(PBRVertex),
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
        &_mergedVBAlloc, vbStaging, &vbStagingAlloc);

    _mergedIB = MeshLoader::UploadBuffer(
        _d3d12maAllocator, _commandList.Get(),
        allIndices.data(), allIndices.size() * sizeof(uint32_t),
        D3D12_RESOURCE_STATE_INDEX_BUFFER,
        &_mergedIBAlloc, ibStaging, &ibStagingAlloc);

    _mergedVBView.BufferLocation = _mergedVB->GetGPUVirtualAddress();
    _mergedVBView.SizeInBytes    = static_cast<UINT>(allVertices.size() * sizeof(PBRVertex));
    _mergedVBView.StrideInBytes  = sizeof(PBRVertex);

    _mergedIBView.BufferLocation = _mergedIB->GetGPUVirtualAddress();
    _mergedIBView.SizeInBytes    = static_cast<UINT>(allIndices.size() * sizeof(uint32_t));
    _mergedIBView.Format         = DXGI_FORMAT_R32_UINT;

    // Upload MeshDrawInfo SSBO
    ComPtr<ID3D12Resource> miStaging;
    D3D12MA::Allocation* miStagingAlloc = nullptr;
    _meshInfoBuffer = MeshLoader::UploadBuffer(
        _d3d12maAllocator, _commandList.Get(),
        meshInfos.data(), meshInfos.size() * sizeof(MeshDrawInfo),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        &_meshInfoAlloc, miStaging, &miStagingAlloc);

    _commandList->Close();
    ID3D12CommandList* lists2[] = { _commandList.Get() };
    _commandQueue->ExecuteCommandLists(1, lists2);
    ++_globalFenceValue;
    _commandQueue->Signal(_fence.Get(), _globalFenceValue);
    _frames[0].fenceValue = _globalFenceValue;
    WaitForFrame(0);

    if (vbStagingAlloc) vbStagingAlloc->Release();
    if (ibStagingAlloc) ibStagingAlloc->Release();
    if (miStagingAlloc) miStagingAlloc->Release();

    LUNA_LOG_INFO("Phase 12: merged geometry -- %zu verts, %zu indices, %zu meshes",
                  allVertices.size(), allIndices.size(), meshInfos.size());

    // Phase 25: Build meshlets for mesh shader pipeline
    if (_meshShadersSupported)
    {
        std::vector<Meshlet>        allMeshlets;
        std::vector<MeshletBounds>  allBounds;
        std::vector<uint32_t>       allMeshletVerts;
        std::vector<uint32_t>       allMeshletTris;
        std::vector<MeshletMeshInfo> meshletMeshInfos;

        _meshMeshletOffsets.clear();
        _meshMeshletCounts.clear();

        for (size_t m = 0; m < meshInfos.size(); ++m)
        {
            const MeshDrawInfo& mi = meshInfos[m];

            // Extract positions for this mesh's vertices (adjusted for merged offsets)
            std::vector<DirectX::XMFLOAT3> positions(mi.indexCount > 0 ? allVertices.size() : 0);
            // We need positions for the vertices referenced by this mesh's indices
            // Build a local index/vertex view
            uint32_t localVertCount = 0;
            // Find max vertex index referenced
            uint32_t maxVtx = 0;
            for (uint32_t i = 0; i < mi.indexCount; ++i)
            {
                uint32_t idx = allIndices[mi.firstIndex + i];
                if (idx > maxVtx) maxVtx = idx;
            }
            localVertCount = maxVtx + 1;

            // Extract positions for the vertex range
            std::vector<DirectX::XMFLOAT3> localPositions(localVertCount);
            for (uint32_t v = 0; v < localVertCount; ++v)
            {
                uint32_t globalV = mi.vertexOffset + v;
                if (globalV < allVertices.size())
                    localPositions[v] = allVertices[globalV].position;
            }

            // Build local indices (subtract vertexOffset to get 0-based)
            std::vector<uint32_t> localIndices(mi.indexCount);
            for (uint32_t i = 0; i < mi.indexCount; ++i)
                localIndices[i] = allIndices[mi.firstIndex + i]; // these are already 0-based for this mesh in merged buffer - actually they're relative to vertexOffset

            // The merged indices stored in allIndices are the original mesh-local indices.
            // BuildMeshlets expects 0-based indices into localPositions.
            MeshletBuildResult mbr = BuildMeshlets(
                localPositions.data(), localVertCount,
                localIndices.data(), mi.indexCount);

            MeshletMeshInfo mmi{};
            mmi.meshletOffset = static_cast<uint32_t>(allMeshlets.size());
            mmi.meshletCount  = static_cast<uint32_t>(mbr.meshlets.size());

            _meshMeshletOffsets.push_back(mmi.meshletOffset);
            _meshMeshletCounts.push_back(mmi.meshletCount);

            // Adjust meshlet vertex indices to global merged VB space
            uint32_t vertBase = static_cast<uint32_t>(allMeshletVerts.size());
            uint32_t triBase  = static_cast<uint32_t>(allMeshletTris.size());

            for (auto& ml : mbr.meshlets)
            {
                Meshlet adjusted = ml;
                adjusted.vertexOffset   += vertBase;
                adjusted.triangleOffset += triBase;
                allMeshlets.push_back(adjusted);
            }
            allBounds.insert(allBounds.end(), mbr.bounds.begin(), mbr.bounds.end());

            // Offset meshlet vertex indices to global merged VB
            for (uint32_t vi : mbr.meshletVertices)
                allMeshletVerts.push_back(vi + mi.vertexOffset);

            allMeshletTris.insert(allMeshletTris.end(), mbr.meshletTriangles.begin(), mbr.meshletTriangles.end());
            meshletMeshInfos.push_back(mmi);
        }

        // Upload meshlet buffers
        _frames[0].cmdAllocator->Reset();
        _commandList->Reset(_frames[0].cmdAllocator.Get(), nullptr);

        ComPtr<ID3D12Resource> mlStaging, mbStaging, mvStaging, mtStaging, mmStaging;
        D3D12MA::Allocation* mlSA = nullptr, *mbSA = nullptr, *mvSA = nullptr, *mtSA = nullptr, *mmSA = nullptr;

        if (!allMeshlets.empty())
        {
            _meshletBuffer = MeshLoader::UploadBuffer(
                _d3d12maAllocator, _commandList.Get(),
                allMeshlets.data(), allMeshlets.size() * sizeof(Meshlet),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                &_meshletBufferAlloc, mlStaging, &mlSA);

            _meshletBoundsBuffer = MeshLoader::UploadBuffer(
                _d3d12maAllocator, _commandList.Get(),
                allBounds.data(), allBounds.size() * sizeof(MeshletBounds),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                &_meshletBoundsAlloc, mbStaging, &mbSA);

            _meshletVertexBuffer = MeshLoader::UploadBuffer(
                _d3d12maAllocator, _commandList.Get(),
                allMeshletVerts.data(), allMeshletVerts.size() * sizeof(uint32_t),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                &_meshletVertexAlloc, mvStaging, &mvSA);

            _meshletTriBuffer = MeshLoader::UploadBuffer(
                _d3d12maAllocator, _commandList.Get(),
                allMeshletTris.data(), allMeshletTris.size() * sizeof(uint32_t),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                &_meshletTriAlloc, mtStaging, &mtSA);

            _meshletMeshInfoBuffer = MeshLoader::UploadBuffer(
                _d3d12maAllocator, _commandList.Get(),
                meshletMeshInfos.data(), meshletMeshInfos.size() * sizeof(MeshletMeshInfo),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                &_meshletMeshInfoAlloc, mmStaging, &mmSA);
        }

        _commandList->Close();
        ID3D12CommandList* lists3[] = { _commandList.Get() };
        _commandQueue->ExecuteCommandLists(1, lists3);
        ++_globalFenceValue;
        _commandQueue->Signal(_fence.Get(), _globalFenceValue);
        _frames[0].fenceValue = _globalFenceValue;
        WaitForFrame(0);

        if (mlSA) mlSA->Release();
        if (mbSA) mbSA->Release();
        if (mvSA) mvSA->Release();
        if (mtSA) mtSA->Release();
        if (mmSA) mmSA->Release();

        LUNA_LOG_INFO("Phase 25: meshlets -- %zu meshlets, %zu meshlet verts, %zu meshlet tris",
                      allMeshlets.size(), allMeshletVerts.size(), allMeshletTris.size());
    }
}

bool DX12Backend::CreateIndirectResources()
{
    if (!_device || _sceneMeshes.empty()) return false;

    // Build merged geometry first
    BuildMergedGeometry();
    if (!_mergedVB || !_mergedIB || !_meshInfoBuffer) return false;

    // Object data buffer (GPU-visible UPLOAD for simplicity — written each frame)
    // Per-frame to avoid CPU-GPU race (Bug #010 fix)
    const UINT objBufSize = MAX_GPU_OBJECTS * sizeof(GPUObjectData);
    D3D12_HEAP_PROPERTIES uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC bufDesc = CD3DX12_RESOURCE_DESC::Buffer(objBufSize);
    HRESULT hr = S_OK;
    for (UINT i = 0; i < FRAMES_IN_FLIGHT; ++i)
    {
        hr = _device->CreateCommittedResource(
            &uploadHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&_objectDataBuffer[i]));
        if (FAILED(hr)) { LUNA_LOG_ERROR("Phase 12: object data buffer[%u] creation failed", i); return false; }
        D3D12_RANGE r = { 0, 0 };
        _objectDataBuffer[i]->Map(0, &r, &_objectDataMapped[i]); // keep mapped, store pointer
    }

    // Indirect argument buffer + draw count buffer — one per frame slot to avoid
    // compute/graphics queue race (cull_F1 writes while draw_F0 reads the same buffer).
    const UINT argBufSize = MAX_GPU_OBJECTS * sizeof(IndirectDrawCommand);
    D3D12_HEAP_PROPERTIES defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format              = DXGI_FORMAT_R32_TYPELESS;
    uavDesc.ViewDimension       = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.FirstElement = 0;
    uavDesc.Buffer.NumElements  = 1;
    uavDesc.Buffer.Flags        = D3D12_BUFFER_UAV_FLAG_RAW;

    for (UINT i = 0; i < FRAMES_IN_FLIGHT; ++i)
    {
        bufDesc = CD3DX12_RESOURCE_DESC::Buffer(argBufSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        hr = _device->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&_indirectArgBuffer[i]));
        if (FAILED(hr)) { LUNA_LOG_ERROR("Phase 12: indirect arg buffer[%u] creation failed", i); return false; }

        bufDesc = CD3DX12_RESOURCE_DESC::Buffer(4, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        hr = _device->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&_drawCountBuffer[i]));
        if (FAILED(hr)) { LUNA_LOG_ERROR("Phase 12: draw count buffer[%u] creation failed", i); return false; }

        D3D12_DESCRIPTOR_HEAP_DESC nonVisDesc = {};
        nonVisDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        nonVisDesc.NumDescriptors = 1;
        nonVisDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(_device->CreateDescriptorHeap(&nonVisDesc, IID_PPV_ARGS(&_drawCountNonVisHeap[i]))))
        {
            LUNA_LOG_ERROR("Phase 12: draw count non-visible heap[%u] creation failed", i);
            return false;
        }
        _drawCountUAVNonVis[i] = _drawCountNonVisHeap[i]->GetCPUDescriptorHandleForHeapStart();
        AllocateSRVSlot(_drawCountUAVCpu[i], _drawCountUAVGpu[i]);
        _device->CreateUnorderedAccessView(_drawCountBuffer[i].Get(), nullptr, &uavDesc, _drawCountUAVCpu[i]);
        _device->CreateUnorderedAccessView(_drawCountBuffer[i].Get(), nullptr, &uavDesc, _drawCountUAVNonVis[i]);
    }

    // Draw count readback (READBACK heap, 4 bytes — single; diagnostic only)
    D3D12_HEAP_PROPERTIES readbackHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
    bufDesc = CD3DX12_RESOURCE_DESC::Buffer(4);
    _device->CreateCommittedResource(
        &readbackHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&_drawCountReadback));

    // ViewProj CBs (per-frame, UPLOAD)
    const UINT vpCBSize = (sizeof(ViewProjCB) + 255) & ~255;
    bufDesc = CD3DX12_RESOURCE_DESC::Buffer(vpCBSize);
    for (UINT i = 0; i < FRAMES_IN_FLIGHT; ++i)
    {
        hr = _device->CreateCommittedResource(
            &uploadHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&_viewProjCB[i]));
        if (FAILED(hr)) return false;
        D3D12_RANGE noRead = { 0, 0 };
        _viewProjCB[i]->Map(0, &noRead, &_viewProjCBMapped[i]);
        _viewProjCBGPUAddr[i] = _viewProjCB[i]->GetGPUVirtualAddress();
    }

    // Command signature: per-draw CBV (b1) + materialIndex (root const b2) + objectIndex (root const b3) + DrawIndexed
    D3D12_INDIRECT_ARGUMENT_DESC argDescs[4] = {};
    // Arg 0: set MaterialConstants CBV at root param index 1
    argDescs[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW;
    argDescs[0].ConstantBufferView.RootParameterIndex = 1;
    // Arg 1: set materialIndex at root param index 2
    argDescs[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    argDescs[1].Constant.RootParameterIndex = 2;
    argDescs[1].Constant.DestOffsetIn32BitValues = 0;
    argDescs[1].Constant.Num32BitValuesToSet = 1;
    // Arg 2: set objectIndex at root param index 3
    argDescs[2].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    argDescs[2].Constant.RootParameterIndex = 3;
    argDescs[2].Constant.DestOffsetIn32BitValues = 0;
    argDescs[2].Constant.Num32BitValuesToSet = 1;
    // Arg 3: DrawIndexedInstanced
    argDescs[3].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;

    // Create the indirect G-buffer pipeline first (need root signature for command signature)
    _indirectGBufPipeline = std::make_unique<DX12Pipeline>();
    {
        PipelineStateDesc desc;
        desc.enableDepthTest  = true;
        desc.vertexLayout     = VertexLayout::PBR;
        desc.rootLayout       = RootSignatureLayout::PBRIndirect;
        desc.numRenderTargets = 3;
        desc.rtvFormats[0]    = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.rtvFormats[1]    = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.rtvFormats[2]    = DXGI_FORMAT_R8G8B8A8_UNORM;
        if (!_indirectGBufPipeline->Initialize(_device, L"pbr_indirect.vert.hlsl", L"gbuffer.frag.hlsl", desc))
        {
            LUNA_LOG_ERROR("Phase 12: indirect G-buffer pipeline init failed");
            _indirectGBufPipeline.reset();
            return false;
        }
    }

    D3D12_COMMAND_SIGNATURE_DESC cmdSigDesc = {};
    cmdSigDesc.ByteStride       = sizeof(IndirectDrawCommand); // 40 bytes
    cmdSigDesc.NumArgumentDescs = 4;
    cmdSigDesc.pArgumentDescs   = argDescs;
    hr = _device->CreateCommandSignature(
        &cmdSigDesc, _indirectGBufPipeline->GetRootSignature().Get(),
        IID_PPV_ARGS(&_indirectCmdSignature));
    if (FAILED(hr)) { LUNA_LOG_ERROR("Phase 12: command signature creation failed: 0x%08lX", (unsigned long)hr); return false; }

    // GPU cull compute pipeline
    _gpuCullPipeline = std::make_unique<DX12Pipeline>();
    {
        PipelineStateDesc desc;
        desc.rootLayout    = RootSignatureLayout::GPUCull;
        desc.computeShader = true;
        if (!_gpuCullPipeline->Initialize(_device, L"gpu_cull.comp.hlsl", L"", desc))
        {
            LUNA_LOG_ERROR("Phase 12: GPU cull pipeline init failed");
            _gpuCullPipeline.reset();
            return false;
        }
    }

    _gpuDrivenReady = true;
    LUNA_LOG_INFO("Phase 12: GPU-driven rendering ready (command sig stride=%u)", sizeof(IndirectDrawCommand));
    return true;
}

void DX12Backend::DestroyIndirectResources()
{
    _gpuDrivenReady = false;
    _indirectCmdSignature.Reset();
    _gpuCullPipeline.reset();
    _indirectGBufPipeline.reset();

    for (UINT i = 0; i < FRAMES_IN_FLIGHT; ++i)
    {
        _objectDataBuffer[i].Reset();
        _indirectArgBuffer[i].Reset();
        _drawCountBuffer[i].Reset();
        _drawCountNonVisHeap[i].Reset();
        _drawCountUAVCpu[i]    = {};
        _drawCountUAVGpu[i]    = {};
        _drawCountUAVNonVis[i] = {};
    }
    _drawCountReadback.Reset();

    for (UINT i = 0; i < FRAMES_IN_FLIGHT; ++i)
        _viewProjCB[i].Reset();

    if (_mergedVBAlloc) { _mergedVBAlloc->Release(); _mergedVBAlloc = nullptr; }
    if (_mergedIBAlloc) { _mergedIBAlloc->Release(); _mergedIBAlloc = nullptr; }
    if (_meshInfoAlloc) { _meshInfoAlloc->Release(); _meshInfoAlloc = nullptr; }
    _mergedVB.Reset();
    _mergedIB.Reset();
    _meshInfoBuffer.Reset();
}

// ===========================================================================
// Phase 23: Hi-Z Occlusion Culling
// ===========================================================================

bool DX12Backend::CreateHiZResources()
{
    if (_screenWidth <= 0 || _screenHeight <= 0) return false;

    // Compute mip count: floor(log2(max(w,h))) + 1
    UINT w = (UINT)_screenWidth;
    UINT h = (UINT)_screenHeight;
    _hizMipCount = 1;
    { UINT t = std::max(w, h); while (t > 1) { t >>= 1; _hizMipCount++; } }
    _hizMipCount = std::min(_hizMipCount, HIZ_MAX_MIPS);

    // Create Hi-Z texture: R32_FLOAT, full-res, full mip chain, UAV+SRV
    D3D12MA::ALLOCATION_DESC allocDesc = {};
    allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width              = w;
    rd.Height             = h;
    rd.DepthOrArraySize   = 1;
    rd.MipLevels          = (UINT16)_hizMipCount;
    rd.Format             = DXGI_FORMAT_R32_FLOAT;
    rd.SampleDesc.Count   = 1;
    rd.Flags              = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    HRESULT hr = _d3d12maAllocator->CreateResource(
        &allocDesc, &rd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        nullptr, &_hizTextureAlloc, IID_PPV_ARGS(&_hizTexture));
    if (FAILED(hr)) { LUNA_LOG_ERROR("Phase 23: Hi-Z texture creation failed"); return false; }
    _hizTexture->SetName(L"Phase23_HiZ_Pyramid");

    // Allocate per-mip SRV and UAV descriptors in _imGuiSrvHeap
    for (UINT m = 0; m < _hizMipCount; ++m)
    {
        // SRV for this mip
        D3D12_CPU_DESCRIPTOR_HANDLE srvCPU;
        D3D12_GPU_DESCRIPTOR_HANDLE srvGPU;
        _hizMipSRVIndex[m] = AllocateSRVSlot(srvCPU, srvGPU);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format                  = DXGI_FORMAT_R32_FLOAT;
        srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MostDetailedMip = m;
        srvDesc.Texture2D.MipLevels       = 1;
        _device->CreateShaderResourceView(_hizTexture.Get(), &srvDesc, srvCPU);

        // UAV for this mip
        D3D12_CPU_DESCRIPTOR_HANDLE uavCPU;
        D3D12_GPU_DESCRIPTOR_HANDLE uavGPU;
        _hizMipUAVIndex[m] = AllocateSRVSlot(uavCPU, uavGPU);

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format              = DXGI_FORMAT_R32_FLOAT;
        uavDesc.ViewDimension       = D3D12_UAV_DIMENSION_TEXTURE2D;
        uavDesc.Texture2D.MipSlice  = m;
        _device->CreateUnorderedAccessView(_hizTexture.Get(), nullptr, &uavDesc, uavCPU);
    }

    // Full-pyramid SRV (all mips) for cull shader sampling
    {
        D3D12_CPU_DESCRIPTOR_HANDLE srvCPU;
        D3D12_GPU_DESCRIPTOR_HANDLE srvGPU;
        _hizFullSRVIndex = AllocateSRVSlot(srvCPU, srvGPU);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format                  = DXGI_FORMAT_R32_FLOAT;
        srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels       = _hizMipCount;
        _device->CreateShaderResourceView(_hizTexture.Get(), &srvDesc, srvCPU);
    }

    // Create Hi-Z generate compute pipeline
    {
        PipelineStateDesc desc;
        desc.computeShader = true;
        desc.rootLayout    = RootSignatureLayout::HiZGenerate;
        _hizGeneratePipeline = std::make_unique<DX12Pipeline>();
        if (!_hizGeneratePipeline->Initialize(_device, L"hiz_generate.comp.hlsl", L"", desc))
        { LUNA_LOG_ERROR("Phase 23: Hi-Z generate pipeline creation failed"); return false; }
    }

    LUNA_LOG_INFO("Phase 23: Hi-Z pyramid created — %ux%u, %u mips", w, h, _hizMipCount);
    return true;
}

void DX12Backend::DestroyHiZResources()
{
    _hizReady = false;
    _hizGeneratePipeline.reset();
    if (_hizTextureAlloc) { _hizTextureAlloc->Release(); _hizTextureAlloc = nullptr; }
    _hizTexture.Reset();
    _hizNonVisUAVHeap.Reset();
    _hizMipCount = 0;
    _hizFullSRVIndex = UINT_MAX;
}

void DX12Backend::BuildHiZPyramid()
{
    if (!_hizTexture || _hizMipCount < 2) return;

    auto* cmd = _commandList.Get();
    ID3D12DescriptorHeap* heaps[] = { _imGuiSrvHeap.Get() };
    cmd->SetDescriptorHeaps(1, heaps);

    _gpuProfiler.InsertBeginTimestamp(cmd, "Hi-Z Build");

    // Copy depth buffer (mip 0 of Hi-Z) from the main depth buffer
    // Transition depth → COPY_SOURCE, Hi-Z mip 0 → COPY_DEST
    {
        D3D12_RESOURCE_BARRIER barriers[2] = {
            CD3DX12_RESOURCE_BARRIER::Transition(_depthBuffer.Get(),
                D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(_hizTexture.Get(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST,
                0),  // subresource 0 = mip 0
        };
        cmd->ResourceBarrier(2, barriers);
    }

    // Copy mip-0: depth (D32_FLOAT) → Hi-Z mip 0 (R32_FLOAT) — same bit layout
    {
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = _depthBuffer.Get();
        src.Type      = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = _hizTexture.Get();
        dst.Type      = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;

        cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }

    // Transition: depth → DEPTH_WRITE, Hi-Z mip 0 → SRV
    {
        D3D12_RESOURCE_BARRIER barriers[2] = {
            CD3DX12_RESOURCE_BARRIER::Transition(_depthBuffer.Get(),
                D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE),
            CD3DX12_RESOURCE_BARRIER::Transition(_hizTexture.Get(),
                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                0),
        };
        cmd->ResourceBarrier(2, barriers);
    }

    // Generate remaining mips via compute dispatch
    cmd->SetPipelineState(_hizGeneratePipeline->GetPSO());
    cmd->SetComputeRootSignature(_hizGeneratePipeline->GetRootSignature().Get());

    UINT srcW = (UINT)_screenWidth;
    UINT srcH = (UINT)_screenHeight;

    D3D12_GPU_DESCRIPTOR_HANDLE heapBase = _imGuiSrvHeap->GetGPUDescriptorHandleForHeapStart();

    for (UINT m = 1; m < _hizMipCount; ++m)
    {
        UINT dstW = std::max(srcW >> 1, 1u);
        UINT dstH = std::max(srcH >> 1, 1u);

        // Transition Hi-Z mip m → UAV
        D3D12_RESOURCE_BARRIER toUAV = CD3DX12_RESOURCE_BARRIER::Transition(_hizTexture.Get(),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, m);
        cmd->ResourceBarrier(1, &toUAV);

        // Set root constants: srcW, srcH, dstW, dstH
        UINT constants[4] = { srcW, srcH, dstW, dstH };
        cmd->SetComputeRoot32BitConstants(0, 4, constants, 0);

        // Bind source mip SRV (mip m-1) and destination mip UAV (mip m)
        D3D12_GPU_DESCRIPTOR_HANDLE srcSrv;
        srcSrv.ptr = heapBase.ptr + _hizMipSRVIndex[m - 1] * _srvDescriptorSize;
        cmd->SetComputeRootDescriptorTable(1, srcSrv);

        D3D12_GPU_DESCRIPTOR_HANDLE dstUav;
        dstUav.ptr = heapBase.ptr + _hizMipUAVIndex[m] * _srvDescriptorSize;
        cmd->SetComputeRootDescriptorTable(2, dstUav);

        cmd->Dispatch((dstW + 7) / 8, (dstH + 7) / 8, 1);

        // Transition Hi-Z mip m → SRV (for next iteration or cull shader)
        D3D12_RESOURCE_BARRIER toSRV = CD3DX12_RESOURCE_BARRIER::Transition(_hizTexture.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, m);
        cmd->ResourceBarrier(1, &toSRV);

        srcW = dstW;
        srcH = dstH;
    }

    _gpuProfiler.InsertEndTimestamp(cmd);
    _hizReady = true;
}

// Phase 13: Dispatch GPU frustum cull on the async compute queue
void DX12Backend::DispatchCullAsync()
{
    if (!_asyncComputeReady || !_gpuDrivenReady || _cpuInstances.empty()) return;

    const UINT instanceCount = static_cast<UINT>(_cpuInstances.size());

    // Bug #010 fix: _objectDataBuffer is now per-frame to avoid CPU-GPU race.
    // No need to wait for previous frame's compute since each frame has its own buffer.

    // Upload object data (UPLOAD heap — CPU-visible, use pre-mapped pointer)
    memcpy(_objectDataMapped[_frameIndex], _cpuInstances.data(), instanceCount * sizeof(GPUObjectData));

    // Open compute command list
    auto* ccmd = _computeCommandList.Get();
    ccmd->Reset(_frames[_frameIndex].computeCmdAllocator.Get(), nullptr);

    // Clear draw count using per-frame pre-allocated UAV descriptors
    ID3D12DescriptorHeap* heaps[] = { _imGuiSrvHeap.Get() };
    ccmd->SetDescriptorHeaps(1, heaps);
    const UINT clearVal[4] = { 0, 0, 0, 0 };
    ccmd->ClearUnorderedAccessViewUint(_drawCountUAVGpu[_frameIndex], _drawCountUAVNonVis[_frameIndex],
                                       _drawCountBuffer[_frameIndex].Get(), clearVal, 0, nullptr);

    D3D12_RESOURCE_BARRIER uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(_drawCountBuffer[_frameIndex].Get());
    ccmd->ResourceBarrier(1, &uavBarrier);

    // Dispatch cull compute
    ccmd->SetPipelineState(_gpuCullPipeline->GetPSO());
    ccmd->SetComputeRootSignature(_gpuCullPipeline->GetRootSignature().Get());

    // Phase 23: Expanded CullConstants (48 DWORDs = 192 bytes)
    struct CullConstants {
        XMFLOAT4   planes[6];
        UINT       objectCount;
        UINT       enableHiZ;
        UINT       hizMipCount;
        UINT       _pad0;
        XMFLOAT4X4 viewProj;
        float      screenW;
        float      screenH;
        float      _pad1[2];
    } cullCB{};
    ExtractFrustumPlanes(_lastView, _lastProj, cullCB.planes);
    cullCB.objectCount = instanceCount;
    cullCB.enableHiZ   = _hizReady ? 1 : 0;
    cullCB.hizMipCount = _hizMipCount;
    {
        XMMATRIX V = XMLoadFloat4x4(&_lastView);
        XMMATRIX P = XMLoadFloat4x4(&_lastProj);
        XMStoreFloat4x4(&cullCB.viewProj, XMMatrixMultiply(V, P));
    }
    cullCB.screenW = (float)_screenWidth;
    cullCB.screenH = (float)_screenHeight;
    ccmd->SetComputeRoot32BitConstants(0, 48, &cullCB, 0);
    ccmd->SetComputeRootShaderResourceView(1, _objectDataBuffer[_frameIndex]->GetGPUVirtualAddress());
    ccmd->SetComputeRootShaderResourceView(2, _meshInfoBuffer->GetGPUVirtualAddress());
    ccmd->SetComputeRootUnorderedAccessView(3, _indirectArgBuffer[_frameIndex]->GetGPUVirtualAddress());
    ccmd->SetComputeRootUnorderedAccessView(4, _drawCountBuffer[_frameIndex]->GetGPUVirtualAddress());

    // Phase 23: Bind Hi-Z pyramid SRV for occlusion test
    if (_hizReady && _hizFullSRVIndex != UINT_MAX)
    {
        D3D12_GPU_DESCRIPTOR_HANDLE heapBase = _imGuiSrvHeap->GetGPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE hizSrv;
        hizSrv.ptr = heapBase.ptr + _hizFullSRVIndex * _srvDescriptorSize;
        ccmd->SetComputeRootDescriptorTable(5, hizSrv);
    }

    UINT groups = (instanceCount + 63) / 64;
    ccmd->Dispatch(groups, 1, 1);

    // UAV barriers
    D3D12_RESOURCE_BARRIER postCompute[2] = {
        CD3DX12_RESOURCE_BARRIER::UAV(_indirectArgBuffer[_frameIndex].Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(_drawCountBuffer[_frameIndex].Get()),
    };
    ccmd->ResourceBarrier(2, postCompute);

    // Close and execute on compute queue
    ccmd->Close();

    // Phase 23 fix: ensure previous frame's graphics work (including BuildHiZPyramid)
    // has completed before the compute queue reads the Hi-Z texture.
    // Without this, the compute cull can race with the graphics Hi-Z write.
    if (_hizReady)
    {
        UINT prevFI = (_frameIndex == 0) ? (FRAMES_IN_FLIGHT - 1) : (_frameIndex - 1);
        if (_frames[prevFI].fenceValue > 0)
            _computeQueue->Wait(_fence.Get(), _frames[prevFI].fenceValue);
    }

    ID3D12CommandList* lists[] = { ccmd };
    _computeQueue->ExecuteCommandLists(1, lists);

    // Signal compute fence
    ++_computeFenceValue;
    _computeQueue->Signal(_computeFence.Get(), _computeFenceValue);
    _frames[_frameIndex].computeFenceValue = _computeFenceValue;
}

void DX12Backend::FlushDraws()
{
    if (!_gpuDrivenReady || _cpuInstances.empty()) return;

    auto* cmd = _commandList.Get();
    const UINT instanceCount = static_cast<UINT>(_cpuInstances.size());

    // Phase 25: Mesh shader path — per-object DispatchMesh with AS+MS culling
    if (_meshShaderReady && _meshShaderGBufPipeline)
    {
        // Upload object data
        memcpy(_objectDataMapped[_frameIndex], _cpuInstances.data(), instanceCount * sizeof(GPUObjectData));

        // Build frustum planes
        XMMATRIX V = XMLoadFloat4x4(&_lastView);
        XMMATRIX P = XMLoadFloat4x4(&_lastProj);
        XMMATRIX VP = XMMatrixMultiply(V, P);
        XMFLOAT4X4 vpMat;
        XMStoreFloat4x4(&vpMat, VP);

        XMFLOAT4 frustumPlanes[6];
        frustumPlanes[0] = { vpMat._14 + vpMat._11, vpMat._24 + vpMat._21, vpMat._34 + vpMat._31, vpMat._44 + vpMat._41 };
        frustumPlanes[1] = { vpMat._14 - vpMat._11, vpMat._24 - vpMat._21, vpMat._34 - vpMat._31, vpMat._44 - vpMat._41 };
        frustumPlanes[2] = { vpMat._14 + vpMat._12, vpMat._24 + vpMat._22, vpMat._34 + vpMat._32, vpMat._44 + vpMat._42 };
        frustumPlanes[3] = { vpMat._14 - vpMat._12, vpMat._24 - vpMat._22, vpMat._34 - vpMat._32, vpMat._44 - vpMat._42 };
        frustumPlanes[4] = { vpMat._13, vpMat._23, vpMat._33, vpMat._43 };
        frustumPlanes[5] = { vpMat._14 - vpMat._13, vpMat._24 - vpMat._23, vpMat._34 - vpMat._33, vpMat._44 - vpMat._43 };
        for (int i = 0; i < 6; ++i)
        {
            XMVECTOR p = XMLoadFloat4(&frustumPlanes[i]);
            float len = XMVectorGetX(XMVector3Length(p));
            if (len > 0.0001f)
            {
                p = XMVectorScale(p, 1.0f / len);
                XMStoreFloat4(&frustumPlanes[i], p);
            }
        }

        // Set pipeline state
        cmd->SetPipelineState(_meshShaderGBufPipeline->GetPSO());
        cmd->SetGraphicsRootSignature(_meshShaderGBufPipeline->GetRootSignature().Get());
        cmd->RSSetViewports(1, &_screenViewport);
        cmd->RSSetScissorRects(1, &_scissorRect);

        D3D12_CPU_DESCRIPTOR_HANDLE rtvs[GBUFFER_COUNT] = { _gbufferRTV[0], _gbufferRTV[1], _gbufferRTV[2] };
        cmd->OMSetRenderTargets(GBUFFER_COUNT, rtvs, FALSE, &_dsvHandle);

        ID3D12DescriptorHeap* heaps[] = { _imGuiSrvHeap.Get() };
        cmd->SetDescriptorHeaps(1, heaps);

        // Bind SRV buffers (shared across all dispatches)
        cmd->SetGraphicsRootShaderResourceView(3, _objectDataBuffer[_frameIndex]->GetGPUVirtualAddress()); // t0: objects
        cmd->SetGraphicsRootShaderResourceView(4, _meshletBuffer->GetGPUVirtualAddress());                 // t1: meshlets
        cmd->SetGraphicsRootShaderResourceView(5, _meshletBoundsBuffer->GetGPUVirtualAddress());           // t2: bounds
        cmd->SetGraphicsRootShaderResourceView(6, _mergedVB->GetGPUVirtualAddress());                      // t3: vertices
        cmd->SetGraphicsRootShaderResourceView(7, _meshletVertexBuffer->GetGPUVirtualAddress());            // t4: meshletVerts
        cmd->SetGraphicsRootShaderResourceView(8, _meshletTriBuffer->GetGPUVirtualAddress());               // t5: meshletTris

        // Bindless texture heap
        D3D12_GPU_DESCRIPTOR_HANDLE heapBase = _imGuiSrvHeap->GetGPUDescriptorHandleForHeapStart();
        cmd->SetGraphicsRootDescriptorTable(9, heapBase);

        // Per-object dispatch
        for (UINT obj = 0; obj < instanceCount; ++obj)
        {
            const GPUObjectData& inst = _cpuInstances[obj];
            uint32_t meshIdx = inst.meshIndex;
            if (meshIdx >= _meshMeshletOffsets.size()) continue;

            uint32_t meshletOff   = _meshMeshletOffsets[meshIdx];
            uint32_t meshletCount = _meshMeshletCounts[meshIdx];
            if (meshletCount == 0) continue;

            // Fill per-dispatch constants
            MeshShaderConstants msc{};
            msc.viewMatrix = _lastView;
            msc.projMatrix = _lastProj;
            memcpy(msc.frustumPlanes, frustumPlanes, sizeof(frustumPlanes));
            msc.objectIndex   = obj;
            msc.meshletOffset = meshletOff;
            msc.meshletCount  = meshletCount;
            memcpy(_meshShaderCBMapped[_frameIndex], &msc, sizeof(msc));

            cmd->SetGraphicsRootConstantBufferView(0, _meshShaderCB[_frameIndex]->GetGPUVirtualAddress());

            // Material
            cmd->SetGraphicsRootConstantBufferView(1, inst.materialCBAddr);
            cmd->SetGraphicsRoot32BitConstant(2, inst.materialIndex, 0);

            // Dispatch mesh: ceil(meshletCount / 32) groups
            UINT groupCount = (meshletCount + 31) / 32;

            ComPtr<ID3D12GraphicsCommandList6> cmd6;
            cmd->QueryInterface(IID_PPV_ARGS(&cmd6));
            if (cmd6)
                cmd6->DispatchMesh(groupCount, 1, 1);
        }

        // Build Hi-Z pyramid for next frame
        if (_hizTexture)
            BuildHiZPyramid();

        _cpuInstances.clear();
        return;
    }

    // Phase 13: async compute path — dispatch cull on compute queue
    if (_asyncComputeReady)
    {
        DispatchCullAsync();

        // Update ViewProj CB
        ViewProjCB vp;
        vp.view = _lastView;
        vp.proj = _lastProj;
        memcpy(_viewProjCBMapped[_frameIndex], &vp, sizeof(ViewProjCB));

        // Graphics queue waits for compute cull to finish
        _commandQueue->Wait(_computeFence.Get(), _computeFenceValue);

        // Transition this frame's buffers to INDIRECT_ARGUMENT on graphics queue
        D3D12_RESOURCE_BARRIER barriers[2] = {
            CD3DX12_RESOURCE_BARRIER::Transition(_indirectArgBuffer[_frameIndex].Get(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT),
            CD3DX12_RESOURCE_BARRIER::Transition(_drawCountBuffer[_frameIndex].Get(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT),
        };
        cmd->ResourceBarrier(2, barriers);
    }
    else
    {
        // Fallback: dispatch cull on graphics queue (Phase 12 legacy path)
        // Use pre-mapped pointer (buffer stays mapped permanently)
        memcpy(_objectDataMapped[_frameIndex], _cpuInstances.data(), instanceCount * sizeof(GPUObjectData));

        ViewProjCB vp;
        vp.view = _lastView;
        vp.proj = _lastProj;
        memcpy(_viewProjCBMapped[_frameIndex], &vp, sizeof(ViewProjCB));


        // Clear draw count using per-frame UAV descriptors
        ID3D12DescriptorHeap* heaps[] = { _imGuiSrvHeap.Get() };
        cmd->SetDescriptorHeaps(1, heaps);
        const UINT clearVal[4] = { 0, 0, 0, 0 };
        cmd->ClearUnorderedAccessViewUint(_drawCountUAVGpu[_frameIndex], _drawCountUAVNonVis[_frameIndex],
                                          _drawCountBuffer[_frameIndex].Get(), clearVal, 0, nullptr);

        D3D12_RESOURCE_BARRIER uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(_drawCountBuffer[_frameIndex].Get());
        cmd->ResourceBarrier(1, &uavBarrier);

        cmd->SetPipelineState(_gpuCullPipeline->GetPSO());
        cmd->SetComputeRootSignature(_gpuCullPipeline->GetRootSignature().Get());

        // Phase 23: Expanded CullConstants (48 DWORDs = 192 bytes)
        struct CullConstants {
            XMFLOAT4   planes[6];     // 96 B
            UINT       objectCount;   //  4 B
            UINT       enableHiZ;     //  4 B
            UINT       hizMipCount;   //  4 B
            UINT       _pad0;         //  4 B → 112 B
            XMFLOAT4X4 viewProj;      // 64 B → 176 B
            float      screenW;       //  4 B
            float      screenH;       //  4 B
            float      _pad1[2];      //  8 B → 192 B = 48 DWORDs
        } cullCB{};
        
        // Inline frustum plane extraction (same as Vulkan approach)
        {
            XMMATRIX V = XMLoadFloat4x4(&_lastView);
            XMMATRIX P = XMLoadFloat4x4(&_lastProj);
            XMMATRIX VP = XMMatrixMultiply(V, P);
            XMFLOAT4X4 m;
            XMStoreFloat4x4(&m, VP);
            
            // Extract columns: colN = (_1N, _2N, _3N, _4N)
            // Left:   col3 + col0
            cullCB.planes[0] = { m._14 + m._11, m._24 + m._21, m._34 + m._31, m._44 + m._41 };
            // Right:  col3 - col0
            cullCB.planes[1] = { m._14 - m._11, m._24 - m._21, m._34 - m._31, m._44 - m._41 };
            // Bottom: col3 + col1
            cullCB.planes[2] = { m._14 + m._12, m._24 + m._22, m._34 + m._32, m._44 + m._42 };
            // Top:    col3 - col1
            cullCB.planes[3] = { m._14 - m._12, m._24 - m._22, m._34 - m._32, m._44 - m._42 };
            // Near:   col2
            cullCB.planes[4] = { m._13, m._23, m._33, m._43 };
            // Far:    col3 - col2
            cullCB.planes[5] = { m._14 - m._13, m._24 - m._23, m._34 - m._33, m._44 - m._43 };
            
            // Normalize planes
            for (int i = 0; i < 6; ++i)
            {
                XMVECTOR p = XMLoadFloat4(&cullCB.planes[i]);
                float len = XMVectorGetX(XMVector3Length(p));
                if (len > 0.0001f)
                {
                    p = XMVectorScale(p, 1.0f / len);
                    XMStoreFloat4(&cullCB.planes[i], p);
                }
            }
            
            // Store viewProj
            XMStoreFloat4x4(&cullCB.viewProj, VP);
        }
        
        cullCB.objectCount = instanceCount;
        // Bug #010: Hi-Z disabled - single-instance _hizTexture has cross-frame race.
        // Frame N may still be writing Hi-Z when Frame N+1 reads it.
        // TODO: Double-buffer _hizTexture to fix.
        cullCB.enableHiZ   = 0;
        cullCB.hizMipCount = _hizMipCount;
        cullCB.screenW = (float)_screenWidth;
        cullCB.screenH = (float)_screenHeight;
        cmd->SetComputeRoot32BitConstants(0, 48, &cullCB, 0);
        cmd->SetComputeRootShaderResourceView(1, _objectDataBuffer[_frameIndex]->GetGPUVirtualAddress());
        cmd->SetComputeRootShaderResourceView(2, _meshInfoBuffer->GetGPUVirtualAddress());
        cmd->SetComputeRootUnorderedAccessView(3, _indirectArgBuffer[_frameIndex]->GetGPUVirtualAddress());
        cmd->SetComputeRootUnorderedAccessView(4, _drawCountBuffer[_frameIndex]->GetGPUVirtualAddress());

        // Phase 23: Bind Hi-Z pyramid SRV for occlusion test
        if (_hizReady && _hizFullSRVIndex != UINT_MAX)
        {
            D3D12_GPU_DESCRIPTOR_HANDLE heapBase = _imGuiSrvHeap->GetGPUDescriptorHandleForHeapStart();
            D3D12_GPU_DESCRIPTOR_HANDLE hizSrv;
            hizSrv.ptr = heapBase.ptr + _hizFullSRVIndex * _srvDescriptorSize;
            cmd->SetComputeRootDescriptorTable(5, hizSrv);
        }

        UINT groups = (instanceCount + 63) / 64;
        cmd->Dispatch(groups, 1, 1);

        D3D12_RESOURCE_BARRIER postCompute[2] = {
            CD3DX12_RESOURCE_BARRIER::UAV(_indirectArgBuffer[_frameIndex].Get()),
            CD3DX12_RESOURCE_BARRIER::UAV(_drawCountBuffer[_frameIndex].Get()),
        };
        cmd->ResourceBarrier(2, postCompute);

        D3D12_RESOURCE_BARRIER restore2[2] = {
            CD3DX12_RESOURCE_BARRIER::Transition(_drawCountBuffer[_frameIndex].Get(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT),
            CD3DX12_RESOURCE_BARRIER::Transition(_indirectArgBuffer[_frameIndex].Get(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT),
        };
        cmd->ResourceBarrier(2, restore2);
    }

    // Execute indirect draws (common path)
    ID3D12DescriptorHeap* heaps[] = { _imGuiSrvHeap.Get() };
    {
        cmd->SetPipelineState(_indirectGBufPipeline->GetPSO());
        cmd->SetGraphicsRootSignature(_indirectGBufPipeline->GetRootSignature().Get());
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->RSSetViewports(1, &_screenViewport);
        cmd->RSSetScissorRects(1, &_scissorRect);

        // Bug #010: Explicitly re-bind G-buffer RTVs + depth before ExecuteIndirect
        // The compute dispatch might have affected GPU pipeline state on some drivers
        D3D12_CPU_DESCRIPTOR_HANDLE rtvs[GBUFFER_COUNT] = { _gbufferRTV[0], _gbufferRTV[1], _gbufferRTV[2] };
        cmd->OMSetRenderTargets(GBUFFER_COUNT, rtvs, FALSE, &_dsvHandle);

        cmd->IASetVertexBuffers(0, 1, &_mergedVBView);
        cmd->IASetIndexBuffer(&_mergedIBView);

        cmd->SetDescriptorHeaps(1, heaps);

        cmd->SetGraphicsRootConstantBufferView(0, _viewProjCBGPUAddr[_frameIndex]);
        cmd->SetGraphicsRootShaderResourceView(4, _objectDataBuffer[_frameIndex]->GetGPUVirtualAddress());
        D3D12_GPU_DESCRIPTOR_HANDLE heapBase = _imGuiSrvHeap->GetGPUDescriptorHandleForHeapStart();
        cmd->SetGraphicsRootDescriptorTable(5, heapBase);

        cmd->ExecuteIndirect(
            _indirectCmdSignature.Get(),
            MAX_GPU_OBJECTS,
            _indirectArgBuffer[_frameIndex].Get(), 0,
            _drawCountBuffer[_frameIndex].Get(), 0);
    }

    // Transition this frame's buffers back to UAV for next use
    D3D12_RESOURCE_BARRIER restore[2] = {
        CD3DX12_RESOURCE_BARRIER::Transition(_indirectArgBuffer[_frameIndex].Get(),
            D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        CD3DX12_RESOURCE_BARRIER::Transition(_drawCountBuffer[_frameIndex].Get(),
            D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    cmd->ResourceBarrier(2, restore);

    // Phase 23: Build Hi-Z pyramid from current frame's depth buffer (ready for next frame's cull)
    if (_hizTexture)
        BuildHiZPyramid();

    _cpuInstances.clear();
}

// ===========================================================================
// Phase 25: Mesh Shader Pipeline
// ===========================================================================

bool DX12Backend::CreateMeshShaderResources()
{
    if (!_meshShadersSupported || !_meshletBuffer) return false;

    // Create mesh shader pipeline (AS + MS + PS)
    _meshShaderGBufPipeline = std::make_unique<DX12Pipeline>();
    PipelineStateDesc msDesc{};
    msDesc.meshShaderPipeline = true;
    msDesc.rootLayout         = RootSignatureLayout::MeshShaderGBuffer;
    msDesc.enableDepthTest    = true;
    msDesc.numRenderTargets   = GBUFFER_COUNT;
    msDesc.rtvFormats[0]      = DXGI_FORMAT_R8G8B8A8_UNORM;       // GB0 albedo
    msDesc.rtvFormats[1]      = DXGI_FORMAT_R16G16B16A16_FLOAT;   // GB1 normal
    msDesc.rtvFormats[2]      = DXGI_FORMAT_R8G8B8A8_UNORM;       // GB2 metalRough

    if (!_meshShaderGBufPipeline->InitializeMeshShader(
            _device,
            L"meshlet_cull.as.hlsl",
            L"gbuffer_mesh.ms.hlsl",
            L"gbuffer_mesh.frag.hlsl",
            msDesc))
    {
        LUNA_LOG_ERROR("Phase 25: mesh shader pipeline creation failed");
        _meshShaderGBufPipeline.reset();
        return false;
    }

    // Create per-frame constant buffers for MeshShaderConstants
    D3D12_HEAP_PROPERTIES uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC cbDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(MeshShaderConstants));
    for (UINT i = 0; i < FRAMES_IN_FLIGHT; ++i)
    {
        D3D12MA::ALLOCATION_DESC allocDesc = {};
        allocDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;
        HRESULT hr = _d3d12maAllocator->CreateResource(
            &allocDesc, &cbDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            &_meshShaderCBAlloc[i],
            IID_PPV_ARGS(&_meshShaderCB[i]));
        if (FAILED(hr))
        {
            LUNA_LOG_ERROR("Phase 25: mesh shader CB[%u] creation failed", i);
            return false;
        }
        D3D12_RANGE readRange = { 0, 0 };
        _meshShaderCB[i]->Map(0, &readRange, &_meshShaderCBMapped[i]);
    }

    _meshShaderReady = true;
    LUNA_LOG_INFO("Phase 25: Mesh shader pipeline ready (%zu meshlets across %zu meshes)",
                  _meshMeshletCounts.empty() ? 0 :
                  _meshMeshletOffsets.back() + _meshMeshletCounts.back(),
                  _meshMeshletCounts.size());
    return true;
}

void DX12Backend::DestroyMeshShaderResources()
{
    _meshShaderReady = false;
    _meshShaderGBufPipeline.reset();

    for (UINT i = 0; i < FRAMES_IN_FLIGHT; ++i)
    {
        if (_meshShaderCBAlloc[i]) { _meshShaderCBAlloc[i]->Release(); _meshShaderCBAlloc[i] = nullptr; }
        _meshShaderCB[i].Reset();
        _meshShaderCBMapped[i] = nullptr;
    }

    if (_meshletBufferAlloc)    { _meshletBufferAlloc->Release();    _meshletBufferAlloc = nullptr; }
    if (_meshletBoundsAlloc)    { _meshletBoundsAlloc->Release();    _meshletBoundsAlloc = nullptr; }
    if (_meshletVertexAlloc)    { _meshletVertexAlloc->Release();    _meshletVertexAlloc = nullptr; }
    if (_meshletTriAlloc)       { _meshletTriAlloc->Release();       _meshletTriAlloc = nullptr; }
    if (_meshletMeshInfoAlloc)  { _meshletMeshInfoAlloc->Release();  _meshletMeshInfoAlloc = nullptr; }
    _meshletBuffer.Reset();
    _meshletBoundsBuffer.Reset();
    _meshletVertexBuffer.Reset();
    _meshletTriBuffer.Reset();
    _meshletMeshInfoBuffer.Reset();
    _meshMeshletOffsets.clear();
    _meshMeshletCounts.clear();
}

// ===========================================================================
// Phase 14: IBL Environment Mapping
// ===========================================================================

// ---------------------------------------------------------------------------
// Helper: execute a command list synchronously (one-shot upload/compute)
// ---------------------------------------------------------------------------
static bool ExecuteCommandListSync(ID3D12Device*        device,
                                   ID3D12CommandQueue*  queue,
                                   ID3D12CommandList*   list)
{
    ComPtr<ID3D12Fence> fence;
    HRESULT hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (FAILED(hr)) return false;

    HANDLE ev = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    if (!ev) return false;

    ID3D12CommandList* lists[] = { list };
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence.Get(), 1);
    fence->SetEventOnCompletion(1, ev);
    WaitForSingleObject(ev, INFINITE);
    CloseHandle(ev);
    return true;
}

// ---------------------------------------------------------------------------
// Create all IBL GPU textures (before precompute dispatches)
// ---------------------------------------------------------------------------
bool DX12Backend::CreateIBLResources()
{
    D3D12MA::ALLOCATION_DESC allocDesc = {};
    allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    auto MakeCube = [&](UINT size, UINT mips, DXGI_FORMAT fmt,
                        D3D12_RESOURCE_STATES initState,
                        ComPtr<ID3D12Resource>& res,
                        D3D12MA::Allocation*&   alloc) -> bool
    {
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width              = size;
        rd.Height             = size;
        rd.DepthOrArraySize   = 6;
        rd.MipLevels          = static_cast<UINT16>(mips);
        rd.Format             = fmt;
        rd.SampleDesc.Count   = 1;
        rd.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rd.Flags              = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        HRESULT hr = _d3d12maAllocator->CreateResource(
            &allocDesc, &rd, initState, nullptr, &alloc, IID_PPV_ARGS(&res));
        return SUCCEEDED(hr);
    };

    // Environment cubemap 512² × 6, no mips needed for skybox (mip=1 here;
    // irradiance + prefilter are separate textures).
    if (!MakeCube(ENV_CUBE_SIZE, 1, DXGI_FORMAT_R16G16B16A16_FLOAT,
                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                  _envCubemap, _envCubemapAlloc))
    {
        LUNA_LOG_ERROR("IBL: failed to create env cubemap");
        return false;
    }
    _envCubemap->SetName(L"IBL_EnvCubemap");

    // Irradiance cubemap 32² × 6
    if (!MakeCube(IRR_CUBE_SIZE, 1, DXGI_FORMAT_R16G16B16A16_FLOAT,
                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                  _irrCubemap, _irrCubemapAlloc))
    {
        LUNA_LOG_ERROR("IBL: failed to create irradiance cubemap");
        return false;
    }
    _irrCubemap->SetName(L"IBL_IrrCubemap");

    // Prefiltered env cubemap 128² × 6, PREFILTER_MIP_COUNT mips
    if (!MakeCube(PREFILTER_CUBE_SIZE, PREFILTER_MIP_COUNT, DXGI_FORMAT_R16G16B16A16_FLOAT,
                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                  _prefilterCubemap, _prefilterCubemapAlloc))
    {
        LUNA_LOG_ERROR("IBL: failed to create prefilter cubemap");
        return false;
    }
    _prefilterCubemap->SetName(L"IBL_PrefilterCubemap");

    // BRDF LUT 512×512, RG16F
    {
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width              = BRDF_LUT_SIZE;
        rd.Height             = BRDF_LUT_SIZE;
        rd.DepthOrArraySize   = 1;
        rd.MipLevels          = 1;
        rd.Format             = DXGI_FORMAT_R16G16_FLOAT;
        rd.SampleDesc.Count   = 1;
        rd.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rd.Flags              = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        HRESULT hr = _d3d12maAllocator->CreateResource(
            &allocDesc, &rd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr, &_brdfLUTAlloc, IID_PPV_ARGS(&_brdfLUT));
        if (FAILED(hr))
        {
            LUNA_LOG_ERROR("IBL: failed to create BRDF LUT");
            return false;
        }
        _brdfLUT->SetName(L"IBL_BrdfLUT");
    }

    // -----------------------------------------------------------------------
    // Non-shader-visible UAV heap for precompute UAV writes
    // Layout (all non-shader-visible for compute dispatch):
    //   [0..5]  = env cubemap face UAVs
    //   [6..11] = irradiance cubemap face UAVs
    //   [12..12+PREFILTER_MIP_COUNT*6-1] = prefilter mip×face UAVs
    //   [last]  = BRDF LUT UAV
    // -----------------------------------------------------------------------
    UINT totalUAVSlots = 6 + 6 + PREFILTER_MIP_COUNT * 6 + 1;

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = totalUAVSlots;
    heapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE; // non-shader-visible
    heapDesc.NodeMask       = 0;
    HRESULT hr = _device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&_iblUavHeap));
    if (FAILED(hr))
    {
        LUNA_LOG_ERROR("IBL: failed to create UAV heap");
        return false;
    }

    auto MakeArraySliceUAV = [&](ID3D12Resource* res, UINT slice, UINT mip,
                                 UINT heapSlot, DXGI_FORMAT fmt)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uvd = {};
        uvd.Format                         = fmt;
        uvd.ViewDimension                  = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        uvd.Texture2DArray.MipSlice        = mip;
        uvd.Texture2DArray.FirstArraySlice = slice;
        uvd.Texture2DArray.ArraySize       = 1;

        D3D12_CPU_DESCRIPTOR_HANDLE cpu;
        cpu.ptr = _iblUavHeap->GetCPUDescriptorHandleForHeapStart().ptr
                  + static_cast<UINT64>(heapSlot) * _srvDescriptorSize;
        _device->CreateUnorderedAccessView(res, nullptr, &uvd, cpu);
    };

    // Env cubemap: 6 face UAVs at slots 0..5
    for (UINT f = 0; f < 6; ++f)
        MakeArraySliceUAV(_envCubemap.Get(), f, 0, f, DXGI_FORMAT_R16G16B16A16_FLOAT);

    // Irradiance cubemap: 6 face UAVs at slots 6..11
    for (UINT f = 0; f < 6; ++f)
        MakeArraySliceUAV(_irrCubemap.Get(), f, 0, 6 + f, DXGI_FORMAT_R16G16B16A16_FLOAT);

    // Prefilter cubemap: mip × face UAVs at slots 12..
    for (UINT m = 0; m < PREFILTER_MIP_COUNT; ++m)
        for (UINT f = 0; f < 6; ++f)
            MakeArraySliceUAV(_prefilterCubemap.Get(), f, m,
                              12 + m * 6 + f, DXGI_FORMAT_R16G16B16A16_FLOAT);

    // BRDF LUT UAV — last slot in non-vis heap (also allocate one in shader-visible heap for dispatch)
    {
        UINT brdfHeapSlot = 12 + PREFILTER_MIP_COUNT * 6;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uvd = {};
        uvd.Format                = DXGI_FORMAT_R16G16_FLOAT;
        uvd.ViewDimension         = D3D12_UAV_DIMENSION_TEXTURE2D;
        uvd.Texture2D.MipSlice    = 0;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu;
        cpu.ptr = _iblUavHeap->GetCPUDescriptorHandleForHeapStart().ptr
                  + static_cast<UINT64>(brdfHeapSlot) * _srvDescriptorSize;
        _device->CreateUnorderedAccessView(_brdfLUT.Get(), nullptr, &uvd, cpu);

        // Also allocate a shader-visible UAV slot for dispatch
        D3D12_CPU_DESCRIPTOR_HANDLE visCPU;
        D3D12_GPU_DESCRIPTOR_HANDLE visGPU;
        _brdfLUTUAVIndex = AllocateSRVSlot(visCPU, visGPU);
        _device->CreateUnorderedAccessView(_brdfLUT.Get(), nullptr, &uvd, visCPU);
    }

    // -----------------------------------------------------------------------
    // Allocate shader-visible SRV slots for runtime IBL sampling
    // -----------------------------------------------------------------------
    auto MakeCubeSRV = [&](ID3D12Resource* res, UINT mips, DXGI_FORMAT fmt, UINT& outIdx)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE cpu; D3D12_GPU_DESCRIPTOR_HANDLE gpu;
        outIdx = AllocateSRVSlot(cpu, gpu);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format                      = fmt;
        srvDesc.ViewDimension               = D3D12_SRV_DIMENSION_TEXTURECUBE;
        srvDesc.Shader4ComponentMapping     = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.TextureCube.MipLevels       = mips;
        srvDesc.TextureCube.MostDetailedMip = 0;
        _device->CreateShaderResourceView(res, &srvDesc, cpu);
    };

    MakeCubeSRV(_envCubemap.Get(),      1,                   DXGI_FORMAT_R16G16B16A16_FLOAT, _envCubemapSRVIndex);
    MakeCubeSRV(_irrCubemap.Get(),      1,                   DXGI_FORMAT_R16G16B16A16_FLOAT, _irrCubemapSRVIndex);
    MakeCubeSRV(_prefilterCubemap.Get(), PREFILTER_MIP_COUNT, DXGI_FORMAT_R16G16B16A16_FLOAT, _prefilterCubemapSRVIndex);

    // BRDF LUT SRV (Texture2D RG16F)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE cpu; D3D12_GPU_DESCRIPTOR_HANDLE gpu;
        _brdfLUTSRVIndex = AllocateSRVSlot(cpu, gpu);
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format                    = DXGI_FORMAT_R16G16_FLOAT;
        srvDesc.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels       = 1;
        srvDesc.Texture2D.MostDetailedMip = 0;
        _device->CreateShaderResourceView(_brdfLUT.Get(), &srvDesc, cpu);
    }

    return true;
}

// ---------------------------------------------------------------------------
// One-time GPU compute precompute: equirect→cube, irr, prefilter, brdfLUT
// ---------------------------------------------------------------------------
bool DX12Backend::DispatchIBLPrecompute()
{
    // Create a dedicated command allocator + command list for precompute
    ComPtr<ID3D12CommandAllocator>    cmdAlloc;
    ComPtr<ID3D12GraphicsCommandList> cmdList;
    HRESULT hr = _device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAlloc));
    if (FAILED(hr)) { LUNA_LOG_ERROR("IBL: CreateCommandAllocator failed"); return false; }
    hr = _device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                    cmdAlloc.Get(), nullptr, IID_PPV_ARGS(&cmdList));
    if (FAILED(hr)) { LUNA_LOG_ERROR("IBL: CreateCommandList failed"); return false; }

    ID3D12DescriptorHeap* heaps[] = { _imGuiSrvHeap.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);

    auto GpuHandle = [&](UINT idx) -> D3D12_GPU_DESCRIPTOR_HANDLE {
        D3D12_GPU_DESCRIPTOR_HANDLE h;
        h.ptr = _imGuiSrvHeap->GetGPUDescriptorHandleForHeapStart().ptr
                + static_cast<UINT64>(idx) * _srvDescriptorSize;
        return h;
    };

    // UAV heap (non-shader-visible) CPU handle helpers
    auto UavCpuNonVis = [&](UINT slot) -> D3D12_CPU_DESCRIPTOR_HANDLE {
        D3D12_CPU_DESCRIPTOR_HANDLE h;
        h.ptr = _iblUavHeap->GetCPUDescriptorHandleForHeapStart().ptr
                + static_cast<UINT64>(slot) * _srvDescriptorSize;
        return h;
    };

    // -----------------------------------------------------------------------
    // Pass 1: Equirectangular → Environment Cubemap
    // Requires _equirectTex to be in PIXEL_SHADER_RESOURCE state
    // -----------------------------------------------------------------------
    if (_equirectTex)
    {
        // Transition equirect to SRV, env cube faces remain UAV
        D3D12_RESOURCE_BARRIER barriers[1] = {
            CD3DX12_RESOURCE_BARRIER::Transition(_equirectTex.Get(),
                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
        };
        cmdList->ResourceBarrier(1, barriers);

        cmdList->SetComputeRootSignature(_equirectToCubePipeline->GetRootSignature().Get());
        cmdList->SetPipelineState(_equirectToCubePipeline->GetPSO());

        // Allocate shader-visible SRV for equirect
        // (the texture was uploaded externally via _equirectTex)
        D3D12_CPU_DESCRIPTOR_HANDLE eqCPU; D3D12_GPU_DESCRIPTOR_HANDLE eqGPU;
        UINT eqSRVIdx = AllocateSRVSlot(eqCPU, eqGPU);
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format                    = DXGI_FORMAT_R32G32B32A32_FLOAT;
        srvDesc.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels       = 1;
        srvDesc.Texture2D.MostDetailedMip = 0;
        _device->CreateShaderResourceView(_equirectTex.Get(), &srvDesc, eqCPU);

        // Dispatch one face at a time (alternately, dispatch all 6 with RWTexture2DArray)
        // Here we dispatch all 6 in one call using the full array
        D3D12_CPU_DESCRIPTOR_HANDLE allFaceCPU; D3D12_GPU_DESCRIPTOR_HANDLE allFaceGPU;
        UINT allFaceIdx = AllocateSRVSlot(allFaceCPU, allFaceGPU);
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uvd = {};
            uvd.Format                         = DXGI_FORMAT_R16G16B16A16_FLOAT;
            uvd.ViewDimension                  = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            uvd.Texture2DArray.MipSlice        = 0;
            uvd.Texture2DArray.FirstArraySlice = 0;
            uvd.Texture2DArray.ArraySize       = 6;
            _device->CreateUnorderedAccessView(_envCubemap.Get(), nullptr, &uvd, allFaceCPU);
        }

        UINT cbData[4] = { ENV_CUBE_SIZE, 0, 0, 0 };
        cmdList->SetComputeRoot32BitConstants(0, 4, cbData, 0);
        cmdList->SetComputeRootDescriptorTable(1, GpuHandle(eqSRVIdx));
        cmdList->SetComputeRootDescriptorTable(2, GpuHandle(allFaceIdx));

        UINT groups = (ENV_CUBE_SIZE + 7) / 8;
        cmdList->Dispatch(groups, groups, 6);
    }
    else
    {
        // No HDR file — leave env cubemap as black (will use solid sky color in skybox)
        LUNA_LOG_WARN("IBL: no equirect texture — environment map will be black");
    }

    // Transition env cubemap UAV → SRV for subsequent irradiance + prefilter passes
    {
        D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
            _envCubemap.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cmdList->ResourceBarrier(1, &b);
    }

    // -----------------------------------------------------------------------
    // Pass 2: Irradiance Convolution
    // -----------------------------------------------------------------------
    {
        // Allocate shader-visible SRV for envCube as TextureCube
        D3D12_CPU_DESCRIPTOR_HANDLE envSrvCPU; D3D12_GPU_DESCRIPTOR_HANDLE envSrvGPU;
        UINT envSRVTmp = AllocateSRVSlot(envSrvCPU, envSrvGPU);
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format                      = DXGI_FORMAT_R16G16B16A16_FLOAT;
        srvDesc.ViewDimension               = D3D12_SRV_DIMENSION_TEXTURECUBE;
        srvDesc.Shader4ComponentMapping     = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.TextureCube.MipLevels       = 1;
        srvDesc.TextureCube.MostDetailedMip = 0;
        _device->CreateShaderResourceView(_envCubemap.Get(), &srvDesc, envSrvCPU);

        // All-face irradiance UAV
        D3D12_CPU_DESCRIPTOR_HANDLE irrAllCPU; D3D12_GPU_DESCRIPTOR_HANDLE irrAllGPU;
        UINT irrAllIdx = AllocateSRVSlot(irrAllCPU, irrAllGPU);
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uvd = {};
            uvd.Format                         = DXGI_FORMAT_R16G16B16A16_FLOAT;
            uvd.ViewDimension                  = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            uvd.Texture2DArray.MipSlice        = 0;
            uvd.Texture2DArray.FirstArraySlice = 0;
            uvd.Texture2DArray.ArraySize       = 6;
            _device->CreateUnorderedAccessView(_irrCubemap.Get(), nullptr, &uvd, irrAllCPU);
        }

        cmdList->SetComputeRootSignature(_irrConvPipeline->GetRootSignature().Get());
        cmdList->SetPipelineState(_irrConvPipeline->GetPSO());

        UINT cbData[4] = { IRR_CUBE_SIZE, 0, 0, 0 };
        cmdList->SetComputeRoot32BitConstants(0, 4, cbData, 0);
        cmdList->SetComputeRootDescriptorTable(1, GpuHandle(envSRVTmp));
        cmdList->SetComputeRootDescriptorTable(2, GpuHandle(irrAllIdx));

        UINT groups = (IRR_CUBE_SIZE + 7) / 8;
        cmdList->Dispatch(groups, groups, 6);
    }

    // Transition irradiance UAV → SRV for runtime
    {
        D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
            _irrCubemap.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdList->ResourceBarrier(1, &b);
    }

    // -----------------------------------------------------------------------
    // Pass 3: Prefiltered Environment Map (per mip level)
    // -----------------------------------------------------------------------
    {
        D3D12_CPU_DESCRIPTOR_HANDLE envSrvCPU2; D3D12_GPU_DESCRIPTOR_HANDLE envSrvGPU2;
        UINT envSRVTmp2 = AllocateSRVSlot(envSrvCPU2, envSrvGPU2);
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format                      = DXGI_FORMAT_R16G16B16A16_FLOAT;
            srvDesc.ViewDimension               = D3D12_SRV_DIMENSION_TEXTURECUBE;
            srvDesc.Shader4ComponentMapping     = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.TextureCube.MipLevels       = 1;
            srvDesc.TextureCube.MostDetailedMip = 0;
            _device->CreateShaderResourceView(_envCubemap.Get(), &srvDesc, envSrvCPU2);
        }

        cmdList->SetComputeRootSignature(_prefilterPipeline->GetRootSignature().Get());
        cmdList->SetPipelineState(_prefilterPipeline->GetPSO());

        for (UINT m = 0; m < PREFILTER_MIP_COUNT; ++m)
        {
            UINT mipSize    = PREFILTER_CUBE_SIZE >> m;
            float roughness = (PREFILTER_MIP_COUNT > 1)
                              ? static_cast<float>(m) / static_cast<float>(PREFILTER_MIP_COUNT - 1)
                              : 0.0f;

            // All-face UAV for this mip
            D3D12_CPU_DESCRIPTOR_HANDLE pfAllCPU; D3D12_GPU_DESCRIPTOR_HANDLE pfAllGPU;
            UINT pfAllIdx = AllocateSRVSlot(pfAllCPU, pfAllGPU);
            {
                D3D12_UNORDERED_ACCESS_VIEW_DESC uvd = {};
                uvd.Format                         = DXGI_FORMAT_R16G16B16A16_FLOAT;
                uvd.ViewDimension                  = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                uvd.Texture2DArray.MipSlice        = m;
                uvd.Texture2DArray.FirstArraySlice = 0;
                uvd.Texture2DArray.ArraySize       = 6;
                _device->CreateUnorderedAccessView(_prefilterCubemap.Get(), nullptr, &uvd, pfAllCPU);
            }

            // Pass roughness as bit-cast uint (HLSL: asfloat on the uint = roughness)
            UINT roughnessBits;
            memcpy(&roughnessBits, &roughness, sizeof(float));
            UINT cbData[4] = { mipSize, m, roughnessBits, 1024u };
            cmdList->SetComputeRoot32BitConstants(0, 4, cbData, 0);
            cmdList->SetComputeRootDescriptorTable(1, GpuHandle(envSRVTmp2));
            cmdList->SetComputeRootDescriptorTable(2, GpuHandle(pfAllIdx));

            UINT groups = ((mipSize + 7u) / 8u < 1u) ? 1u : (mipSize + 7u) / 8u;
            cmdList->Dispatch(groups, groups, 6);
        }
    }

    // Transition prefilter UAV → SRV
    {
        D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
            _prefilterCubemap.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdList->ResourceBarrier(1, &b);
    }

    // -----------------------------------------------------------------------
    // Pass 4: BRDF Integration LUT
    // -----------------------------------------------------------------------
    {
        cmdList->SetComputeRootSignature(_brdfLutPipeline->GetRootSignature().Get());
        cmdList->SetPipelineState(_brdfLutPipeline->GetPSO());

        cmdList->SetComputeRootDescriptorTable(0, GpuHandle(_brdfLUTUAVIndex));

        UINT groups = (BRDF_LUT_SIZE + 15) / 16;
        cmdList->Dispatch(groups, groups, 1);
    }

    // Transition BRDF LUT UAV → SRV
    {
        D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
            _brdfLUT.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdList->ResourceBarrier(1, &b);
    }

    // Transition env cubemap to PIXEL_SHADER_RESOURCE for runtime skybox
    {
        D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
            _envCubemap.Get(),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdList->ResourceBarrier(1, &b);
    }

    cmdList->Close();
    if (!ExecuteCommandListSync(_device.Get(), _commandQueue.Get(), cmdList.Get()))
    {
        LUNA_LOG_ERROR("IBL: precompute GPU execution failed");
        return false;
    }

    // Release equirect source — no longer needed
    if (_equirectTexAlloc) { _equirectTexAlloc->Release(); _equirectTexAlloc = nullptr; }
    _equirectTex.Reset();

    LUNA_LOG_INFO("IBL: precompute complete (env=%u², irr=%u², prefilter=%u²×%u mips, brdfLUT=%u²)",
                  ENV_CUBE_SIZE, IRR_CUBE_SIZE, PREFILTER_CUBE_SIZE, PREFILTER_MIP_COUNT, BRDF_LUT_SIZE);
    return true;
}

// ---------------------------------------------------------------------------
// Public API: load HDR file, upload to GPU, run precompute
// ---------------------------------------------------------------------------
bool DX12Backend::LoadHDREnvironment(const std::string& hdrPath)
{
    // -----------------------------------------------------------------------
    // Step 1: load equirectangular HDR image via stb_image
    // -----------------------------------------------------------------------
    stbi_set_flip_vertically_on_load(0);
    int w = 0, h = 0, ch = 0;
    float* pixels = stbi_loadf(hdrPath.c_str(), &w, &h, &ch, 4);
    if (!pixels)
    {
        LUNA_LOG_ERROR("IBL: stbi_loadf failed for '%s': %s", hdrPath.c_str(), stbi_failure_reason());
        return false;
    }
    LUNA_LOG_INFO("IBL: loaded '%s' (%d×%d)", hdrPath.c_str(), w, h);

    // -----------------------------------------------------------------------
    // Step 2: upload float4 pixels to a GPU texture (RGBA32F)
    // -----------------------------------------------------------------------
    {
        D3D12MA::ALLOCATION_DESC uploadDesc = {};
        uploadDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;
        D3D12MA::ALLOCATION_DESC defaultDesc = {};
        defaultDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC texDesc = {};
        texDesc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width              = static_cast<UINT>(w);
        texDesc.Height             = static_cast<UINT>(h);
        texDesc.DepthOrArraySize   = 1;
        texDesc.MipLevels          = 1;
        texDesc.Format             = DXGI_FORMAT_R32G32B32A32_FLOAT;
        texDesc.SampleDesc.Count   = 1;
        texDesc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        texDesc.Flags              = D3D12_RESOURCE_FLAG_NONE;

        HRESULT hr = _d3d12maAllocator->CreateResource(&defaultDesc, &texDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, &_equirectTexAlloc, IID_PPV_ARGS(&_equirectTex));
        if (FAILED(hr))
        {
            stbi_image_free(pixels);
            LUNA_LOG_ERROR("IBL: failed to create equirect GPU texture");
            return false;
        }
        _equirectTex->SetName(L"IBL_Equirect");

        // Upload via intermediary
        UINT64 uploadSize = GetRequiredIntermediateSize(_equirectTex.Get(), 0, 1);

        D3D12_RESOURCE_DESC uploadBufDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadSize);
        ComPtr<ID3D12Resource>  uploadBuf;
        D3D12MA::Allocation*    uploadBufAlloc = nullptr;
        hr = _d3d12maAllocator->CreateResource(&uploadDesc, &uploadBufDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, &uploadBufAlloc, IID_PPV_ARGS(&uploadBuf));
        if (FAILED(hr))
        {
            stbi_image_free(pixels);
            LUNA_LOG_ERROR("IBL: failed to create equirect upload buffer");
            return false;
        }

        // One-shot command list for upload
        ComPtr<ID3D12CommandAllocator>    upAlloc;
        ComPtr<ID3D12GraphicsCommandList> upList;
        _device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&upAlloc));
        _device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, upAlloc.Get(), nullptr, IID_PPV_ARGS(&upList));

        D3D12_SUBRESOURCE_DATA subData = {};
        subData.pData      = pixels;
        subData.RowPitch   = static_cast<LONG_PTR>(w) * 4 * sizeof(float);
        subData.SlicePitch = subData.RowPitch * h;

        UpdateSubresources(upList.Get(), _equirectTex.Get(), uploadBuf.Get(), 0, 0, 1, &subData);

        upList->Close();
        ExecuteCommandListSync(_device.Get(), _commandQueue.Get(), upList.Get());
        uploadBufAlloc->Release();
    }
    stbi_image_free(pixels);

    // -----------------------------------------------------------------------
    // Step 3: Create IBL GPU textures (cubemaps, LUT)
    // -----------------------------------------------------------------------
    if (!CreateIBLResources()) return false;

    // -----------------------------------------------------------------------
    // Step 4: Compile IBL compute + skybox pipelines if not already compiled
    // -----------------------------------------------------------------------
    auto CompileCompute = [&](std::unique_ptr<DX12Pipeline>& pipe,
                              const wchar_t* csName, RootSignatureLayout layout) -> bool
    {
        pipe = std::make_unique<DX12Pipeline>();
        PipelineStateDesc d;
        d.computeShader = true;
        d.rootLayout    = layout;
        return pipe->Initialize(_device, csName, L"", d);
    };

    if (!CompileCompute(_equirectToCubePipeline, L"equirect_to_cube.comp.hlsl", RootSignatureLayout::EquirectToCube))
    { LUNA_LOG_ERROR("IBL: equirect_to_cube pipeline failed"); return false; }

    if (!CompileCompute(_irrConvPipeline, L"irradiance_conv.comp.hlsl", RootSignatureLayout::IrradianceConv))
    { LUNA_LOG_ERROR("IBL: irradiance_conv pipeline failed"); return false; }

    if (!CompileCompute(_prefilterPipeline, L"prefilter_env.comp.hlsl", RootSignatureLayout::PrefilterEnv))
    { LUNA_LOG_ERROR("IBL: prefilter_env pipeline failed"); return false; }

    if (!CompileCompute(_brdfLutPipeline, L"brdf_lut.comp.hlsl", RootSignatureLayout::BrdfLut))
    { LUNA_LOG_ERROR("IBL: brdf_lut pipeline failed"); return false; }

    // Skybox graphics pipeline
    {
        _skyboxPipeline = std::make_unique<DX12Pipeline>();
        PipelineStateDesc skyDesc;
        skyDesc.noInputLayout    = true;
        skyDesc.rootLayout       = RootSignatureLayout::Skybox;
        skyDesc.numRenderTargets = 1;
        skyDesc.rtvFormats[0]    = DXGI_FORMAT_R16G16B16A16_FLOAT;
        skyDesc.enableDepthTest  = true; // LESS_EQUAL + no write — handled in CreatePipelineState
        if (!_skyboxPipeline->Initialize(_device, L"skybox.vert.hlsl", L"skybox.frag.hlsl", skyDesc))
        { LUNA_LOG_ERROR("IBL: skybox pipeline failed"); return false; }
    }

    // IBL deferred lighting pipeline (HDR output + IBL)
    {
        _lightingPipelineIBL = std::make_unique<DX12Pipeline>();
        PipelineStateDesc ld;
        ld.rootLayout       = RootSignatureLayout::DeferredLightingIBL;
        ld.noInputLayout    = true;
        ld.numRenderTargets = 1;
        ld.rtvFormats[0]    = DXGI_FORMAT_R16G16B16A16_FLOAT;
        if (!_lightingPipelineIBL->Initialize(_device, L"fullscreen.vert.hlsl",
                                               L"deferred_lighting_ibl.frag.hlsl", ld))
        {
            LUNA_LOG_ERROR("IBL: deferred_lighting_ibl pipeline FAILED — IBL lighting disabled, falling back to HDR");
            _lightingPipelineIBL.reset();
        }
        else
        {
            LUNA_LOG_INFO("IBL: deferred_lighting_ibl pipeline ready");
        }
    }

    // -----------------------------------------------------------------------
    // Step 5: Dispatch precompute
    // -----------------------------------------------------------------------
    if (!DispatchIBLPrecompute()) return false;

    _iblReady = true;
    LUNA_LOG_INFO("IBL environment loaded: '%s'", hdrPath.c_str());
    return true;
}

// ---------------------------------------------------------------------------
// Phase 16B: SSR resources
// ---------------------------------------------------------------------------
bool DX12Backend::CreateSSRResources()
{
    const UINT W = (UINT)_screenWidth;
    const UINT H = (UINT)_screenHeight;

    // 1. SSR render target (R16G16B16A16_FLOAT, UAV + SRV, full-res)
    {
        D3D12MA::ALLOCATION_DESC ad = {}; ad.HeapType = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R16G16B16A16_FLOAT, W, H, 1, 1, 1, 0,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (FAILED(_d3d12maAllocator->CreateResource(
                &ad, &rd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                &_ssrRTAlloc, IID_PPV_ARGS(&_ssrRT))))
        { LUNA_LOG_ERROR("Phase 16B: SSR RT alloc failed"); return false; }
        _ssrRT->SetName(L"SSR_RT");
    }

    // 2. UAV descriptor
    {
        D3D12_CPU_DESCRIPTOR_HANDLE cpuH; D3D12_GPU_DESCRIPTOR_HANDLE gpuH;
        _ssrUAVSRVIndex = AllocateSRVSlot(cpuH, gpuH);
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format        = DXGI_FORMAT_R16G16B16A16_FLOAT;
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        _device->CreateUnorderedAccessView(_ssrRT.Get(), nullptr, &ud, cpuH);
    }

    // 3. SRV descriptor (for blend pass)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE cpuH; D3D12_GPU_DESCRIPTOR_HANDLE gpuH;
        _ssrSRVIndex = AllocateSRVSlot(cpuH, gpuH);
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format                    = DXGI_FORMAT_R16G16B16A16_FLOAT;
        sd.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels       = 1;
        _device->CreateShaderResourceView(_ssrRT.Get(), &sd, cpuH);
    }

    // 4. Per-frame SSR constant buffers (256B, persistently mapped)
    for (UINT i = 0; i < FRAMES_IN_FLIGHT; ++i)
    {
        D3D12MA::ALLOCATION_DESC ad = {}; ad.HeapType = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC      cd = CD3DX12_RESOURCE_DESC::Buffer(256);
        if (FAILED(_d3d12maAllocator->CreateResource(
                &ad, &cd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                &_ssrCBAlloc[i], IID_PPV_ARGS(&_ssrCB[i]))))
        { LUNA_LOG_ERROR("Phase 16B: SSR CB alloc failed (frame %u)", i); return false; }
        _ssrCB[i]->Map(0, nullptr, &_ssrCBMapped[i]);
    }

    // 5. SSR compute pipeline
    _ssrComputePipeline = std::make_unique<DX12Pipeline>();
    {
        PipelineStateDesc d;
        d.rootLayout    = RootSignatureLayout::SSRCompute;
        d.computeShader = true;
        if (!_ssrComputePipeline->Initialize(_device, L"ssr.comp.hlsl", L"", d))
        {
            LUNA_LOG_ERROR("Phase 16B: SSR compute pipeline init failed");
            _ssrComputePipeline.reset();
            return false;
        }
    }

    // 6. SSR blend pipeline (additive onto _hdrRT)
    _ssrBlendPipeline = std::make_unique<DX12Pipeline>();
    {
        PipelineStateDesc d;
        d.rootLayout       = RootSignatureLayout::SSRBlend;
        d.noInputLayout    = true;
        d.numRenderTargets = 1;
        d.rtvFormats[0]    = DXGI_FORMAT_R16G16B16A16_FLOAT;
        if (!_ssrBlendPipeline->Initialize(_device,
                L"fullscreen.vert.hlsl", L"ssr_blend.frag.hlsl", d))
        {
            LUNA_LOG_ERROR("Phase 16B: SSR blend pipeline init failed");
            _ssrBlendPipeline.reset();
            return false;
        }
    }

    _ssrRTFirstFrame = true;
    LUNA_LOG_INFO("Phase 16B: SSR resources created (%ux%u)", W, H);
    return true;
}

void DX12Backend::DestroySSRResources()
{
    for (UINT i = 0; i < FRAMES_IN_FLIGHT; ++i)
    {
        if (_ssrCBMapped[i]) { _ssrCB[i]->Unmap(0, nullptr); _ssrCBMapped[i] = nullptr; }
        if (_ssrCBAlloc[i])  { _ssrCBAlloc[i]->Release();    _ssrCBAlloc[i]  = nullptr; }
        _ssrCB[i].Reset();
    }
    if (_ssrRTAlloc) { _ssrRTAlloc->Release(); _ssrRTAlloc = nullptr; }
    _ssrRT.Reset();
    _ssrUAVSRVIndex = UINT_MAX;
    _ssrSRVIndex    = UINT_MAX;
    _ssrComputePipeline.reset();
    _ssrBlendPipeline.reset();
    _ssrRTFirstFrame = true;
}

// ---------------------------------------------------------------------------
// Phase 16B: SSR pass — dispatches ray-march compute, then additively blends
// Entry state: _hdrRT=RENDER_TARGET, depth=DEPTH_WRITE, gbuf[1,2]=RENDER_TARGET
// Exit  state: _hdrRT=RENDER_TARGET (with SSR blended in), depth=DEPTH_WRITE
// ---------------------------------------------------------------------------
void DX12Backend::DrawSSRPass()
{
    if (!_ssrComputePipeline || !_ssrBlendPipeline || !_ssrRT) return;
    if (_ssrUAVSRVIndex == UINT_MAX || _ssrSRVIndex == UINT_MAX)         return;
    if (_gbufferSRVIndex[1] == UINT_MAX || _gbufferSRVIndex[2] == UINT_MAX) return;
    if (_depthSRVIndex == UINT_MAX || _hdrSRVIndex == UINT_MAX)           return;

    auto* cmd = _commandList.Get();
    UINT  W   = (UINT)_screenWidth;
    UINT  H   = (UINT)_screenHeight;
    const D3D12_RESOURCE_STATES SRV_BOTH = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                                         | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    // ── Pre-compute barriers ─────────────────────────────────────────────────
    D3D12_RESOURCE_BARRIER pre[5];
    UINT numPre = 0;
    pre[numPre++] = CD3DX12_RESOURCE_BARRIER::Transition(
        _hdrRT.Get(),       D3D12_RESOURCE_STATE_RENDER_TARGET, SRV_BOTH);
    pre[numPre++] = CD3DX12_RESOURCE_BARRIER::Transition(
        _depthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    pre[numPre++] = CD3DX12_RESOURCE_BARRIER::Transition(
        _gbuffer[1].Get(),  D3D12_RESOURCE_STATE_RENDER_TARGET, SRV_BOTH);
    pre[numPre++] = CD3DX12_RESOURCE_BARRIER::Transition(
        _gbuffer[2].Get(),  D3D12_RESOURCE_STATE_RENDER_TARGET, SRV_BOTH);
    if (!_ssrRTFirstFrame)
        pre[numPre++] = CD3DX12_RESOURCE_BARRIER::Transition(
            _ssrRT.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmd->ResourceBarrier(numPre, pre);

    // ── Upload SSR constants ──────────────────────────────────────────────────
    struct SSRConstants
    {
        XMFLOAT4X4 view;
        XMFLOAT4X4 proj;
        XMFLOAT4X4 invViewProj;
        float      eyePos[3];   float maxDistance;
        UINT       screenW;     UINT  screenH;     UINT  maxSteps; float stepSize;
        float      thickness;   float maxRoughness; float _pad0[2];
        float      _pad1[4];
    };
    static_assert(sizeof(SSRConstants) == 256, "SSRConstants must be 256B");

    XMFLOAT4X4 viewF = _lastView;
    XMFLOAT4X4 projF = _lastProj;
    XMMATRIX   VP    = XMMatrixMultiply(XMLoadFloat4x4(&viewF), XMLoadFloat4x4(&projF));

    float negTx = -viewF._41, negTy = -viewF._42, negTz = -viewF._43;

    SSRConstants cb = {};
    cb.view         = viewF;
    cb.proj         = projF;
    XMStoreFloat4x4(&cb.invViewProj, XMMatrixInverse(nullptr, VP));
    cb.eyePos[0]    = viewF._11 * negTx + viewF._12 * negTy + viewF._13 * negTz;
    cb.eyePos[1]    = viewF._21 * negTx + viewF._22 * negTy + viewF._23 * negTz;
    cb.eyePos[2]    = viewF._31 * negTx + viewF._32 * negTy + viewF._33 * negTz;
    cb.maxDistance  = 100.0f;
    cb.screenW      = W;
    cb.screenH      = H;
    cb.maxSteps     = 32;
    cb.stepSize     = 0.05f;
    cb.thickness    = 0.1f;
    cb.maxRoughness = 0.6f;
    memcpy(_ssrCBMapped[_frameIndex], &cb, sizeof(cb));

    // ── Dispatch SSR compute ─────────────────────────────────────────────────
    ID3D12DescriptorHeap* heaps[] = { _imGuiSrvHeap.Get() };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetPipelineState(_ssrComputePipeline->GetPSO());
    cmd->SetComputeRootSignature(_ssrComputePipeline->GetRootSignature().Get());
    cmd->SetComputeRootConstantBufferView(0, _ssrCB[_frameIndex]->GetGPUVirtualAddress());
    cmd->SetComputeRootDescriptorTable(1, PPSRVHandle(_imGuiSrvHeap, _depthSRVIndex,      _srvDescriptorSize));
    cmd->SetComputeRootDescriptorTable(2, PPSRVHandle(_imGuiSrvHeap, _gbufferSRVIndex[1], _srvDescriptorSize));
    cmd->SetComputeRootDescriptorTable(3, PPSRVHandle(_imGuiSrvHeap, _gbufferSRVIndex[2], _srvDescriptorSize));
    cmd->SetComputeRootDescriptorTable(4, PPSRVHandle(_imGuiSrvHeap, _hdrSRVIndex,        _srvDescriptorSize));
    cmd->SetComputeRootDescriptorTable(5, PPSRVHandle(_imGuiSrvHeap, _ssrUAVSRVIndex,     _srvDescriptorSize));
    cmd->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
    _ssrRTFirstFrame = false;

    // ── UAV barrier + post-compute transitions ───────────────────────────────
    D3D12_RESOURCE_BARRIER uavB = CD3DX12_RESOURCE_BARRIER::UAV(_ssrRT.Get());
    cmd->ResourceBarrier(1, &uavB);

    D3D12_RESOURCE_BARRIER post[] = {
        CD3DX12_RESOURCE_BARRIER::Transition(
            _ssrRT.Get(),     D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        CD3DX12_RESOURCE_BARRIER::Transition(
            _hdrRT.Get(),     SRV_BOTH, D3D12_RESOURCE_STATE_RENDER_TARGET),
        CD3DX12_RESOURCE_BARRIER::Transition(
            _depthBuffer.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_DEPTH_WRITE),
        CD3DX12_RESOURCE_BARRIER::Transition(
            _gbuffer[1].Get(), SRV_BOTH, D3D12_RESOURCE_STATE_RENDER_TARGET),
        CD3DX12_RESOURCE_BARRIER::Transition(
            _gbuffer[2].Get(), SRV_BOTH, D3D12_RESOURCE_STATE_RENDER_TARGET),
    };
    cmd->ResourceBarrier(_countof(post), post);

    // ── Additive SSR blend into _hdrRT ───────────────────────────────────────
    BindPPPipeline(cmd, _ssrBlendPipeline.get(), _imGuiSrvHeap, _hdrRTV, W, H);
    cmd->SetGraphicsRootDescriptorTable(
        0, PPSRVHandle(_imGuiSrvHeap, _ssrSRVIndex, _srvDescriptorSize));
    cmd->DrawInstanced(3, 1, 0, 0);
    // _hdrRT remains RENDER_TARGET for subsequent DrawTAAPass
}

// ---------------------------------------------------------------------------
// Skybox pass — draws behind geometry using depth=1.0 / LESS_EQUAL
// Called from CompositeFrame() after deferred lighting, before post-process
// ---------------------------------------------------------------------------
void DX12Backend::DrawSkyboxPass()
{
    if (!_iblReady || !_skyboxPipeline || _envCubemapSRVIndex == UINT_MAX) return;
    if (!_ppResourcesValid || !_hdrRT) return; // need HDR RT

    // Bind HDR RT + depth (depth for LESS_EQUAL test)
    _commandList->OMSetRenderTargets(1, &_hdrRTV, FALSE, &_dsvHandle);
    _commandList->RSSetViewports(1, &_screenViewport);
    _commandList->RSSetScissorRects(1, &_scissorRect);

    // Build invViewProj (unjittered) — use _ppPrevVP as reference for "current frame unjittered"
    // Actually use the cached _lastView / _lastProj from UpdateMVP
    XMMATRIX view = XMLoadFloat4x4(&_lastView);
    XMMATRIX proj = XMLoadFloat4x4(&_lastProj);
    // Remove translation from view for skybox (only rotation)
    view.r[3] = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
    XMMATRIX vp        = XMMatrixMultiply(view, proj);
    XMMATRIX invVP     = XMMatrixInverse(nullptr, vp);
    XMFLOAT4X4 invVPf;
    XMStoreFloat4x4(&invVPf, XMMatrixTranspose(invVP));

    ID3D12DescriptorHeap* heaps[] = { _imGuiSrvHeap.Get() };
    _commandList->SetDescriptorHeaps(1, heaps);

    BindPipeline(_skyboxPipeline.get());
    _commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // 16 root constants = invViewProj (row-major float4x4)
    _commandList->SetGraphicsRoot32BitConstants(0, 16, &invVPf, 0);

    D3D12_GPU_DESCRIPTOR_HANDLE envGpu;
    envGpu.ptr = _imGuiSrvHeap->GetGPUDescriptorHandleForHeapStart().ptr
                 + static_cast<UINT64>(_envCubemapSRVIndex) * _srvDescriptorSize;
    _commandList->SetGraphicsRootDescriptorTable(1, envGpu);

    _commandList->DrawInstanced(3, 1, 0, 0);
}

// ---------------------------------------------------------------------------
// Cleanup
// ---------------------------------------------------------------------------
void DX12Backend::DestroyIBLResources()
{
    if (_equirectTexAlloc) { _equirectTexAlloc->Release(); _equirectTexAlloc = nullptr; }
    _equirectTex.Reset();

    if (_envCubemapAlloc)     { _envCubemapAlloc->Release();     _envCubemapAlloc     = nullptr; }
    if (_irrCubemapAlloc)     { _irrCubemapAlloc->Release();     _irrCubemapAlloc     = nullptr; }
    if (_prefilterCubemapAlloc){ _prefilterCubemapAlloc->Release();_prefilterCubemapAlloc = nullptr; }
    if (_brdfLUTAlloc)        { _brdfLUTAlloc->Release();        _brdfLUTAlloc        = nullptr; }

    _envCubemap.Reset();
    _irrCubemap.Reset();
    _prefilterCubemap.Reset();
    _brdfLUT.Reset();
    _iblUavHeap.Reset();

    _envCubemapSRVIndex      = UINT_MAX;
    _irrCubemapSRVIndex      = UINT_MAX;
    _prefilterCubemapSRVIndex= UINT_MAX;
    _brdfLUTSRVIndex         = UINT_MAX;
    _brdfLUTUAVIndex         = UINT_MAX;
    _iblReady                = false;
}

// ===========================================================================
// Phase 18B: Motion Blur
// ===========================================================================

bool DX12Backend::CreateMotionBlurResources()
{
    const UINT W = (UINT)_screenWidth;
    const UINT H = (UINT)_screenHeight;

    // 1. Dedicated 1-slot RTV heap
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 1;
        if (FAILED(_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&_motionBlurRtvHeap))))
        { LUNA_LOG_ERROR("Phase 18B: Motion blur RTV heap failed"); return false; }
        _motionBlurRTV = _motionBlurRtvHeap->GetCPUDescriptorHandleForHeapStart();
    }

    // 2. Full-res RGBA16F render target
    if (!AllocPPTarget(_device.Get(), _d3d12maAllocator,
            W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, L"MotionBlur_RT",
            &_motionBlurRTAlloc, _motionBlurRT,
            _motionBlurRTV, _motionBlurSRVIndex, this))
    { LUNA_LOG_ERROR("Phase 18B: Motion blur RT alloc failed"); return false; }

    // 3. Per-frame constant buffers (256 B, UPLOAD, persistently mapped)
    for (UINT i = 0; i < FRAMES_IN_FLIGHT; ++i)
    {
        D3D12MA::ALLOCATION_DESC ad = {}; ad.HeapType = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC      cd = CD3DX12_RESOURCE_DESC::Buffer(256);
        if (FAILED(_d3d12maAllocator->CreateResource(&ad, &cd,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                &_motionBlurCBAlloc[i], IID_PPV_ARGS(&_motionBlurCB[i]))))
        { LUNA_LOG_ERROR("Phase 18B: Motion blur CB alloc failed (frame %u)", i); return false; }
        _motionBlurCB[i]->Map(0, nullptr, &_motionBlurCBMapped[i]);
    }

    // 4. Motion blur pipeline
    _motionBlurPipeline = std::make_unique<DX12Pipeline>();
    {
        PipelineStateDesc d;
        d.rootLayout       = RootSignatureLayout::MotionBlur;
        d.noInputLayout    = true;
        d.numRenderTargets = 1;
        d.rtvFormats[0]    = DXGI_FORMAT_R16G16B16A16_FLOAT;
        if (!_motionBlurPipeline->Initialize(_device,
                L"fullscreen.vert.hlsl", L"motion_blur.frag.hlsl", d))
        {
            LUNA_LOG_ERROR("Phase 18B: Motion blur pipeline init failed");
            _motionBlurPipeline.reset();
            return false;
        }
    }

    LUNA_LOG_INFO("Phase 18B: Motion blur resources created (%ux%u)", W, H);
    return true;
}

void DX12Backend::DestroyMotionBlurResources()
{
    for (UINT i = 0; i < FRAMES_IN_FLIGHT; ++i)
    {
        if (_motionBlurCBMapped[i]) { _motionBlurCB[i]->Unmap(0, nullptr); _motionBlurCBMapped[i] = nullptr; }
        if (_motionBlurCBAlloc[i])  { _motionBlurCBAlloc[i]->Release();    _motionBlurCBAlloc[i]  = nullptr; }
        _motionBlurCB[i].Reset();
    }
    if (_motionBlurRTAlloc) { _motionBlurRTAlloc->Release(); _motionBlurRTAlloc = nullptr; }
    _motionBlurRT.Reset();
    _motionBlurSRVIndex = UINT_MAX;
    _motionBlurRtvHeap.Reset();
    _motionBlurPipeline.reset();
}

// ---------------------------------------------------------------------------
// Phase 18B: Motion blur pass
// Entry:  _hdrRT=RENDER_TARGET (SSR blended in), depth=DEPTH_WRITE
// Exit:   _hdrRT stays SRV, depth=DEPTH_WRITE, _motionBlurRT=SRV (ready for TAA)
// ---------------------------------------------------------------------------
void DX12Backend::DrawMotionBlurPass()
{
    if (!_motionBlurPipeline || !_motionBlurRT) return;
    if (_motionBlurSRVIndex == UINT_MAX || _hdrSRVIndex == UINT_MAX) return;
    if (_depthSRVIndex == UINT_MAX) return;

    auto* cmd = _commandList.Get();
    UINT  W   = (UINT)_screenWidth;
    UINT  H   = (UINT)_screenHeight;
    const D3D12_RESOURCE_STATES SRV_BOTH = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                                         | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    // ── Build and upload constants ─────────────────────────────────────────
    XMMATRIX V   = XMLoadFloat4x4(&_lastView);
    XMMATRIX P   = XMLoadFloat4x4(&_lastProj);
    XMMATRIX VP  = XMMatrixMultiply(V, P);
    XMMATRIX iVP = XMMatrixInverse(nullptr, VP);
    XMFLOAT4X4 iVPf;
    XMStoreFloat4x4(&iVPf, iVP);

    MotionBlurConstants mb = {};
    mb.invViewProj  = iVPf;
    mb.prevViewProj = _mbLastViewProj;
    mb.screenSizeX  = (float)W;
    mb.screenSizeY  = (float)H;
    mb.shutterScale = 0.5f;
    mb.numSamples   = 8;
    memcpy(_motionBlurCBMapped[_frameIndex], &mb, sizeof(mb));

    // Store current VP for next frame
    XMFLOAT4X4 vpF; XMStoreFloat4x4(&vpF, VP);
    _mbLastViewProj = vpF;

    // ── Pre-barriers: hdrRT RT→SRV, depth DW→PSR, mbRT RT→RT (already RT from alloc) ──
    D3D12_RESOURCE_BARRIER pre[2];
    pre[0] = CD3DX12_RESOURCE_BARRIER::Transition(
        _hdrRT.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, SRV_BOTH);
    pre[1] = CD3DX12_RESOURCE_BARRIER::Transition(
        _depthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(2, pre);

    // ── Draw fullscreen motion blur ────────────────────────────────────────
    ID3D12DescriptorHeap* heaps[] = { _imGuiSrvHeap.Get() };
    cmd->SetDescriptorHeaps(1, heaps);
    BindPPPipeline(cmd, _motionBlurPipeline.get(), _imGuiSrvHeap, _motionBlurRTV, W, H);
    cmd->SetGraphicsRootConstantBufferView(0, _motionBlurCB[_frameIndex]->GetGPUVirtualAddress());
    cmd->SetGraphicsRootDescriptorTable(1, PPSRVHandle(_imGuiSrvHeap, _hdrSRVIndex,   _srvDescriptorSize));
    cmd->SetGraphicsRootDescriptorTable(2, PPSRVHandle(_imGuiSrvHeap, _depthSRVIndex, _srvDescriptorSize));
    cmd->DrawInstanced(3, 1, 0, 0);

    // ── Post-barriers: mbRT → SRV (for TAA), depth → DEPTH_WRITE ──────────
    D3D12_RESOURCE_BARRIER post[2];
    post[0] = CD3DX12_RESOURCE_BARRIER::Transition(
        _motionBlurRT.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, SRV_BOTH);
    post[1] = CD3DX12_RESOURCE_BARRIER::Transition(
        _depthBuffer.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_DEPTH_WRITE);
    cmd->ResourceBarrier(2, post);
    // _hdrRT remains SRV — not needed by TAA (which reads _motionBlurSRVIndex)
}

// ===========================================================================
// Phase 24: Clustered Lighting
// ===========================================================================

void DX12Backend::SetPointLights(const std::vector<PointLightDesc>& lights)
{
    _pointLights = lights;
}

bool DX12Backend::CreateClusteredLightingResources()
{
    // ─── Light buffer: host-visible UPLOAD heap (32 KB for 1024 lights) ───
    {
        D3D12MA::ALLOCATION_DESC allocDesc = {};
        allocDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC rd = CD3DX12_RESOURCE_DESC::Buffer(
            MAX_POINT_LIGHTS * sizeof(DX12GPUPointLight));

        HRESULT hr = _d3d12maAllocator->CreateResource(
            &allocDesc, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, &_clusterLightBufferAlloc, IID_PPV_ARGS(&_clusterLightBuffer));
        if (FAILED(hr)) return false;

        _clusterLightBuffer->Map(0, nullptr, &_clusterLightBufferMapped);

        // Create SRV for lights structured buffer
        D3D12_CPU_DESCRIPTOR_HANDLE cpu; D3D12_GPU_DESCRIPTOR_HANDLE gpu;
        _clusterLightSRVIndex = AllocateSRVSlot(cpu, gpu);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.FirstElement = 0;
        srv.Buffer.NumElements = MAX_POINT_LIGHTS;
        srv.Buffer.StructureByteStride = sizeof(DX12GPUPointLight);
        srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
        _device->CreateShaderResourceView(_clusterLightBuffer.Get(), &srv, cpu);
    }

    // ─── ClusterParams constant buffer (96 bytes) ───
    {
        D3D12MA::ALLOCATION_DESC allocDesc = {};
        allocDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC rd = CD3DX12_RESOURCE_DESC::Buffer(256);  // 256-byte aligned

        HRESULT hr = _d3d12maAllocator->CreateResource(
            &allocDesc, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, &_clusterParamsCBAlloc, IID_PPV_ARGS(&_clusterParamsCB));
        if (FAILED(hr)) return false;

        _clusterParamsCB->Map(0, nullptr, &_clusterParamsCBMapped);
    }

    // ─── Cluster counts buffer: DEFAULT heap with UAV+SRV (~14 KB) ───
    {
        D3D12MA::ALLOCATION_DESC allocDesc = {};
        allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

        UINT64 size = TOTAL_CLUSTERS * sizeof(uint32_t);
        D3D12_RESOURCE_DESC rd = CD3DX12_RESOURCE_DESC::Buffer(
            size, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

        HRESULT hr = _d3d12maAllocator->CreateResource(
            &allocDesc, &rd, D3D12_RESOURCE_STATE_COMMON,
            nullptr, &_clusterCountsBufferAlloc, IID_PPV_ARGS(&_clusterCountsBuffer));
        if (FAILED(hr)) return false;

        // SRV
        D3D12_CPU_DESCRIPTOR_HANDLE cpuS, cpuU; D3D12_GPU_DESCRIPTOR_HANDLE gpuS, gpuU;
        _clusterCountsSRVIndex = AllocateSRVSlot(cpuS, gpuS);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.NumElements = TOTAL_CLUSTERS;
        srv.Buffer.StructureByteStride = sizeof(uint32_t);
        _device->CreateShaderResourceView(_clusterCountsBuffer.Get(), &srv, cpuS);

        // UAV
        _clusterCountsUAVIndex = AllocateSRVSlot(cpuU, gpuU);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = TOTAL_CLUSTERS;
        uav.Buffer.StructureByteStride = sizeof(uint32_t);
        _device->CreateUnorderedAccessView(_clusterCountsBuffer.Get(), nullptr, &uav, cpuU);
    }

    // ─── Cluster indices buffer: DEFAULT heap with UAV+SRV (~1.7 MB) ───
    {
        D3D12MA::ALLOCATION_DESC allocDesc = {};
        allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

        UINT64 size = TOTAL_CLUSTERS * MAX_LIGHTS_PER_CLUSTER * sizeof(uint32_t);
        D3D12_RESOURCE_DESC rd = CD3DX12_RESOURCE_DESC::Buffer(
            size, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

        HRESULT hr = _d3d12maAllocator->CreateResource(
            &allocDesc, &rd, D3D12_RESOURCE_STATE_COMMON,
            nullptr, &_clusterIndicesBufferAlloc, IID_PPV_ARGS(&_clusterIndicesBuffer));
        if (FAILED(hr)) return false;

        // SRV
        D3D12_CPU_DESCRIPTOR_HANDLE cpuS, cpuU; D3D12_GPU_DESCRIPTOR_HANDLE gpuS, gpuU;
        _clusterIndicesSRVIndex = AllocateSRVSlot(cpuS, gpuS);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.NumElements = TOTAL_CLUSTERS * MAX_LIGHTS_PER_CLUSTER;
        srv.Buffer.StructureByteStride = sizeof(uint32_t);
        _device->CreateShaderResourceView(_clusterIndicesBuffer.Get(), &srv, cpuS);

        // UAV
        _clusterIndicesUAVIndex = AllocateSRVSlot(cpuU, gpuU);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = TOTAL_CLUSTERS * MAX_LIGHTS_PER_CLUSTER;
        uav.Buffer.StructureByteStride = sizeof(uint32_t);
        _device->CreateUnorderedAccessView(_clusterIndicesBuffer.Get(), nullptr, &uav, cpuU);
    }

    // ─── Cluster assign compute pipeline ───
    _clusterAssignPipeline = std::make_unique<DX12Pipeline>();
    {
        PipelineStateDesc desc;
        desc.rootLayout    = RootSignatureLayout::ClusterAssign;
        desc.computeShader = true;
        if (!_clusterAssignPipeline->Initialize(_device, L"cluster_assign.comp.hlsl", L"", desc))
        {
            LUNA_LOG_WARN("Failed to create cluster assign compute pipeline");
            _clusterAssignPipeline.reset();
            return false;
        }
    }

    _clusteredLightingReady = true;
    LUNA_LOG_INFO("DX12: Clustered lighting initialized (16x9x24 = %u clusters, max %u lights)",
                  TOTAL_CLUSTERS, MAX_POINT_LIGHTS);
    return true;
}

void DX12Backend::DestroyClusteredLightingResources()
{
    _clusteredLightingReady = false;
    _clusterAssignPipeline.reset();

    if (_clusterLightBufferAlloc)   { _clusterLightBufferAlloc->Release();   _clusterLightBufferAlloc = nullptr; }
    if (_clusterParamsCBAlloc)      { _clusterParamsCBAlloc->Release();      _clusterParamsCBAlloc = nullptr; }
    if (_clusterCountsBufferAlloc)  { _clusterCountsBufferAlloc->Release();  _clusterCountsBufferAlloc = nullptr; }
    if (_clusterIndicesBufferAlloc) { _clusterIndicesBufferAlloc->Release(); _clusterIndicesBufferAlloc = nullptr; }

    _clusterLightBuffer.Reset();
    _clusterParamsCB.Reset();
    _clusterCountsBuffer.Reset();
    _clusterIndicesBuffer.Reset();
}

void DX12Backend::DispatchClusterAssign()
{
    if (!_clusteredLightingReady || _pointLights.empty()) return;

    auto cmd = _commandList.Get();

    // ─── Transform lights to view-space and upload ───
    XMMATRIX view = XMLoadFloat4x4(&_lastView);
    uint32_t numLights = (uint32_t)std::min(_pointLights.size(), (size_t)MAX_POINT_LIGHTS);

    auto* dst = static_cast<DX12GPUPointLight*>(_clusterLightBufferMapped);
    for (uint32_t i = 0; i < numLights; ++i)
    {
        const auto& src = _pointLights[i];
        XMVECTOR posW = XMVectorSet(src.position[0], src.position[1], src.position[2], 1.0f);
        XMVECTOR posV = XMVector3TransformCoord(posW, view);
        XMFLOAT3 pv; XMStoreFloat3(&pv, posV);

        dst[i].position[0] = pv.x;
        dst[i].position[1] = pv.y;
        dst[i].position[2] = pv.z;
        dst[i].radius = src.radius;
        dst[i].color[0] = src.color[0];
        dst[i].color[1] = src.color[1];
        dst[i].color[2] = src.color[2];
        dst[i].intensity = src.intensity;
    }

    // ─── Upload ClusterParams ───
    XMMATRIX proj = XMLoadFloat4x4(&_lastProj);
    XMMATRIX invProj = XMMatrixInverse(nullptr, proj);
    ClusterParamsData params;
    XMStoreFloat4x4(&params.invProj, invProj);
    params.nearZ = 0.1f;
    params.farZ = 100.0f;
    params.screenW = (float)_screenWidth;
    params.screenH = (float)_screenHeight;
    params.numLights = numLights;
    params._pad[0] = params._pad[1] = params._pad[2] = 0;
    memcpy(_clusterParamsCBMapped, &params, sizeof(params));

    // ─── Barrier: cluster buffers to UAV state ───
    D3D12_RESOURCE_BARRIER pre[2];
    pre[0] = CD3DX12_RESOURCE_BARRIER::Transition(
        _clusterCountsBuffer.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    pre[1] = CD3DX12_RESOURCE_BARRIER::Transition(
        _clusterIndicesBuffer.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmd->ResourceBarrier(2, pre);

    // ─── Clear cluster counts to zero ───
    D3D12_GPU_DESCRIPTOR_HANDLE countsUAVGpu;
    countsUAVGpu.ptr = _imGuiSrvHeap->GetGPUDescriptorHandleForHeapStart().ptr
                     + static_cast<UINT64>(_clusterCountsUAVIndex) * _srvDescriptorSize;
    D3D12_CPU_DESCRIPTOR_HANDLE countsUAVCpu;
    countsUAVCpu.ptr = _imGuiSrvHeap->GetCPUDescriptorHandleForHeapStart().ptr
                     + static_cast<UINT64>(_clusterCountsUAVIndex) * _srvDescriptorSize;
    UINT clearVal[4] = {0, 0, 0, 0};
    cmd->ClearUnorderedAccessViewUint(countsUAVGpu, countsUAVCpu, 
        _clusterCountsBuffer.Get(), clearVal, 0, nullptr);

    // ─── Dispatch cluster assignment ───
    ID3D12DescriptorHeap* heaps[] = { _imGuiSrvHeap.Get() };
    cmd->SetDescriptorHeaps(1, heaps);

    BindPipeline(_clusterAssignPipeline.get());
    cmd->SetComputeRootConstantBufferView(0, _clusterParamsCB->GetGPUVirtualAddress());

    D3D12_GPU_DESCRIPTOR_HANDLE lightSrvGpu;
    lightSrvGpu.ptr = _imGuiSrvHeap->GetGPUDescriptorHandleForHeapStart().ptr
                    + static_cast<UINT64>(_clusterLightSRVIndex) * _srvDescriptorSize;
    cmd->SetComputeRootDescriptorTable(1, lightSrvGpu);
    cmd->SetComputeRootDescriptorTable(2, countsUAVGpu);

    D3D12_GPU_DESCRIPTOR_HANDLE indicesUAVGpu;
    indicesUAVGpu.ptr = _imGuiSrvHeap->GetGPUDescriptorHandleForHeapStart().ptr
                      + static_cast<UINT64>(_clusterIndicesUAVIndex) * _srvDescriptorSize;
    cmd->SetComputeRootDescriptorTable(3, indicesUAVGpu);

    cmd->Dispatch(CLUSTER_X, CLUSTER_Y, CLUSTER_Z);

    // ─── Barrier: UAV → SRV for deferred lighting ───
    D3D12_RESOURCE_BARRIER post[2];
    post[0] = CD3DX12_RESOURCE_BARRIER::Transition(
        _clusterCountsBuffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    post[1] = CD3DX12_RESOURCE_BARRIER::Transition(
        _clusterIndicesBuffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(2, post);
}

} // namespace Luna
