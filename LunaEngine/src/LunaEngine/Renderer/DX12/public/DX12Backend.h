#pragma once

// DX12Device is only used as a unique_ptr member — forward-declaration keeps
// D3D12 device types out of every translation unit that includes DX12Backend.h.
// The full definition is pulled in by DX12Backend.cpp where it is actually needed.
#include "D3D12MemAlloc.h"
#include "Renderer/Mesh.h"
#include "Renderer/HAL/Public/IRenderBackend.h"
#include "LunaEngine/Renderer/RenderGraph.h"

namespace Luna
{
// Forward-declare instead of including DX12Device.h — avoids dragging D3D12 types
// into every TU that includes this header.
class DX12Device;

using Microsoft::WRL::ComPtr;
using int32  = __int32;
using uint32 = unsigned __int32;

// ---------------------------------------------------------------------------
// Per-frame GPU resources — two sets allow the CPU to record frame N+1 while
// the GPU is still executing frame N, eliminating the per-frame WaitSync stall.
// ---------------------------------------------------------------------------
struct FrameResource
{
    ComPtr<ID3D12CommandAllocator> cmdAllocator;

    // b0 — MVP transform constant buffer
    ComPtr<ID3D12Resource>    mvpCB;
    void*                     mvpCBMapped  = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS mvpCBGPUAddr = 0;

    // b2 — SceneBuffer constant buffer (PBR pass only)
    ComPtr<ID3D12Resource>    sceneCB;
    void*                     sceneCBMapped  = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS sceneCBGPUAddr = 0;

    UINT64 fenceValue = 0;
};

class DX12Backend : public IRenderBackend
{
  public:
    DX12Backend();
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
    void UpdateMVP(const XMFLOAT4X4& model, const XMFLOAT4X4& view, const XMFLOAT4X4& proj) override;
    void DrawMesh(const Mesh* mesh, const XMFLOAT4X4& model) override;

    // Load all primitives from a glTF/GLB file and store them in _sceneMeshes.
    // Returns references (shared_ptrs) so callers can hand them to MeshRenderers.
    std::vector<std::shared_ptr<Mesh>> LoadMeshes(const std::string& path);

    const char *GetBackendName() const override { return "DirectX 12"; }

    // P2-04: respect the vsync flag set by the application layer
    void SetVSync(bool vsync) override { _vsync = vsync; }

    ComPtr<ID3D12Device>              GetDevice()         const { return _device; }
    ComPtr<IDXGIFactory6>             GetDXGIFactory()    const { return _mdxgiFactory; }
    ComPtr<ID3D12CommandQueue>        GetCommandQueue()   const { return _commandQueue; }
    ComPtr<ID3D12GraphicsCommandList> GetCommandList()    const { return _commandList; }
    ComPtr<IDXGISwapChain4>           GetSwapChain()      const { return _swapChain; }
    ComPtr<ID3D12Resource>            GetRenderTarget(int32 index) const { return _rtvBuffer[index]; }
    ComPtr<ID3D12Resource>            GetCurrentBackBufferResource() const;
    D3D12_CPU_DESCRIPTOR_HANDLE       GetRTV(int32 index) const { return _rtvHandle[index]; }
    D3D12_CPU_DESCRIPTOR_HANDLE       GetBackBufferView();

    // SRV heap slot allocation — callers get the next free CPU/GPU descriptor handle pair
    // Returns the index so callers can refer back to it
    UINT AllocateSRVSlot(D3D12_CPU_DESCRIPTOR_HANDLE& outCPU, D3D12_GPU_DESCRIPTOR_HANDLE& outGPU);

    D3D12MA::Allocator* GetD3D12MAAllocator() const { return _d3d12maAllocator; }

  private:
    // Init helpers
    bool CheckIfImGuiData();
    bool CreateCommandQueueAndFenceEvent();
    bool CreateSwapChain();
    bool CreateRenderTarget();
    bool CreateImGuiRenderTarget();
    bool CreateDepthBuffer();
    bool CreateVertexBuffer();
    bool CreateMVPConstantBuffer();
    bool CreateSceneConstantBuffer();
    bool CreateFallbackShadowSRV();
    bool InitD3D12MA();
    bool CreateShadowUAV();

    // Synchronization
    void WaitForFrame(UINT i);
    void WaitAllFrames();

    void SetResolution(const uint32_t &width, const uint32_t &height);
    void SetPipelineState(D3D12_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST,
                          const D3D12_VIEWPORT* viewport = nullptr,
                          const D3D12_RECT* scissorRect = nullptr);

  private:
    // -----------------------------------------------------------------------
    // Constants
    // -----------------------------------------------------------------------
    static constexpr UINT FRAMES_IN_FLIGHT  = 2;
    static constexpr UINT SRV_HEAP_SIZE     = 1024; // [0]=ImGui, [1..N]=textures, [N+1..]=DXR UAV

    // -----------------------------------------------------------------------
    // Window / viewport
    // -----------------------------------------------------------------------
    int  _screenWidth  = 0;
    int  _screenHeight = 0;
    HWND _mainWindow   = nullptr;
    bool _vsync        = true;  // P2-04: toggled via SetVSync(); drives Present() interval

    D3D12_VIEWPORT _screenViewport = {};
    D3D12_RECT     _scissorRect    = {};

