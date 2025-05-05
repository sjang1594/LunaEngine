#include "LunaPCH.h"
#include "LunaEngine/Graphics/IBuffer.h"
#include "LunaEngine/Renderer/DX12/public/DX12Backend.h"
#include "LunaEngine/Renderer/DX12/public/DX12Pipeline.h"
#include "Renderer/DX12/public/DX12Buffer.h"

// TRIANGLE TEST
struct Vertex
{
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT3 color;
};

Vertex vertices[] = {
    {{0.0f, 0.5f, 0.0f}, {1.0f, 0.0f, 0.0f}},  // TOP (RED)
    {{0.5f, -0.5f, 0.0f}, {0.0f, 1.0f, 0.0f}}, // Right (Green)
    {{-0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}} // Left (Blue)
};

namespace Luna
{

bool DX12Backend::Init(void *windowHandler, uint32_t width, uint32_t height)
{
    _mainWindow = static_cast<HWND>(windowHandler);
    _screenViewport = {0, 0, static_cast<FLOAT>(width), static_cast<FLOAT>(height), 0.0f, 1.0f};

    _scissorRect = CD3DX12_RECT(0, 0, width, height); // NOLINT(bugprone-narrowing-conversions)

    PipelineStateDesc desc;
    
    SetResolution(width, height);
    CreateDebugLayer();
    if (!CreateFactoryAndAdapter())
        return false;
    if (!CreateDevice())
        return false;
    if (!CreateCommandQueueAndFenceEvent())
        return false;
    if (!CreateSwapChain())
        return false;
    if (!CreateRenderTarget())
        return false;
    if (!CreateImGuiRenderTarget())
        return false;
    
    std::cout << "DX12 Initialization is Successful" << endl;

    _triangleVertexBuffer = std::make_shared<DX12Buffer>(
        BufferUsage::Vertex, vertices, sizeof(vertices), sizeof(Vertex));
    
    _trianglePipeline = std::make_unique<DX12Pipeline>();
    if (_device == nullptr)
        cout << "Device is null" << endl;
    if (!_trianglePipeline->Initialize(_device,
        L"triangle.vert.hlsl",
        L"triangle.frag.hlsl",
        desc))
    {
        std::cerr << "Failed to initialize triangle pipeline" << std::endl;
        return false;
    }
    return true;
}

bool DX12Backend::CheckIfImGuiData()
{
    auto *drawData = ImGui::GetDrawData();
    if (!drawData)
    {
        std::cout << "[ImGui] DrawData is null. Nothing to render.\n";
        return false;
    }
    return true;
}

void DX12Backend::CreateDebugLayer()
{
#if defined(_DEBUG) || defined(DEBUG)
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&_debugController))))
    {
        _debugController->EnableDebugLayer();
    }
#endif
}

bool DX12Backend::CreateFactoryAndAdapter()
{
    UINT flags = 0;

#if defined(_DEBUG) || defined(DEBUG)
    flags |= DXGI_CREATE_FACTORY_DEBUG;
#endif

    HRESULT hr = CreateDXGIFactory2(flags, IID_PPV_ARGS(&_mdxgiFactory));
    if (FAILED(hr))
    {
        std::cout << "Create DXGIFactory2 Failed" << std::endl;
        return false;
    }

    for (UINT i = 0;; ++i)
    {
        ComPtr<IDXGIAdapter1> candidate;
        if (_mdxgiFactory->EnumAdapters1(i, candidate.GetAddressOf()) == DXGI_ERROR_NOT_FOUND)
            break;

        DXGI_ADAPTER_DESC1 desc;
        candidate->GetDesc1(&desc);

        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            continue;

        if (desc.DedicatedVideoMemory > 0)
        {
            _adapter = candidate;
            wprintf(L" Selected Adapter: %s\n", desc.Description);
            return true;
        }
    }
    std::cout << "No suitable GPU adapter found." << std::endl;
    return false;
}

bool DX12Backend::CreateDevice()
{
    HRESULT hr = D3D12CreateDevice(_adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&_device));

    if (FAILED(hr))
    {
        std::cout << "Failed to Create D3D12 Device" << std::endl;
        return false;
    }

    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS msQualityLevels;
    msQualityLevels.Format = mBackBufferFormat;
    msQualityLevels.SampleCount = 4;
    msQualityLevels.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
    msQualityLevels.NumQualityLevels = 0;

    hr = _device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
        &msQualityLevels, sizeof(msQualityLevels));
    

    if (FAILED(hr) || msQualityLevels.NumQualityLevels == 0)
    {
        std::cout << "[MSAA] 4x not supported. Disabling MSAA.\n";
        m4xMsaaQuality = 0;
    }
    else
    {
        m4xMsaaQuality = msQualityLevels.NumQualityLevels;
        std::cout << "[MSAA] Supported quality levels: " << m4xMsaaQuality << std::endl;
    }
    return true;
}

