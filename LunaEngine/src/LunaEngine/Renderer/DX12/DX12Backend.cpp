#include "LunaPCH.h"
#include "DX12Backend.h"

bool Luna::DX12Backend::Init(void *windowHandler, uint32_t width,
                             uint32_t height) {
  _mainWindow = static_cast<HWND>(windowHandler);
  _screenViewport = {
      0, 0, static_cast<FLOAT>(width), static_cast<FLOAT>(height), 0.0f, 1.0f};

  _scissorRect = CD3DX12_RECT(0, 0, width, height);

  SetResolution(width, height);
  CreateDebugLayer();
  if (!CreateFactoryAndAdapter()) return false;
  if (!CreateDevice()) return false;
  if (!CreateCommandQueueAndFenceEvent()) return false;
  if (!CreateSwapChain()) return false;
  if (!CreateDescriptorHeap()) return false;
  if (!CreateImGuiDescriptorHeap()) return false;

  std::cout << "Initialization is Successed" << endl;
  return true;
}

bool Luna::DX12Backend::CheckIfImGuiData() 
{ 
  auto *drawData = ImGui::GetDrawData();
  if (!drawData) {
    std::cout << "[ImGui] DrawData is null. Nothing to render.\n";
    return false;
  }
  return true;
}

void Luna::DX12Backend::CreateDebugLayer() {
#if defined(_DEBUG)
  if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&_debugController)))) {
    _debugController->EnableDebugLayer();
  }
#endif
}

bool Luna::DX12Backend::CreateFactoryAndAdapter() {
  UINT flags = 0;
#if defined(_DEBUG)
  flags |= DXGI_CREATE_FACTORY_DEBUG;
#endif

  HRESULT hr = CreateDXGIFactory2(flags, IID_PPV_ARGS(&_dxgiFactory));
  if (FAILED(hr)) {
    std::cout << "Create DXGIFactory2 Failed" << std::endl;
    return false;
  }

  for (UINT i = 0;; ++i) {
    ComPtr<IDXGIAdapter1> candidate;
    if (_dxgiFactory->EnumAdapters1(i, candidate.GetAddressOf()) ==
        DXGI_ERROR_NOT_FOUND)
      break;

    DXGI_ADAPTER_DESC1 desc;
    candidate->GetDesc1(&desc);

    if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

    if (desc.DedicatedVideoMemory > 0) {
      _adapter = candidate;
      wprintf(L" Selected Adapter: %s\n", desc.Description);
      return true;
    }
  }
  std::cout << "No suitable GPU adapter found." << std::endl;
  return false;
}

bool Luna::DX12Backend::CreateDevice() {
  HRESULT hr = D3D12CreateDevice(_adapter.Get(), D3D_FEATURE_LEVEL_12_0,
                                 IID_PPV_ARGS(&_device));

  if (FAILED(hr)) {
    std::cout << "Failed to Create D3D12 Device" << std::endl;
    return false;
  }

#if defined(_DEBUG)
  ComPtr<ID3D12InfoQueue> infoQueue;
  if (SUCCEEDED(_device->QueryInterface(IID_PPV_ARGS(&infoQueue)))) {
    infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
    infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
    infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, TRUE);
  }
#endif
  return true;
}

bool Luna::DX12Backend::CreateCommandQueueAndFenceEvent() {
  D3D12_COMMAND_QUEUE_DESC queueDesc = {};
  queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
  queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

  HRESULT hr =
      _device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&_commandQueue));
  if (FAILED(hr)) {
    std::cout << "Failed to Create D3D12 Command Queue" << std::endl;
    return false;
  }

  // - D3D12_COMMAND_LIST_TYPE_DIRECT: CommandList by GPU.
  _device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                  IID_PPV_ARGS(&_commandAllocator));

  _device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                             _commandAllocator.Get(), nullptr,
                             IID_PPV_ARGS(&_commandList));
  
  _commandList->Close();

  hr = _device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS((&_fence)));
  if (FAILED(hr)) { std::cout << "Failed to Create Fence" << std::endl; return false; }
  _fenceEvent = ::CreateEvent(nullptr, FALSE, FALSE, nullptr);
  if (_fenceEvent == nullptr) {
    std::cout << "Failed Create Fence Event" << std::endl;
    return false;
  }

  return true;
}

