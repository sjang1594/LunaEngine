#include "LunaPCH.h"
#include "LunaEngine/Graphics/IBuffer.h"
#include "LunaEngine/Renderer/DX12/Public/DX12Backend.h"
#include "LunaEngine/Renderer/DX12/Public/DX12Pipeline.h"
#include "LunaEngine/Renderer/DX12/Public/DX12Device.h"
#include "Renderer/DX12/Public/DX12Buffer.h"

namespace Luna
{

// ---------------------------------------------------------------------------
// Test geometry — remove once mesh system is in place
// ---------------------------------------------------------------------------
struct Vertex
{
    Vec3 position;
    Vec4 color;
};

static const Vertex s_Vertices[] = {
    {{ 0.0f,  0.5f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}}, // top    (red)
    {{ 0.5f, -0.5f, 0.0f}, {0.0f, 1.0f, 0.0f, 1.0f}}, // right  (green)
    {{-0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f, 1.0f}}, // left   (blue)
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
DX12Backend::~DX12Backend()
{
    Shutdown();
}

void DX12Backend::Shutdown()
{
    if (_fenceEvent)
    {
        WaitSync();
        CloseHandle(_fenceEvent);
        _fenceEvent = nullptr;
    }
}

bool DX12Backend::Init(void *windowHandler, uint32_t width, uint32_t height)
{
    _mainWindow     = static_cast<HWND>(windowHandler);
    _screenViewport = {0.0f, 0.0f, static_cast<FLOAT>(width), static_cast<FLOAT>(height), 0.0f, 1.0f};
    _scissorRect    = CD3DX12_RECT(0, 0, static_cast<LONG>(width), static_cast<LONG>(height));
    SetResolution(width, height);

    // Device and factory are created by DX12Device
    _dx12Device   = std::make_unique<DX12Device>();
    _device       = _dx12Device->GetDeviceComPtr();
    _mdxgiFactory = _dx12Device->GetDXGIFactory();

    if (!CreateCommandQueueAndFenceEvent()) return false;
    if (!CreateSwapChain())                return false;
    if (!CreateRenderTarget())             return false;
    if (!CreateImGuiRenderTarget())        return false;

    _trianglePipeline = std::make_unique<DX12Pipeline>();
    PipelineStateDesc desc;
    if (!_trianglePipeline->Initialize(_device, L"triangle.vert.hlsl", L"triangle.frag.hlsl", desc))
    {
        LUNA_LOG_ERROR("Failed to initialize triangle pipeline");
        return false;
    }

    LUNA_LOG_INFO("DX12 backend initialized successfully");
    return true;
}

// ---------------------------------------------------------------------------
// Synchronization
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

    if (FAILED(_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               IID_PPV_ARGS(&_commandAllocator))))
    {
        LUNA_LOG_ERROR("Failed to create command allocator");
        return false;
    }

    if (FAILED(_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                          _commandAllocator.Get(), nullptr,
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

void DX12Backend::WaitSync()
{
    ++_fenceValue;
    _commandQueue->Signal(_fence.Get(), _fenceValue);

    if (_fence->GetCompletedValue() < _fenceValue)
    {
        _fence->SetEventOnCompletion(_fenceValue, _fenceEvent);
        WaitForSingleObject(_fenceEvent, INFINITE);
    }
}

// ---------------------------------------------------------------------------
// Swap chain & render targets
// ---------------------------------------------------------------------------
bool DX12Backend::CreateSwapChain()
{
    _swapChain.Reset();

    UINT msaaQuality = _dx12Device->GetMSAAQuality();

    DXGI_SWAP_CHAIN_DESC1 scDesc = {};
    scDesc.Width       = static_cast<UINT>(_screenWidth);
    scDesc.Height      = static_cast<UINT>(_screenHeight);
    scDesc.Format      = _backBufferFormat;
    scDesc.Stereo      = FALSE;
    scDesc.SampleDesc.Count   = (msaaQuality > 1) ? 4 : 1;
    scDesc.SampleDesc.Quality = (msaaQuality > 1) ? (msaaQuality - 1) : 0;
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
        LUNA_LOG_ERROR("CreateSwapChainForHwnd failed: HRESULT 0x%08lX", static_cast<unsigned long>(hr));
        return false;
    }

    hr = tempSwapChain.As(&_swapChain);
    if (FAILED(hr))
    {
        LUNA_LOG_ERROR("SwapChain1 -> SwapChain4 cast failed: HRESULT 0x%08lX", static_cast<unsigned long>(hr));
        return false;
    }

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
            LUNA_LOG_ERROR("Failed to get swap chain buffer");
            return false;
        }
        _rtvHandle[i].ptr = heapStart.ptr + i * rtvSize;
        _device->CreateRenderTargetView(_rtvBuffer[i].Get(), nullptr, _rtvHandle[i]);
    }
    return true;
}

bool DX12Backend::CreateImGuiRenderTarget()
{
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 1;
    heapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    if (FAILED(_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&_imGuiSrvHeap))))
    {
        LUNA_LOG_ERROR("Failed to create ImGui SRV descriptor heap");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------
void DX12Backend::BeginFrame()
{
    _commandAllocator->Reset();
    _commandList->Reset(_commandAllocator.Get(), nullptr);

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
    _commandList->OMSetRenderTargets(1, &_rtvHandle[backIdx], FALSE, nullptr);
}

void DX12Backend::DrawFrame()
{
    BindPipeline(_trianglePipeline.get());
    SetPipelineState(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, &_screenViewport, &_scissorRect);
    Draw(3);
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
    _swapChain->Present(1, 0); // vsync on; use Present(0,0) for uncapped

    WaitSync();
}

void DX12Backend::Resize(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0) return;

    WaitSync();
    SetResolution(width, height);

    _screenViewport = {0.0f, 0.0f, static_cast<FLOAT>(width), static_cast<FLOAT>(height), 0.0f, 1.0f};
    _scissorRect    = CD3DX12_RECT(0, 0, static_cast<LONG>(width), static_cast<LONG>(height));

    for (UINT i = 0; i < SWAP_CHAIN_BUFFER_COUNT; ++i)
        _rtvBuffer[i].Reset();

    _swapChain->ResizeBuffers(SWAP_CHAIN_BUFFER_COUNT, width, height, _backBufferFormat, 0);
    CreateRenderTarget();
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