bool DX12Backend::CreateCommandQueueAndFenceEvent()
{
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

    HRESULT hr = _device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&_commandQueue));
    if (FAILED(hr))
    {
        std::cout << "Failed to Create D3D12 Command Queue" << std::endl;
        return false;
    }

    // - D3D12_COMMAND_LIST_TYPE_DIRECT: CommandList by GPU.
    _device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                    IID_PPV_ARGS(&_commandAllocator));

    _device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _commandAllocator.Get(), nullptr,
                               IID_PPV_ARGS(&_commandList));

    _commandList->Close();

    hr = _device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS((&_fence)));
    if (FAILED(hr))
    {
        std::cout << "Failed to Create Fence" << std::endl;
        return false;
    }
    _fenceEvent = ::CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (_fenceEvent == nullptr)
    {
        std::cout << "Failed Create Fence Event" << std::endl;
        return false;
    }

    return true;
}

void DX12Backend::WaitSync()
{
    _fenceValue++;

    // GPU TIME LINE //
    _commandQueue->Signal(_fence.Get(), _fenceValue);

    // Wait until the GPU has Completed commands up to this fence point
    if (_fence->GetCompletedValue() < _fenceValue)
    {
        // Fire Event when GPU hits currenct fence value
        _fence->SetEventOnCompletion(_fenceValue, _fenceEvent);

        // Wait until the GPU hitss current fence event is fired
        WaitForSingleObject(_fenceEvent, INFINITE);
    }
}

bool DX12Backend::CreateSwapChain()
{
    _swapChain.Reset();
    DXGI_SWAP_CHAIN_DESC1 scDesc1 = {};
    scDesc1.Width = static_cast<UINT>(_screenWidth);
    scDesc1.Height = static_cast<UINT>(_screenHeight);
    scDesc1.Format = mBackBufferFormat;
    scDesc1.Stereo = FALSE;
    scDesc1.SampleDesc.Count = (m4xMsaaQuality > 1) ? 4 : 1;
    scDesc1.SampleDesc.Quality = (m4xMsaaQuality > 1) ? (m4xMsaaQuality - 1) : 0;
    scDesc1.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc1.BufferCount = SWAP_CHAIN_BUFFER_COUNT;
    scDesc1.Scaling = DXGI_SCALING_STRETCH;
    scDesc1.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scDesc1.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    scDesc1.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fsDesc = {};
    fsDesc.RefreshRate.Numerator = 60;
    fsDesc.RefreshRate.Denominator = 1;
    fsDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
    fsDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
    fsDesc.Windowed = TRUE;
    
    ComPtr<IDXGISwapChain1> tempSwapChain1;
    HRESULT hr = _mdxgiFactory->CreateSwapChainForHwnd(_commandQueue.Get(), _mainWindow, &scDesc1,
        &fsDesc, nullptr, &tempSwapChain1);
    if (FAILED(hr))
    {
        std::cerr << "Failed to create SwapChainForHwnd: " << std::hex << hr << std::endl;
        return false;
    }

    hr = tempSwapChain1.As(&_swapChain);
    if (FAILED(hr))
    {
        std::cerr << "Failed to cast SwapChain1 to SwapChain4: " << std::hex << hr << std::endl;
        return false;
    }

    for (UINT i = 0; i < SWAP_CHAIN_BUFFER_COUNT; ++i)
    {
        ComPtr<ID3D12Resource> buffer;
        hr = _swapChain->GetBuffer(i, IID_PPV_ARGS(&buffer));
        if (FAILED(hr))
        {
            std::cerr << "Failed to get buffer " << i << ": " << std::hex << hr << std::endl;
            return false;
        }

        _rtvBuffer[i] = buffer; // 배열 또는 vector에 저장
        std::cout << "Swap Chain Buffer " << i << " acquired" << std::endl;
    }

    return true;
}

bool DX12Backend::CreateRenderTarget()
{
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDesc.NumDescriptors = SWAP_CHAIN_BUFFER_COUNT;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    HRESULT hr = _device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&_rtvHeap));

    if (FAILED(hr))
    {
        std::cout << "Failed to Create RenderTarget" << std::endl;
        return false;
    }

    UINT _rtvHeapSize = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHeapBegin = _rtvHeap->GetCPUDescriptorHandleForHeapStart();

    for (int i = 0; i < SWAP_CHAIN_BUFFER_COUNT; ++i)
    {
        _swapChain->GetBuffer(i, IID_PPV_ARGS(&_rtvBuffer[i]));
        _rtvHandle[i].ptr = rtvHeapBegin.ptr + i * _rtvHeapSize;
        _device->CreateRenderTargetView(_rtvBuffer[i].Get(), nullptr, _rtvHandle[i]);
    }
    return true;
}