void Luna::DX12Backend::WaitSync() {
  _fenceValue++;

  // GPU TIME LINE //
  _commandQueue->Signal(_fence.Get(), _fenceValue);

  // Wait until the GPU has Completed commands up to this fence point
  if (_fence->GetCompletedValue() < _fenceValue) {
    // Fire Event when GPU hits currenct fence value
    _fence->SetEventOnCompletion(_fenceValue, _fenceEvent);

    // Wait until the GPU hitss current fence event is fired
    ::WaitForSingleObject(_fenceEvent, INFINITE);
  }
}

bool Luna::DX12Backend::CreateSwapChain() {
  _swapChain.Reset();
  DXGI_SWAP_CHAIN_DESC1 scDesc = {};
  scDesc.Width = static_cast<UINT>(_screenWidth);
  scDesc.Height = static_cast<UINT>(_screenHeight);
  scDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  scDesc.BufferCount = SWAP_CHAIN_BUFFER_COUNT;
  scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  scDesc.SampleDesc.Count = 1;  // MultiSampling = OFF
  scDesc.SampleDesc.Quality = 0;

  ComPtr<IDXGISwapChain1> tempSwapChain1;
  HRESULT hr = _dxgiFactory->CreateSwapChainForHwnd(
      _commandQueue.Get(), _mainWindow, &scDesc, nullptr, nullptr,
      &tempSwapChain1);
  if (FAILED(hr)) {
    std::cout << "Failed to create SwapChain" << std::endl;
    return false;
  }

  hr = tempSwapChain1.As(&_swapChain);
  if (FAILED(hr)) {
    std::cout << "Failed to cast SwapChain1 to SwapChain4" << std::endl;
    return false;
  }

  for (int i = 0; i < SWAP_CHAIN_BUFFER_COUNT; ++i) {
    ComPtr<ID3D12Resource> buffer;
    HRESULT hr = _swapChain->GetBuffer(i, IID_PPV_ARGS(&buffer));
    if (FAILED(hr)) {
      std::cerr << "Swap Chain Buffer " << i << " Failed to Catch: " << std::hex << hr << std::endl;
      return false;
    }
    std::cout << "Swap Chain Buffer" << i << " Success" << std::endl;
  }
  
  return true;
}

bool Luna::DX12Backend::CreateDescriptorHeap() {
  D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
  heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  heapDesc.NumDescriptors = SWAP_CHAIN_BUFFER_COUNT;
  heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

  HRESULT hr =
      _device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&_rtvHeap));

  if (FAILED(hr)) {
    std::cout << "Failed to Create RenderTarget - DescriptorHeap" << std::endl;
    return false;
  }

  _rtvHeapSize =
      _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

  D3D12_CPU_DESCRIPTOR_HANDLE rtvHeapBegin =
      _rtvHeap->GetCPUDescriptorHandleForHeapStart();

  for (int i = 0; i < SWAP_CHAIN_BUFFER_COUNT; ++i) {
    _swapChain->GetBuffer(i, IID_PPV_ARGS(&_renderTargets[i]));
    _rtvHandle[i].ptr = rtvHeapBegin.ptr + i * _rtvHeapSize;
    _device->CreateRenderTargetView(_renderTargets[i].Get(), nullptr,
                                    _rtvHandle[i]);
  }
  return true;
}

bool Luna::DX12Backend::CreateImGuiDescriptorHeap() {
  D3D12_DESCRIPTOR_HEAP_DESC imguiHeapDesc = {};
  imguiHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  imguiHeapDesc.NumDescriptors = 1;
  imguiHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

  HRESULT hr = _device->CreateDescriptorHeap(&imguiHeapDesc,
                                             IID_PPV_ARGS(&_imguiSrvHeap));

  if (FAILED(hr)) {
    std::cout << "Failed to Create IMGUI - Descriptor Heap" << std::endl;
    return false;
  }
  return true;
}

void Luna::DX12Backend::SetResolution(const uint32_t &width,
                                      const uint32_t &height) {
  _screenWidth = width;
  _screenHeight = height;
}

D3D12_CPU_DESCRIPTOR_HANDLE Luna::DX12Backend::GetBackBufferView() {
  return GetRTV(_swapChain->GetCurrentBackBufferIndex());
}

