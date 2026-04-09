#pragma once

#include "DX12Device.h"

namespace Luna
{
using Microsoft::WRL::ComPtr;
using int32 = __int32;
using uint32 = unsigned __int32;

class DX12Backend : public IRenderBackend
{
  public:
    DX12Backend() = default;
    ~DX12Backend() override;

    bool Init(void *windowHandler, uint32_t width, uint32_t height) override;
    void Shutdown() override;
    void BeginFrame() override;
    void InitImGui(void *windowHandler) override;
    void StartImGui() override;
    void RenderImGui() override;
    void DrawFrame() override;
    void ShutdownImGui() override;
    void EndFrame() override;
    void Resize(uint32_t width, uint32_t height) override;
    void Draw(uint32_t vertexCount) override;
    void SetVertexBuffer(class IBuffer *buffer) override;
    void BindPipeline(class IPipeline* pipeline) override;

    const char *GetBackendName() const override { return "DirectX 12"; }

    ComPtr<ID3D12Device>              GetDevice()         const { return _device; }
    ComPtr<IDXGIFactory6>             GetDXGIFactory()    const { return _mdxgiFactory; }
    ComPtr<ID3D12CommandQueue>        GetCommandQueue()   const { return _commandQueue; }
    ComPtr<ID3D12GraphicsCommandList> GetCommandList()    const { return _commandList; }
    ComPtr<IDXGISwapChain4>           GetSwapChain()      const { return _swapChain; }
    ComPtr<ID3D12Resource>            GetRenderTarget(int32 index) const { return _rtvBuffer[index]; }
    ComPtr<ID3D12Resource>            GetCurrentBackBufferResource() const;
    D3D12_CPU_DESCRIPTOR_HANDLE       GetRTV(int32 index) const { return _rtvHandle[index]; }
    D3D12_CPU_DESCRIPTOR_HANDLE       GetBackBufferView();

  private:
    bool CheckIfImGuiData();
    bool CreateCommandQueueAndFenceEvent();
    void WaitSync();
    bool CreateSwapChain();
    bool CreateRenderTarget();
    bool CreateImGuiRenderTarget();
    void SetResolution(const uint32_t &width, const uint32_t &height);
    void SetPipelineState(D3D12_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST,
                          const D3D12_VIEWPORT* viewport = nullptr,
                          const D3D12_RECT* scissorRect = nullptr);

  private:
    int _screenWidth  = 0;
    int _screenHeight = 0;

    HWND _mainWindow = nullptr;

    // Viewport
    D3D12_VIEWPORT _screenViewport = {};
    D3D12_RECT     _scissorRect    = {};

    // Device — owned by DX12Device, references cached here for convenience
    std::unique_ptr<DX12Device> _dx12Device;
    ComPtr<ID3D12Device>        _device;
    ComPtr<IDXGIFactory6>       _mdxgiFactory;

    // Command recording
    ComPtr<ID3D12CommandQueue>        _commandQueue;
    ComPtr<ID3D12CommandAllocator>    _commandAllocator;
    ComPtr<ID3D12GraphicsCommandList> _commandList;

    // GPU synchronization
    ComPtr<ID3D12Fence> _fence;
    UINT64              _fenceValue = 0;
    HANDLE              _fenceEvent = nullptr;

    // Swap chain
    ComPtr<IDXGISwapChain4>  _swapChain;
    DXGI_FORMAT              _backBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    ComPtr<ID3D12Resource>   _rtvBuffer[SWAP_CHAIN_BUFFER_COUNT];

    // Descriptor heaps
    ComPtr<ID3D12DescriptorHeap> _rtvHeap;
    ComPtr<ID3D12DescriptorHeap> _imGuiSrvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE  _rtvHandle[SWAP_CHAIN_BUFFER_COUNT] = {};

    // Pipelines
    std::unique_ptr<class DX12Pipeline> _trianglePipeline;
};
} // namespace Luna