bool DX12Backend::CreateImGuiRenderTarget()
{
    D3D12_DESCRIPTOR_HEAP_DESC imguiHeapDesc = {};
    imguiHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    imguiHeapDesc.NumDescriptors = 1;
    imguiHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    HRESULT hr = _device->CreateDescriptorHeap(&imguiHeapDesc, IID_PPV_ARGS(&_imGuiSrvHeap));

    if (FAILED(hr))
    {
        std::cout << "Failed to Create RenderTarget for IMGUI" << std::endl;
        return false;
    }
    return true;
}

void DX12Backend::SetResolution(const uint32_t &width, const uint32_t &height)
{
    _screenWidth = width;
    _screenHeight = height;
}

D3D12_CPU_DESCRIPTOR_HANDLE DX12Backend::GetBackBufferView()
{
    return GetRTV(_swapChain->GetCurrentBackBufferIndex());
}

void DX12Backend::BeginFrame()
{
    _commandAllocator->Reset();
    _commandList->Reset(_commandAllocator.Get(), nullptr);

    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        GetCurrentBackBufferResource().Get(), D3D12_RESOURCE_STATE_PRESENT,
        D3D12_RESOURCE_STATE_RENDER_TARGET);

    _commandList->ResourceBarrier(1, &barrier);
    _commandList->RSSetViewports(1, &_screenViewport);
    _commandList->RSSetScissorRects(1, &_scissorRect);

    D3D12_CPU_DESCRIPTOR_HANDLE backBufferView = GetBackBufferView();
    const FLOAT clearColor[] = { 0.2f, 0.2f, 0.2f, 1.0f };
    _commandList->ClearRenderTargetView(backBufferView, clearColor, 0, nullptr);
    _commandList->OMSetRenderTargets(1, &backBufferView, FALSE, nullptr);
}

void DX12Backend::InitImGui(void *windowHandler)
{
    IMGUI_CHECKVERSION();
    auto window = static_cast<GLFWwindow *>(windowHandler);
    if (!window)
    {
        std::cout << "Casting Failed" << std::endl;
    }
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;
    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForOther(window, true);
    if (!ImGui_ImplDX12_Init(_device.Get(), SWAP_CHAIN_BUFFER_COUNT, DXGI_FORMAT_R8G8B8A8_UNORM,
                             _imGuiSrvHeap.Get(),
                             _imGuiSrvHeap->GetCPUDescriptorHandleForHeapStart(),
                             _imGuiSrvHeap->GetGPUDescriptorHandleForHeapStart()))
    {
        std::cout << "ImGui_ImplDX12_Init failed!" << std::endl;
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
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = GetBackBufferView();
    _commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    
    ImGui::Render();
#if defined(_DEBUG)
    if (!CheckIfImGuiData())
        return;
#endif
    ID3D12DescriptorHeap *heaps[] = {_imGuiSrvHeap.Get()};
    _commandList->SetDescriptorHeaps(_countof(heaps), heaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), _commandList.Get());

    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }
}

void DX12Backend::DrawFrame()
{
    std::cout << "=== DRAW FRAME START ===" << std::endl;
    BindPipeline(_trianglePipeline.get());
    SetVertexBuffer(_triangleVertexBuffer.get());
    _commandList->RSSetViewports(1, &_screenViewport);
    _commandList->RSSetScissorRects(1, &_scissorRect);
    _commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    std::cout << "-> Calling Draw(3)" << std::endl;
    Draw(3);
    std::cout << "=== DRAW FRAME END ===" << std::endl;
}

void DX12Backend::ShutdownImGui()
{
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

void DX12Backend::EndFrame()
{
    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        GetCurrentBackBufferResource().Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PRESENT);

    _commandList->ResourceBarrier(1, &barrier);
    ID3D12CommandList *cmdListArr[] = {_commandList.Get()};
    _commandList->Close();
    _commandQueue->ExecuteCommandLists(_countof(cmdListArr), cmdListArr);

    _swapChain->Present(0, 0);

    WaitSync();

    _backbufferIndex = (_backbufferIndex + 1) % SWAP_CHAIN_BUFFER_COUNT;
}

void DX12Backend::Resize(uint32_t width, uint32_t height)
{
}

const char *DX12Backend::GetBackendName() const
{
    return "DirectX 12";
}

void DX12Backend::Draw(uint32_t vertexCount)
{
    _commandList->DrawInstanced(vertexCount, 1, 0, 0);
}

void DX12Backend::SetVertexBuffer(IBuffer *buffer)
{
    auto dxBuffer = dynamic_cast<DX12Buffer*>(buffer);
    
    _commandList->IASetVertexBuffers(0, 1, dxBuffer->GetVBView());
}

void DX12Backend::BindPipeline(IPipeline *pipeline)
{
    auto dx = dynamic_cast<DX12Pipeline*>(pipeline);
    if (!dx)
    {
        std::cerr << "[DX12Backend] BindPipeline failed: pipeline is not a DX12Pipeline." << std::endl;
        return;
    }
    _commandList->SetGraphicsRootSignature(dx->GetRootSignature().Get());
    _commandList->SetPipelineState(dx->GetPipelineState().Get());
}
} // namespace Luna