void Luna::DX12Backend::BeginFrame() {
  _commandAllocator->Reset();
  _commandList->Reset(_commandAllocator.Get(), nullptr);

  D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
      GetCurrentBackBufferResource().Get(), D3D12_RESOURCE_STATE_PRESENT,
      D3D12_RESOURCE_STATE_RENDER_TARGET);

  _commandList->ResourceBarrier(1, &barrier);
  _commandList->RSSetViewports(1, &_screenViewport);
  _commandList->RSSetScissorRects(1, &_scissorRect);

  D3D12_CPU_DESCRIPTOR_HANDLE backBufferView = GetBackBufferView();
  _commandList->ClearRenderTargetView(backBufferView, Colors::Black, 0,
                                      nullptr);
  _commandList->OMSetRenderTargets(1, &backBufferView, FALSE, nullptr);
}

void Luna::DX12Backend::InitImGui(void *windowHandler) {
  IMGUI_CHECKVERSION();
  GLFWwindow *window = static_cast<GLFWwindow *>(windowHandler);
  if (!window) {
    std::cout << "Casting Failed" << std::endl;
  }
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
  ImGui::StyleColorsDark();

  ImGui_ImplGlfw_InitForOther(window, true);
  if (!ImGui_ImplDX12_Init(
          _device.Get(), SWAP_CHAIN_BUFFER_COUNT, DXGI_FORMAT_R8G8B8A8_UNORM,
          _imguiSrvHeap.Get(),
          _imguiSrvHeap->GetCPUDescriptorHandleForHeapStart(),
          _imguiSrvHeap->GetGPUDescriptorHandleForHeapStart())) {
    std::cout << "ImGui_ImplDX12_Init failed!" << std::endl;
    return;
  }

  ImGui_ImplDX12_CreateDeviceObjects();
}

void Luna::DX12Backend::StartImGui(){
  ImGui_ImplDX12_NewFrame();
  ImGui_ImplGlfw_NewFrame();
  ImGui::NewFrame();
}

void Luna::DX12Backend::RenderImGui() {
  ImGui::Render();
#if defined(_DEBUG)
  if (!CheckIfImGuiData())
      return
#endif

  ImGui::Render();

  if (!_imguiSrvHeap) {
    std::cout << "[ImGui] DescriptorHeap is null!\n";
    return;
  }
  ID3D12DescriptorHeap* heaps[] = { _imguiSrvHeap.Get() };
  _commandList->SetDescriptorHeaps(_countof(heaps), heaps);
  ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), _commandList.Get());

  if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
    ImGui::UpdatePlatformWindows();
    ImGui::RenderPlatformWindowsDefault();  
  }
}

void Luna::DX12Backend::DrawFrame() {}

void Luna::DX12Backend::ShutdownImGui() {
  ImGui_ImplDX12_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
}

void Luna::DX12Backend::EndFrame() {
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

void Luna::DX12Backend::Resize(uint32_t width, uint32_t height) {
  if (width == 0 || height == 0 || !_swapChain) return;

  WaitSync();

  for (int i = 0; i < SWAP_CHAIN_BUFFER_COUNT; ++i) {
    _renderTargets[i].Reset();
  }

  DXGI_SWAP_CHAIN_DESC desc = {};
  _swapChain->GetDesc(&desc);

  HRESULT hr = _swapChain->ResizeBuffers(SWAP_CHAIN_BUFFER_COUNT, width, height,
                                         desc.BufferDesc.Format, desc.Flags);

  if (FAILED(hr)) {
    std::cout << "Resize Buffer Failed!" << std::endl;
    return;
  }
  
  _backbufferIndex = _swapChain->GetCurrentBackBufferIndex();
  D3D12_CPU_DESCRIPTOR_HANDLE rtvHeapBegin =
      _rtvHeap->GetCPUDescriptorHandleForHeapStart();

  for (UINT i = 0; i < SWAP_CHAIN_BUFFER_COUNT; ++i) {
    _swapChain->GetBuffer(i, IID_PPV_ARGS(&_renderTargets[i]));
    _rtvHandle[i].ptr = rtvHeapBegin.ptr + i * _rtvHeapSize;

    _device->CreateRenderTargetView(_renderTargets[i].Get(), nullptr,
                                    _rtvHandle[i]);
  }

  SetResolution(width, height);
  _screenViewport.Width = static_cast<float>(_screenWidth);
  _screenViewport.Height = static_cast<float>(_screenHeight);
  _scissorRect.right = static_cast<LONG>(_screenWidth);
  _scissorRect.bottom = static_cast<LONG>(_screenHeight);
}

ComPtr<ID3D12Resource> Luna::DX12Backend::GetRenderTarget(int32 index) {
  return _renderTargets[index];
}

const char *Luna::DX12Backend::GetBackendName() const { return "DirectX 12"; }