    // -----------------------------------------------------------------------
    // Device
    // -----------------------------------------------------------------------
    std::unique_ptr<DX12Device> _dx12Device;
    ComPtr<ID3D12Device>        _device;
    ComPtr<IDXGIFactory6>       _mdxgiFactory;

    // -----------------------------------------------------------------------
    // D3D12 Memory Allocator
    // -----------------------------------------------------------------------
    D3D12MA::Allocator* _d3d12maAllocator = nullptr;

    // -----------------------------------------------------------------------
    // Command recording (command list is shared; allocator is per-frame)
    // -----------------------------------------------------------------------
    ComPtr<ID3D12CommandQueue>        _commandQueue;
    ComPtr<ID3D12GraphicsCommandList> _commandList;

    // -----------------------------------------------------------------------
    // GPU synchronization — ring buffer with FRAMES_IN_FLIGHT entries
    // -----------------------------------------------------------------------
    ComPtr<ID3D12Fence> _fence;
    UINT64              _globalFenceValue = 0;
    HANDLE              _fenceEvent       = nullptr;

    FrameResource _frames[FRAMES_IN_FLIGHT];
    UINT          _frameIndex = 0;

    // -----------------------------------------------------------------------
    // Swap chain
    // -----------------------------------------------------------------------
    ComPtr<IDXGISwapChain4>  _swapChain;
    DXGI_FORMAT              _backBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    ComPtr<ID3D12Resource>   _rtvBuffer[SWAP_CHAIN_BUFFER_COUNT];

    // -----------------------------------------------------------------------
    // Descriptor heaps
    // -----------------------------------------------------------------------
    ComPtr<ID3D12DescriptorHeap> _rtvHeap;
    ComPtr<ID3D12DescriptorHeap> _imGuiSrvHeap;   // CBV/SRV/UAV — shader-visible, SRV_HEAP_SIZE slots
    ComPtr<ID3D12DescriptorHeap> _dsvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE  _rtvHandle[SWAP_CHAIN_BUFFER_COUNT] = {};
    D3D12_CPU_DESCRIPTOR_HANDLE  _dsvHandle = {};
    UINT                         _srvDescriptorSize = 0;
    UINT                         _srvAllocIndex     = 1; // slot 0 reserved for ImGui font

    // -----------------------------------------------------------------------
    // Depth buffer
    // -----------------------------------------------------------------------
    ComPtr<ID3D12Resource>  _depthBuffer;

    // -----------------------------------------------------------------------
    // Triangle geometry (UPLOAD heap — Phase 1 legacy, replaced by D3D12MA in Phase 2C)
    // -----------------------------------------------------------------------
    ComPtr<ID3D12Resource>   _vertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW _vertexBufferView = {};
    D3D12MA::Allocation*     _vertexBufferAlloc = nullptr;

    // -----------------------------------------------------------------------
    // Pipelines
    // -----------------------------------------------------------------------
    std::unique_ptr<class DX12Pipeline> _cbvPipeline;          // triangle (constantbuffer shaders)
    std::unique_ptr<class DX12Pipeline> _pbrPipeline;          // full PBR (Phase 3C)
    std::unique_ptr<class DX12Pipeline> _meshPreviewPipeline;  // PBR vertex + normal shading

    // -----------------------------------------------------------------------
    // Scene meshes — loaded from glTF, drawn via MeshRenderer::Render()
    // -----------------------------------------------------------------------
    std::vector<std::shared_ptr<Mesh>> _sceneMeshes;

    // Cached camera matrices — set by UpdateMVP(), used by DrawMesh() per object
    XMFLOAT4X4 _lastView = {};
    XMFLOAT4X4 _lastProj = {};

    // -----------------------------------------------------------------------
    // DXR shadow map
    // -----------------------------------------------------------------------
    bool                   _dxrSupported         = false;
    ComPtr<ID3D12Resource> _shadowUAV;             // R32_FLOAT, viewport-sized, UAV-flagged
    D3D12MA::Allocation*   _shadowUAVAlloc        = nullptr;
    UINT                   _shadowUAVSRVIndex     = 0; // SRV slot index in _imGuiSrvHeap
    UINT                   _fallbackShadowSRVIdx  = 0; // SRV for 1x1 white R32 texture (1.0 = fully lit)
    ComPtr<ID3D12Resource> _fallbackShadowTex;         // persistent 1x1 R32_FLOAT = 1.0f
    D3D12MA::Allocation*   _fallbackShadowAlloc   = nullptr;

    // DXR pipeline and acceleration structure (forward-declared — included in .cpp)
    std::unique_ptr<class DX12AccelStructure> _accelStructure;
    std::unique_ptr<class DX12RTPipeline>     _rtPipeline;

    // -----------------------------------------------------------------------
    // Phase 6 — Render Graph
    // Rebuilt each DrawFrame(); tracks resource states and emits barriers.
    // -----------------------------------------------------------------------
    RenderGraph _renderGraph;

    // True when the shadow UAV was transitioned to PIXEL_SHADER_RESOURCE by the
    // DXR pass and has not yet been restored. DrawMesh() reads it in that state;
    // the next DrawFrame() restores it to UAV before the new DXR dispatch.
    bool _shadowInSRVState = false;
};
} // namespace Luna
