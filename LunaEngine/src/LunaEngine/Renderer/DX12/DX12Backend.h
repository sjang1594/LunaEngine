#pragma once

#include <LunaEngine/Renderer/IRenderBackend.h>

struct GLFWwindow;
namespace Luna {
class DX12Backend : public IRenderBackend {
 public:
  DX12Backend();
  ~DX12Backend() override;

  bool Init(void* windowHandler, uint32_t width,
            uint32_t height) override;
  void BeginFrame() override;
  void InitImGui(void* windowHandler) override;
  void StartImGui() override;
  void RenderImGui() override;
  void DrawFrame() override;
  void ShutdownImGui() override;
  void EndFrame() override;
  void Resize(uint32_t width, uint32_t height) override;
  const char* GetBackendName() const override;

  ComPtr<IDXGIFactory> GetDXGIFactory() { return _dxgiFactory; }
  ComPtr<ID3D12Device> GetDevice() { return _device; }
  ComPtr<ID3D12CommandQueue> GetCommandQueue() { return _commandQueue; }
  ComPtr<IDXGISwapChain> GetSwapChain() { return _swapChain; }
  ComPtr<ID3D12Resource> GetRenderTarget(int32 index);
  uint32 GetCurrentBackBufferIndex() { return _backbufferIndex; }
  ComPtr<ID3D12Resource> GetCurrentBackBufferResource() {
    return _renderTargets[_backbufferIndex];
  }

  D3D12_CPU_DESCRIPTOR_HANDLE GetRTV(int32 index) { return _rtvHandle[index]; }
  D3D12_CPU_DESCRIPTOR_HANDLE GetBackBufferView();

 private:
  void CreateDebugLayer();
  bool CreateFactoryAndAdapter();
  bool CreateDevice();
  bool CreateCommandQueueAndFenceEvent();
  void WaitSync();
  bool CreateSwapChain();
  bool CreateDescriptorHeap();
  bool CreateImGuiDescriptorHeap();
  void SetResolution(const uint32_t& width, const uint32_t& height);

 private:
  int _screenWidth;
  int _screenHeight;

  HWND _mainWindow = nullptr;

  // ViewPort
  D3D12_VIEWPORT _screenViewport = {};
  D3D12_RECT _scissorRect = {};

  // Device Related
#if defined(_DEBUG)
  ComPtr<ID3D12Debug> _debugController;
#endif

  ComPtr<IDXGIFactory6> _dxgiFactory;
  ComPtr<IDXGIAdapter1> _adapter;
  ComPtr<ID3D12Device> _device;

  // CommandQueue
  ComPtr<ID3D12CommandQueue> _commandQueue;
  ComPtr<ID3D12CommandAllocator> _commandAllocator;
  ComPtr<ID3D12GraphicsCommandList> _commandList;
  ComPtr<ID3D12Fence> _fence;
  UINT64 _fenceValue = 0;
  HANDLE _fenceEvent = nullptr;  // INVALID_HANDLE_VALUE

  // Swap Chain
  ComPtr<IDXGISwapChain4> _swapChain;
  ComPtr<ID3D12Resource> _renderTargets[SWAP_CHAIN_BUFFER_COUNT];
  UINT _backbufferIndex = 0;

  // Descriptor Heap
  ComPtr<ID3D12DescriptorHeap> _rtvHeap;
  ComPtr<ID3D12DescriptorHeap> _imguiSrvHeap;
  UINT _rtvHeapSize = 0;
  D3D12_CPU_DESCRIPTOR_HANDLE _rtvHandle[SWAP_CHAIN_BUFFER_COUNT];
};
}  // namespace Luna