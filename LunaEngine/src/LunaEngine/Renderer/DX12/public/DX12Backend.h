#pragma once

#include <LunaEngine/Renderer/IRenderBackend.h>

struct GLFWwindow;
namespace Luna
{
using Microsoft::WRL::ComPtr;
using int32 = __int32;
using uint32 = unsigned __int32;

class DX12Backend : public IRenderBackend
{
  public:
    /**/ DX12Backend() = default;
    ~DX12Backend() override = default;

    bool Init(void *windowHandler, uint32_t width, uint32_t height) override;
    void BeginFrame() override;
    void InitImGui(void *windowHandler) override;
    void StartImGui() override;
    void RenderImGui() override;
    void DrawFrame() override;
    void ShutdownImGui() override;
    void EndFrame() override;
    void Resize(uint32_t width, uint32_t height) override;
    const char *GetBackendName() const override;
    
    void Draw(uint32_t vertexCount) override;
    void SetVertexBuffer(class IBuffer *buffer) override;
    void BindPipeline(class IPipeline* pipeline) override;

    ComPtr<IDXGIFactory> GetDXGIFactory() const
    {
        return _mdxgiFactory;
    }
    ComPtr<ID3D12Device> GetDevice() const
    {
        return _device;
    }
    ComPtr<ID3D12CommandQueue> GetCommandQueue() const
    {
        return _commandQueue;
    }
    
    ComPtr<ID3D12GraphicsCommandList> GetCommandList() const
    {
        return _commandList;
    }
    
    ComPtr<IDXGISwapChain> GetSwapChain() const
    {
        return _swapChain;
    }
    ComPtr<ID3D12Resource> GetRenderTarget(int32 index) const
    {
        return _rtvBuffer[index];
    }
    uint32 GetCurrentBackBufferIndex() const
    {
        return _backbufferIndex;
    }
    ComPtr<ID3D12Resource> GetCurrentBackBufferResource() const
    {
        return _rtvBuffer[_backbufferIndex];
    }
    D3D12_CPU_DESCRIPTOR_HANDLE GetRTV(int32 index) const
    {
        return _rtvHandle[index];
    }
    D3D12_CPU_DESCRIPTOR_HANDLE GetBackBufferView();

  private:
    bool CheckIfImGuiData();
    void CreateDebugLayer();
    bool CreateFactoryAndAdapter();
    bool CreateDevice();
    bool CreateCommandQueueAndFenceEvent();
    void WaitSync();
    bool CreateSwapChain();
    bool CreateRenderTarget();
    bool CreateImGuiRenderTarget();
    void SetResolution(const uint32_t &width, const uint32_t &height);

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

    ComPtr<IDXGIFactory6> _mdxgiFactory;
    ComPtr<IDXGIAdapter1> _adapter;
    ComPtr<ID3D12Device> _device;
    UINT m4xMsaaQuality = 0;

    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS msQualityLevels;

    // CommandQueue
    ComPtr<ID3D12CommandQueue> _commandQueue;
    ComPtr<ID3D12CommandAllocator> _commandAllocator;
    ComPtr<ID3D12GraphicsCommandList> _commandList;
    ComPtr<ID3D12Fence> _fence;
    UINT64 _fenceValue = 0;
    HANDLE _fenceEvent = nullptr; // INVALID_HANDLE_VALUE

    // Swap Chain
    ComPtr<IDXGISwapChain4> _swapChain;
    DXGI_FORMAT mBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    ComPtr<ID3D12Resource> _rtvBuffer[SWAP_CHAIN_BUFFER_COUNT];
    UINT _backbufferIndex = 0;

    // Descriptor Heap
    ComPtr<ID3D12DescriptorHeap> _rtvHeap;
    ComPtr<ID3D12DescriptorHeap> _imGuiSrvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE _rtvHandle[SWAP_CHAIN_BUFFER_COUNT];

    // Vertex Buffer
    std::shared_ptr<class DX12Buffer> _triangleVertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW _vertexBufferView;
    std::shared_ptr<class DX12Pipeline> _trianglePipeline;
};
} // namespace Luna