#pragma once

#include "DX12Device.h"
#include "D3D12MemAlloc.h"
#include "Renderer/Mesh.h"
#include "Renderer/HAL/Public/IRenderBackend.h"
#include "DX12GPUProfiler.h"
#include <unordered_map>

// Forward declarations
namespace Luna { class CameraSensor; class LiDARSensor; }

namespace Luna
{
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
    ComPtr<ID3D12Resource>         mvpCB;
    void*                          mvpCBMapped  = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS      mvpCBGPUAddr = 0;
    // Phase 7: per-frame scene constants (invVP, eyePos, lightDir/Color)
    ComPtr<ID3D12Resource>         sceneCB;
    void*                          sceneCBMapped  = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS      sceneCBGPUAddr = 0;
    UINT64                         fenceValue   = 0;
    // Phase 13: per-frame async compute command allocator
    ComPtr<ID3D12CommandAllocator> computeCmdAllocator;
    UINT64                         computeFenceValue = 0;
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
    void CompositeFrame() override;
    void ShutdownImGui() override;
    void EndFrame() override;
    void Resize(uint32_t width, uint32_t height) override;
    void Draw(uint32_t vertexCount) override;
    void SetVertexBuffer(class IBuffer *buffer) override;
    void BindPipeline(class IPipeline* pipeline) override;
    void UpdateMVP(const XMFLOAT4X4& model, const XMFLOAT4X4& view, const XMFLOAT4X4& proj) override;
    void DrawMesh(const Mesh* mesh, const XMFLOAT4X4& model) override;
    void SetVSync(bool vsync) override { _vsync = vsync; }

    // Phase 22: GPU profiler
    IGPUProfiler* GetGPUProfiler() override { return &_gpuProfiler; }

    // Load all primitives from a glTF/GLB file and store them in _sceneMeshes.
    // Returns references (shared_ptrs) so callers can hand them to MeshRenderers.
    std::vector<std::shared_ptr<Mesh>> LoadMeshes(const std::string& path) override;
    std::vector<std::shared_ptr<Mesh>> LoadCalibrationScene() override;
    std::shared_ptr<Mesh> CreateProceduralBoxScene(
        const std::vector<DirectX::XMFLOAT4X4>& instanceTransforms) override;
    DirectX::XMFLOAT4X4 GetCurrentVP() const override { return _ppPrevVP; }
    std::vector<XMFLOAT4X4> GetLastLoadTransforms() const override { return _lastLoadTransforms; }

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
    bool CreateSceneCBs();   // Phase 7: per-frame SceneConstants CB
    bool InitD3D12MA();
    bool CreateShadowUAV();
    bool CreateGBuffer();    // Phase 7: G-buffer textures + SRV/RTV descriptors
    void DestroyGBuffer();   // Phase 7: release G-buffer resources (not SRV slot indices)
    bool CreateCSMResources();  // Phase 8: CSM shadow map array + DSV heap + SRV slot
    void DestroyCSMResources(); // Phase 8: release CSM resources (not SRV slot index)
    void UpdateCSMMatrices(const XMFLOAT4X4& view, const XMFLOAT4X4& proj); // Phase 8
    void DrawCSMPass();         // Phase 8: 4-cascade depth draw

    // Phase 9: SSAO (declared in private section with members below)

    // post-process — declared here (TAAConstants is in private members below)
    bool CreatePostProcessResources();
    void DestroyPostProcessResources();
    void DrawTAAPass();
    void DrawBloomBrightPass();
    void DrawBloomBlurPass(bool horizontal);  // same pipeline, direction via root constants
    void DrawToneMappingPass(UINT backIdx);

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
    static constexpr UINT SRV_HEAP_SIZE     = 4096; // [0]=ImGui, [1..N]=textures, [N+1..]=DXR UAV

    // -----------------------------------------------------------------------
    // Window / viewport
    // -----------------------------------------------------------------------
    int  _screenWidth  = 0;
    int  _screenHeight = 0;
    HWND _mainWindow   = nullptr;

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
    // Phase 13: Async compute queue — GPU cull dispatch overlaps graphics
    // -----------------------------------------------------------------------
    ComPtr<ID3D12CommandQueue>        _computeQueue;
    ComPtr<ID3D12GraphicsCommandList> _computeCommandList;
    ComPtr<ID3D12Fence>               _computeFence;
    UINT64                            _computeFenceValue = 0;
    HANDLE                            _computeFenceEvent = nullptr;
    bool                              _asyncComputeReady = false;

    void WaitForComputeFrame(UINT i);
    void DispatchCullAsync();  // records + submits cull on _computeQueue

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
    std::unique_ptr<class DX12Pipeline> _cbvPipeline;           // triangle (constantbuffer shaders)
    std::unique_ptr<class DX12Pipeline> _gbufferPipeline;       // Phase 7: G-buffer fill (PBR vert + gbuffer.frag)
    std::unique_ptr<class DX12Pipeline> _lightingPipeline;      // Phase 7: deferred lighting (fullscreen vert + deferred_lighting.frag)
    std::unique_ptr<class DX12Pipeline> _meshPreviewPipeline;   // PBR vertex + normal shading

    // -----------------------------------------------------------------------
    // Scene meshes — loaded from glTF, drawn via MeshRenderer::Render()
    // -----------------------------------------------------------------------
    std::vector<std::shared_ptr<Mesh>> _sceneMeshes;
    std::vector<XMFLOAT4X4>            _lastLoadTransforms; // Phase 21

    // Cached camera matrices — set by UpdateMVP(), used by DrawMesh() per object
    XMFLOAT4X4 _lastView = {};
    XMFLOAT4X4 _lastProj = {};

    // Cached scene lighting — updated each frame in CompositeFrame(); used by sensor renders
    XMFLOAT3 _cachedLightDir   = {0.447f, 0.894f, 0.447f};
    XMFLOAT4 _cachedLightColor = {1.0f, 1.0f, 1.0f, 1.0f}; // rgb + intensity

    // -----------------------------------------------------------------------
    // Presentation
    // -----------------------------------------------------------------------
    bool _vsync = false;  // P2-04: controlled via SetVSync()

    // -----------------------------------------------------------------------
    // DXR shadow map
    // -----------------------------------------------------------------------
    bool                   _dxrSupported      = false;
    ComPtr<ID3D12Resource> _shadowUAV;          // R32_FLOAT, viewport-sized, UAV-flagged
    D3D12MA::Allocation*   _shadowUAVAlloc     = nullptr;
    UINT                   _shadowUAVSRVIndex  = 0; // SRV slot index in _imGuiSrvHeap

    // DXR pipeline and acceleration structure (forward-declared — included in .cpp)
    std::unique_ptr<class DX12AccelStructure> _accelStructure;
    std::unique_ptr<class DX12RTPipeline>     _rtPipeline;

    // -----------------------------------------------------------------------
    // Phase 7: G-buffer resources
    // -----------------------------------------------------------------------
    static constexpr UINT GBUFFER_COUNT = 3;

    // -----------------------------------------------------------------------
    // Phase 8: Cascaded Shadow Maps (CSM)
    // -----------------------------------------------------------------------
    static constexpr UINT CSM_CASCADE_COUNT = 4;
    static constexpr UINT CSM_SHADOW_SIZE   = 2048;

    // Texture2DArray: 4 slices × 2048×2048, R32_TYPELESS (DSV=D32_FLOAT, SRV=R32_FLOAT)
    ComPtr<ID3D12Resource>       _csmShadowMap;
    D3D12MA::Allocation*         _csmShadowMapAlloc         = nullptr;

    // Separate DSV heap for the 4 cascade slice DSVs (keeps _dsvHeap with 1 slot)
    ComPtr<ID3D12DescriptorHeap> _csmDsvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE  _csmDsvHandles[4]          = {};

    // SRV slot in _imGuiSrvHeap — Texture2DArray SRV covering all 4 slices
    UINT _csmSRVIndex = UINT_MAX;

    // Depth-only pipeline: CSMDepth root sig + csm_depth.vert.hlsl, no PS
    std::unique_ptr<class DX12Pipeline> _csmPipeline;

    // Per-cascade light-space ViewProjection matrices (updated in UpdateCSMMatrices each frame)
    XMFLOAT4X4 _csmLightVP[4]   = {};
    XMFLOAT4   _csmCascadeSplits = {};  // view-space Z far plane per cascade

    // Cached per-mesh model matrices from the last DrawMesh() pass (for CSM depth pre-pass)
    // Per-instance cache for the CSM shadow pass: (mesh ptr, world model matrix).
    // Populated by DrawMesh() each frame; consumed by DrawCSMPass() the following frame.
    struct MeshInstance { const Mesh* mesh; XMFLOAT4X4 model; };
    std::vector<MeshInstance> _lastMeshModels;

    // Separate RTV heap for G-buffer targets (keeps _rtvHeap clean)
    ComPtr<ID3D12DescriptorHeap> _gbufferRtvHeap;
    ComPtr<ID3D12Resource>       _gbuffer[GBUFFER_COUNT];
    D3D12MA::Allocation*         _gbufferAlloc[GBUFFER_COUNT] = {};
    D3D12_CPU_DESCRIPTOR_HANDLE  _gbufferRTV[GBUFFER_COUNT]   = {};
    // SRV slot indices — use UINT_MAX as "not yet allocated" sentinel
    // (slot 0 can be legitimately occupied, so 0 is not a safe sentinel)
    UINT _gbufferSRVIndex[GBUFFER_COUNT] = {UINT_MAX, UINT_MAX, UINT_MAX};
    UINT _depthSRVIndex  = UINT_MAX;  // SRV for depth as R32_FLOAT (consecutive after GB2)
    UINT _shadowSRVIndex = UINT_MAX;  // read-only SRV for shadow UAV (consecutive after depth)

    // -----------------------------------------------------------------------
    // Phase 9: Screen-Space Ambient Occlusion (SSAO)
    // -----------------------------------------------------------------------
    bool CreateSSAOResources();
    void DestroySSAOResources();
    void DrawSSAOPass();          // raw SSAO → _ssaoRT
    void DrawSSAOBlurPass();      // blur _ssaoRT → _ssaoBlurRT

    // Half-resolution (screenW/2 × screenH/2) R8_UNORM render targets
    ComPtr<ID3D12Resource>  _ssaoRT;               // raw SSAO output
    D3D12MA::Allocation*    _ssaoRTAlloc     = nullptr;
    ComPtr<ID3D12Resource>  _ssaoBlurRT;           // blurred SSAO (read by deferred lighting)
    D3D12MA::Allocation*    _ssaoBlurRTAlloc = nullptr;

    // Dedicated RTV heap for the two SSAO targets
    ComPtr<ID3D12DescriptorHeap> _ssaoRtvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE  _ssaoRtv     = {};
    D3D12_CPU_DESCRIPTOR_HANDLE  _ssaoBlurRtv = {};

    // SRV slots in _imGuiSrvHeap
    UINT _ssaoSRVIndex     = UINT_MAX;  // raw SSAO SRV  (input to blur pass)
    UINT _ssaoBlurSRVIndex = UINT_MAX;  // blurred SSAO SRV (t5 in deferred lighting)

    // Noise texture: 4×4 R8G8_UNORM random rotation vectors
    ComPtr<ID3D12Resource>  _ssaoNoiseTex;
    D3D12MA::Allocation*    _ssaoNoiseAlloc = nullptr;
    UINT                    _ssaoNoiseSRVIndex = UINT_MAX;

    // Kernel samples + per-frame CB
    static constexpr int    SSAO_SAMPLE_COUNT = 16;
    struct SSAOConstants
    {
        XMFLOAT4            samples[SSAO_SAMPLE_COUNT];
        XMFLOAT4X4          projection;
        XMFLOAT4X4          invProjection;
        XMFLOAT4X4          view;
        XMFLOAT2            noiseScale;
        float               radius = 0.5f;
        float               bias   = 0.025f;
    };
    SSAOConstants           _ssaoKernel{};           // generated once at init
    ComPtr<ID3D12Resource>  _ssaoCB;
    D3D12MA::Allocation*    _ssaoCBAlloc = nullptr;
    void*                   _ssaoCBMapped = nullptr;

    // Pipelines
    std::unique_ptr<class DX12Pipeline> _ssaoPipeline;
    std::unique_ptr<class DX12Pipeline> _ssaoBlurPipeline;

    // -----------------------------------------------------------------------
    // Phase 10: Post-process stack — HDR buffer, TAA, Bloom, ACES tone map
    // -----------------------------------------------------------------------
    struct TAAConstants
    {
        XMFLOAT4X4 invViewProj;  // 64 B — current frame inverse (jittered) VP
        XMFLOAT4X4 prevViewProj; // 64 B — previous frame unjittered VP
        XMFLOAT2   jitter;       //  8 B
        XMFLOAT2   prevJitter;   //  8 B
        float      alpha;        //  4 B — blend weight (~0.1)
        float      _pad[3];      // 12 B → 160 B total → 256-byte aligned CB
    };

    bool _ppResourcesValid = false;
    UINT _ppFrameCount     = 0;      // Halton sequence frame index
    int  _taaHistoryIndex  = 0;      // ping-pong: 0 or 1
    XMFLOAT2   _ppCurJitter  = {};
    XMFLOAT2   _ppPrevJitter = {};
    XMFLOAT4X4 _ppPrevVP     = {};   // unjittered VP of previous frame

    // HDR intermediate RT (R16G16B16A16_FLOAT, full-res, REST=RENDER_TARGET)
    ComPtr<ID3D12Resource>       _hdrRT;
    D3D12MA::Allocation*         _hdrRTAlloc  = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE  _hdrRTV      = {};
    UINT                         _hdrSRVIndex = UINT_MAX;

    // TAA history ping-pong (R16G16B16A16_FLOAT, full-res × 2)
    ComPtr<ID3D12Resource>       _taaHistory[2];
    D3D12MA::Allocation*         _taaHistoryAlloc[2]    = {};
    D3D12_CPU_DESCRIPTOR_HANDLE  _taaHistoryRTV[2]      = {};
    UINT                         _taaHistorySRVIndex[2] = {UINT_MAX, UINT_MAX};

    // Per-frame TAA constant buffers (FRAMES_IN_FLIGHT = 2)
    ComPtr<ID3D12Resource>       _taaCB[FRAMES_IN_FLIGHT];
    D3D12MA::Allocation*         _taaCBAlloc[FRAMES_IN_FLIGHT]  = {};
    void*                        _taaCBMapped[FRAMES_IN_FLIGHT] = {};

    // Bloom half-res buffers (R11G11B10_FLOAT)
    // _bloomBrightRT: bright-pass output AND V-blur final result (reused)
    // _bloomBlurRT:   H-blur intermediate
    ComPtr<ID3D12Resource>       _bloomBrightRT;
    D3D12MA::Allocation*         _bloomBrightAlloc    = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE  _bloomBrightRTV      = {};
    UINT                         _bloomBrightSRVIndex = UINT_MAX;

    ComPtr<ID3D12Resource>       _bloomBlurRT;
    D3D12MA::Allocation*         _bloomBlurAlloc    = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE  _bloomBlurRTV      = {};
    UINT                         _bloomBlurSRVIndex = UINT_MAX;

    // Combined RTV heap for all Phase 10 targets: [HDR, hist0, hist1, bright, blurH]
    ComPtr<ID3D12DescriptorHeap> _ppRtvHeap;

    // Phase 10 pipelines
    std::unique_ptr<class DX12Pipeline> _lightingPipelineHDR;  // DeferredLighting, R16G16B16A16_FLOAT output
    std::unique_ptr<class DX12Pipeline> _taaPipeline;
    std::unique_ptr<class DX12Pipeline> _bloomBrightPipeline;
    std::unique_ptr<class DX12Pipeline> _bloomBlurPipeline;
    std::unique_ptr<class DX12Pipeline> _toneMappingPipeline;

    // -----------------------------------------------------------------------
    // Phase 12: GPU-driven rendering — indirect draw + GPU frustum culling
    // -----------------------------------------------------------------------
    void FlushDraws() override;
    bool CreateIndirectResources();
    void DestroyIndirectResources();
    void BuildMergedGeometry();        // merge all _sceneMeshes into single VB+IB
    void ExtractFrustumPlanes(const XMFLOAT4X4& view, const XMFLOAT4X4& proj, XMFLOAT4 planes[6]);

    bool _gpuDrivenReady = false;      // true after BuildMergedGeometry()

    // Merged geometry (all scene meshes concatenated into one VB + one IB)
    ComPtr<ID3D12Resource>  _mergedVB;
    D3D12MA::Allocation*    _mergedVBAlloc = nullptr;
    D3D12_VERTEX_BUFFER_VIEW _mergedVBView = {};
    ComPtr<ID3D12Resource>  _mergedIB;
    D3D12MA::Allocation*    _mergedIBAlloc = nullptr;
    D3D12_INDEX_BUFFER_VIEW _mergedIBView  = {};

    // MeshDrawInfo SSBO (GPU-visible, one entry per scene mesh)
    ComPtr<ID3D12Resource>  _meshInfoBuffer;
    D3D12MA::Allocation*    _meshInfoAlloc = nullptr;

    // Per-frame instance staging + GPU buffers
    static constexpr UINT MAX_GPU_OBJECTS = 4096;
    std::vector<GPUObjectData> _cpuInstances;  // CPU staging list (filled by DrawMesh)

    // GPU object data SSBO (uploaded from _cpuInstances each frame)
    // Per-frame to avoid CPU-GPU race (Bug #010: previous frame's GPU may still be reading)
    ComPtr<ID3D12Resource>  _objectDataBuffer[FRAMES_IN_FLIGHT];
    D3D12MA::Allocation*    _objectDataAlloc[FRAMES_IN_FLIGHT] = {};
    void*                   _objectDataMapped[FRAMES_IN_FLIGHT] = {};

    // Indirect argument buffer — one per frame slot to avoid compute/graphics queue race
    ComPtr<ID3D12Resource>  _indirectArgBuffer[FRAMES_IN_FLIGHT];

    // Draw count buffer — one per frame slot (same reason)
    ComPtr<ID3D12Resource>  _drawCountBuffer[FRAMES_IN_FLIGHT];

    // Per-frame UAV descriptors for draw count buffer
    D3D12_CPU_DESCRIPTOR_HANDLE  _drawCountUAVCpu[FRAMES_IN_FLIGHT]    = {};
    D3D12_GPU_DESCRIPTOR_HANDLE  _drawCountUAVGpu[FRAMES_IN_FLIGHT]    = {};
    D3D12_CPU_DESCRIPTOR_HANDLE  _drawCountUAVNonVis[FRAMES_IN_FLIGHT] = {};
    ComPtr<ID3D12DescriptorHeap> _drawCountNonVisHeap[FRAMES_IN_FLIGHT];

    // Draw count readback (for CPU-side diagnostics, optional)
    ComPtr<ID3D12Resource>  _drawCountReadback;

    // ViewProj CB for indirect vertex shader (no model matrix — comes from SSBO)
    ComPtr<ID3D12Resource>  _viewProjCB[FRAMES_IN_FLIGHT];
    void*                   _viewProjCBMapped[FRAMES_IN_FLIGHT] = {};
    D3D12_GPU_VIRTUAL_ADDRESS _viewProjCBGPUAddr[FRAMES_IN_FLIGHT] = {};

    // Command signature for ExecuteIndirect (DrawIndexed + 2 root constants: matIdx, objIdx)
    ComPtr<ID3D12CommandSignature> _indirectCmdSignature;

    // Compute pipeline for GPU frustum culling
    std::unique_ptr<class DX12Pipeline> _gpuCullPipeline;

    // Indirect G-buffer fill pipeline (PBRIndirect root sig)
    std::unique_ptr<class DX12Pipeline> _indirectGBufPipeline;

    // -----------------------------------------------------------------------
    // Phase 25: Mesh Shader Pipeline
    // -----------------------------------------------------------------------
    bool _meshShadersSupported = false;
    bool _meshShaderReady      = false;

    bool CreateMeshShaderResources();
    void DestroyMeshShaderResources();

    std::unique_ptr<class DX12Pipeline> _meshShaderGBufPipeline;

    // Meshlet GPU buffers (uploaded alongside merged geometry)
    ComPtr<ID3D12Resource>  _meshletBuffer;        // StructuredBuffer<Meshlet>
    D3D12MA::Allocation*    _meshletBufferAlloc = nullptr;
    ComPtr<ID3D12Resource>  _meshletBoundsBuffer;  // StructuredBuffer<MeshletBounds>
    D3D12MA::Allocation*    _meshletBoundsAlloc = nullptr;
    ComPtr<ID3D12Resource>  _meshletVertexBuffer;  // StructuredBuffer<uint> (vertex indices)
    D3D12MA::Allocation*    _meshletVertexAlloc = nullptr;
    ComPtr<ID3D12Resource>  _meshletTriBuffer;     // StructuredBuffer<uint> (packed triangle indices)
    D3D12MA::Allocation*    _meshletTriAlloc = nullptr;
    ComPtr<ID3D12Resource>  _meshletMeshInfoBuffer;// StructuredBuffer<MeshletMeshInfo>
    D3D12MA::Allocation*    _meshletMeshInfoAlloc = nullptr;

    // Per-frame mesh shader constants CB
    struct MeshShaderConstants
    {
        DirectX::XMFLOAT4X4 viewMatrix;      // 64 B
        DirectX::XMFLOAT4X4 projMatrix;      // 64 B
        DirectX::XMFLOAT4   frustumPlanes[6]; // 96 B
        uint32_t objectIndex;     //  4 B
        uint32_t meshletOffset;   //  4 B
        uint32_t meshletCount;    //  4 B
        uint32_t _pad0;           //  4 B → 240 B → pad to 256 B
        uint32_t _pad1[4];        // 16 B → 256 B total
    };
    static_assert(sizeof(MeshShaderConstants) == 256, "MeshShaderConstants must be 256 bytes");

    ComPtr<ID3D12Resource>  _meshShaderCB[FRAMES_IN_FLIGHT];
    D3D12MA::Allocation*    _meshShaderCBAlloc[FRAMES_IN_FLIGHT] = {};
    void*                   _meshShaderCBMapped[FRAMES_IN_FLIGHT] = {};

    // Meshlet metadata per mesh (populated during BuildMergedGeometry)
    std::vector<uint32_t>   _meshMeshletOffsets;  // per-mesh: first meshlet index
    std::vector<uint32_t>   _meshMeshletCounts;   // per-mesh: meshlet count

    // -----------------------------------------------------------------------
    // Phase 23: Hi-Z Occlusion Culling
    // -----------------------------------------------------------------------
    bool CreateHiZResources();
    void DestroyHiZResources();
    void BuildHiZPyramid();  // dispatches mip-chain generation on graphics cmd list

    static constexpr UINT HIZ_MAX_MIPS = 13;  // enough for up to 4096² resolution

    ComPtr<ID3D12Resource>  _hizTexture;               // R32_FLOAT, full-res, multi-mip, UAV+SRV
    D3D12MA::Allocation*    _hizTextureAlloc = nullptr;
    UINT                    _hizMipCount = 0;          // actual mip count (computed from resolution)
    bool                    _hizReady    = false;       // true after first pyramid build

    // Per-mip SRV and UAV slot indices in _imGuiSrvHeap (for Hi-Z generation dispatches)
    UINT _hizMipSRVIndex[HIZ_MAX_MIPS] = {};  // SRV for reading mip N
    UINT _hizMipUAVIndex[HIZ_MAX_MIPS] = {};  // UAV for writing mip N

    // Full-pyramid SRV (all mips, for cull shader sampling via SampleLevel)
    UINT _hizFullSRVIndex = UINT_MAX;

    // Hi-Z generation compute pipeline (one dispatch per mip)
    std::unique_ptr<class DX12Pipeline> _hizGeneratePipeline;

    // Non-shader-visible UAV heap for per-mip ClearUnorderedAccessViewFloat
    ComPtr<ID3D12DescriptorHeap> _hizNonVisUAVHeap;

    // -----------------------------------------------------------------------
    // Phase 14: IBL Environment Mapping
    // -----------------------------------------------------------------------
public:
    // Load an equirectangular HDR file and kick off GPU precompute.
    // Call once after Init(). Returns true on success.
    bool LoadHDREnvironment(const std::string& hdrPath);

private:
    bool CreateIBLResources();          // allocate cubemap textures
    bool DispatchIBLPrecompute();       // compute equirect→cube, irr, prefilter, brdfLUT
    void DestroyIBLResources();
    void DrawSkyboxPass();              // renders env cubemap behind geometry (depth=1)

    static constexpr UINT ENV_CUBE_SIZE        = 512;
    static constexpr UINT IRR_CUBE_SIZE        = 32;
    static constexpr UINT PREFILTER_CUBE_SIZE  = 128;
    static constexpr UINT PREFILTER_MIP_COUNT  = 5;
    static constexpr UINT BRDF_LUT_SIZE        = 512;

    bool _iblReady = false;

    // Environment cubemap (equirect → cube, 512²×6, R16G16B16A16_FLOAT)
    ComPtr<ID3D12Resource>  _envCubemap;
    D3D12MA::Allocation*    _envCubemapAlloc    = nullptr;
    UINT                    _envCubemapSRVIndex = UINT_MAX; // TextureCube SRV

    // Per-face UAV descriptors for equirect→cube compute (6 entries in _iblUavHeap)
    ComPtr<ID3D12DescriptorHeap> _iblUavHeap;   // non-shader-visible heap for UAV creation

    // Irradiance cubemap (32²×6, R16G16B16A16_FLOAT)
    ComPtr<ID3D12Resource>  _irrCubemap;
    D3D12MA::Allocation*    _irrCubemapAlloc    = nullptr;
    UINT                    _irrCubemapSRVIndex = UINT_MAX;

    // Prefiltered env cubemap (128²×6×5 mips, R16G16B16A16_FLOAT)
    ComPtr<ID3D12Resource>  _prefilterCubemap;
    D3D12MA::Allocation*    _prefilterCubemapAlloc    = nullptr;
    UINT                    _prefilterCubemapSRVIndex = UINT_MAX;

    // BRDF integration LUT (512×512, RG16F)
    ComPtr<ID3D12Resource>  _brdfLUT;
    D3D12MA::Allocation*    _brdfLUTAlloc    = nullptr;
    UINT                    _brdfLUTSRVIndex = UINT_MAX;
    // UAV index in _imGuiSrvHeap for compute write
    UINT                    _brdfLUTUAVIndex = UINT_MAX;

    // Equirectangular source texture (upload + default heap pair, released after precompute)
    ComPtr<ID3D12Resource>  _equirectTex;
    D3D12MA::Allocation*    _equirectTexAlloc = nullptr;

    // IBL pipelines
    std::unique_ptr<class DX12Pipeline> _equirectToCubePipeline;
    std::unique_ptr<class DX12Pipeline> _irrConvPipeline;
    std::unique_ptr<class DX12Pipeline> _prefilterPipeline;
    std::unique_ptr<class DX12Pipeline> _brdfLutPipeline;
    std::unique_ptr<class DX12Pipeline> _skyboxPipeline;
    std::unique_ptr<class DX12Pipeline> _lightingPipelineIBL;  // deferred lighting + IBL variant

    // -----------------------------------------------------------------------
    // Phase 16B: Screen-Space Reflections (SSR)
    // -----------------------------------------------------------------------
    bool CreateSSRResources();
    void DestroySSRResources();
    void DrawSSRPass();

    // SSR render target (R16G16B16A16_FLOAT, UAV + SRV, full-res)
    ComPtr<ID3D12Resource>  _ssrRT;
    D3D12MA::Allocation*    _ssrRTAlloc     = nullptr;
    UINT                    _ssrUAVSRVIndex = UINT_MAX;
    UINT                    _ssrSRVIndex    = UINT_MAX;

    std::unique_ptr<class DX12Pipeline> _ssrComputePipeline;
    std::unique_ptr<class DX12Pipeline> _ssrBlendPipeline;
    bool                    _ssrRTFirstFrame = true;

    ComPtr<ID3D12Resource>  _ssrCB[FRAMES_IN_FLIGHT];
    D3D12MA::Allocation*    _ssrCBAlloc[FRAMES_IN_FLIGHT]  = {};
    void*                   _ssrCBMapped[FRAMES_IN_FLIGHT] = {};

    // -----------------------------------------------------------------------
    // Phase 18B: Screen-Space Motion Blur
    // -----------------------------------------------------------------------
    bool CreateMotionBlurResources();
    void DestroyMotionBlurResources();
    void DrawMotionBlurPass();

    struct MotionBlurConstants
    {
        XMFLOAT4X4 invViewProj;   // 64 B — current frame inverse VP
        XMFLOAT4X4 prevViewProj;  // 64 B — previous frame VP
        float      screenSizeX;   //  4 B
        float      screenSizeY;   //  4 B
        float      shutterScale;  //  4 B  (0=disabled, 1=full)
        int        numSamples;    //  4 B
        float      _pad[28];      // 112 B → 256 B total
    };
    static_assert(sizeof(MotionBlurConstants) == 256, "MotionBlurConstants must be 256B");

    // Full-res RGBA16F render target (output of motion blur pass)
    ComPtr<ID3D12Resource>       _motionBlurRT;
    D3D12MA::Allocation*         _motionBlurRTAlloc  = nullptr;
    UINT                         _motionBlurSRVIndex = UINT_MAX;

    // Dedicated 1-slot RTV heap for motion blur target
    ComPtr<ID3D12DescriptorHeap> _motionBlurRtvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE  _motionBlurRTV = {};

    // Pipeline
    std::unique_ptr<class DX12Pipeline> _motionBlurPipeline;

    // Per-frame constant buffers (256 B, UPLOAD heap, persistently mapped)
    ComPtr<ID3D12Resource>  _motionBlurCB[FRAMES_IN_FLIGHT];
    D3D12MA::Allocation*    _motionBlurCBAlloc[FRAMES_IN_FLIGHT]  = {};
    void*                   _motionBlurCBMapped[FRAMES_IN_FLIGHT] = {};

    // Previous-frame view-proj (for velocity computation)
    XMFLOAT4X4 _mbLastViewProj = {};

    // -----------------------------------------------------------------------
    // Phase 22: GPU Profiler
    // -----------------------------------------------------------------------
    DX12GPUProfiler _gpuProfiler;

    // -----------------------------------------------------------------------
    // Phase 29: Volumetric Fog
    // -----------------------------------------------------------------------
    bool CreateVolumetricFogResources();
    void DestroyVolumetricFogResources();
    void DispatchVolumetricFog();    // inject + scatter compute
    void DrawVolumetricFogApply();   // fullscreen additive blend into HDR

    static constexpr UINT FROXEL_X = 160u;
    static constexpr UINT FROXEL_Y = 90u;
    static constexpr UINT FROXEL_Z = 64u;

    struct VolumetricParamsCB  // 512 bytes
    {
        XMFLOAT4X4 invProj;                          // 64B
        XMFLOAT4X4 invView;                          // 64B
        XMFLOAT4X4 lightVP[4];                       // 256B
        XMFLOAT4   cascadeSplits;                    // 16B
        XMFLOAT3   lightDir;    float _p0;           // 16B
        XMFLOAT3   lightColor;  float lightIntensity; // 16B
        float nearZ;         float farZ;
        float screenW;       float screenH;           // 16B
        float fogDensity;    float fogHeightFalloff;
        float fogBaseHeight; float scatteringCoeff;   // 16B
        float extinctionCoeff; float phaseG;
        float _pad1[2];                              // 8B → 512B total
    };
    static_assert(sizeof(VolumetricParamsCB) == 480, "VolumetricParamsCB must be 480 bytes");

    bool _volFogReady   = false;
    bool _volFogEnabled = false;

    void SetVolumetricFogParams(const VolumetricFogParams& p) override {
        _volFogEnabled       = p.enabled;
        _volFogDensity       = p.density;
        _volFogHeightFalloff = p.heightFalloff;
        _volFogBaseHeight    = p.baseHeight;
        _volFogScattering    = p.scattering;
        _volFogExtinction    = p.extinction;
        _volFogPhaseG        = p.phaseG;
    }

    float _volFogDensity       = 0.05f;
    float _volFogHeightFalloff = 0.0f;
    float _volFogBaseHeight    = 0.0f;
    float _volFogScattering    = 0.8f;
    float _volFogExtinction    = 1.0f;
    float _volFogPhaseG        = 0.3f;

private:
    // 3D inject volume (RGBA16F, 160×90×64) — written by inject CS, read by scatter CS
    ComPtr<ID3D12Resource>       _volFogInject;
    D3D12MA::Allocation*         _volFogInjectAlloc    = nullptr;
    UINT                         _volFogInjectUAVIndex = UINT_MAX;
    UINT                         _volFogInjectSRVIndex = UINT_MAX;

    // 3D accumulation volume (RGBA16F, 160×90×64) — written by scatter CS, read by apply PS
    ComPtr<ID3D12Resource>       _volFogAccum;
    D3D12MA::Allocation*         _volFogAccumAlloc    = nullptr;
    UINT                         _volFogAccumUAVIndex = UINT_MAX;
    UINT                         _volFogAccumSRVIndex = UINT_MAX;

    // Non-shader-visible heap for UAV clear on 3D textures
    ComPtr<ID3D12DescriptorHeap> _volFogNonVisUAVHeap;

    // Per-frame params constant buffer (512 bytes, persistently mapped)
    ComPtr<ID3D12Resource>  _volFogParamsCB[FRAMES_IN_FLIGHT];
    D3D12MA::Allocation*    _volFogParamsCBAlloc[FRAMES_IN_FLIGHT]  = {};
    void*                   _volFogParamsCBMapped[FRAMES_IN_FLIGHT] = {};

    // Pipelines
    std::unique_ptr<class DX12Pipeline> _volInjectPipeline;
    std::unique_ptr<class DX12Pipeline> _volScatterPipeline;
    std::unique_ptr<class DX12Pipeline> _volApplyPipeline;

    // -----------------------------------------------------------------------
    // Phase 24: Clustered Lighting
    // -----------------------------------------------------------------------
public:
    void SetPointLights(const std::vector<PointLightDesc>& lights) override;

private:
    bool CreateClusteredLightingResources();
    void DestroyClusteredLightingResources();
    void DispatchClusterAssign();  // compute dispatch before deferred lighting

    static constexpr UINT CLUSTER_X = 16u;
    static constexpr UINT CLUSTER_Y = 9u;
    static constexpr UINT CLUSTER_Z = 24u;
    static constexpr UINT MAX_LIGHTS_PER_CLUSTER = 128u;
    static constexpr UINT MAX_POINT_LIGHTS = 1024u;
    static constexpr UINT TOTAL_CLUSTERS = CLUSTER_X * CLUSTER_Y * CLUSTER_Z;  // 3456

    bool _clusteredLightingReady = false;
    std::vector<PointLightDesc> _pointLights;

    // GPU data structures (32B per light, std430-compatible)
    struct DX12GPUPointLight
    {
        float position[3]; float radius;
        float color[3];    float intensity;
    };

    struct ClusterParamsData
    {
        XMFLOAT4X4 invProj;
        float nearZ;
        float farZ;
        float screenW;
        float screenH;
        uint32_t numLights;
        uint32_t _pad[3];
    };

    // Light buffer — host-visible (upload per frame)
    ComPtr<ID3D12Resource>  _clusterLightBuffer;
    D3D12MA::Allocation*    _clusterLightBufferAlloc = nullptr;
    void*                   _clusterLightBufferMapped = nullptr;

    // ClusterParams constant buffer
    ComPtr<ID3D12Resource>  _clusterParamsCB;
    D3D12MA::Allocation*    _clusterParamsCBAlloc = nullptr;
    void*                   _clusterParamsCBMapped = nullptr;

    // Cluster counts buffer — UAV/SRV (written by compute, read by fragment)
    ComPtr<ID3D12Resource>  _clusterCountsBuffer;
    D3D12MA::Allocation*    _clusterCountsBufferAlloc = nullptr;
    UINT                    _clusterCountsSRVIndex = UINT_MAX;
    UINT                    _clusterCountsUAVIndex = UINT_MAX;

    // Cluster indices buffer — UAV/SRV (~1.7 MB)
    ComPtr<ID3D12Resource>  _clusterIndicesBuffer;
    D3D12MA::Allocation*    _clusterIndicesBufferAlloc = nullptr;
    UINT                    _clusterIndicesSRVIndex = UINT_MAX;
    UINT                    _clusterIndicesUAVIndex = UINT_MAX;

    // Light buffer SRV
    UINT                    _clusterLightSRVIndex = UINT_MAX;

    // Cluster assign compute pipeline
    std::unique_ptr<class DX12Pipeline> _clusterAssignPipeline;

    // -----------------------------------------------------------------------
    // Phase 30: Global Illumination (SSGI + Irradiance Probes)
    // -----------------------------------------------------------------------
public:
    void SetGIParams(const GIParams& p) override;

private:
    bool CreateSSGIResources();
    void DestroySSGIResources();
    void DispatchSSGI();
    void DispatchProbeUpdate();

    static constexpr UINT PROBE_GRID_X   = 8u;
    static constexpr UINT PROBE_GRID_Y   = 4u;
    static constexpr UINT PROBE_GRID_Z   = 8u;
    static constexpr UINT PROBE_COUNT    = 256u;   // 8*4*8
    static constexpr UINT PROBE_TEX_SIZE = 16u;
    static constexpr UINT PROBE_ATLAS_W  = 128u;  // 8 probes * 16
    static constexpr UINT PROBE_ATLAS_H  = 64u;   // 4 probes * 16

    bool     _ssgiReady      = false;
    uint32_t _ssgiFrameCount = 0;
    uint32_t _ssgiProbeIdx   = 0;
    int      _ssgiPingPong   = 0;  // 0=write, 1=history

    // Half-res RGBA16F ping-pong (2 textures)
    ComPtr<ID3D12Resource>  _ssgiRT[2];
    D3D12MA::Allocation*    _ssgiRTAlloc[2]    = {};
    UINT _ssgiUAVIndex[2] = {UINT_MAX, UINT_MAX};
    UINT _ssgiSRVIndex[2] = {UINT_MAX, UINT_MAX};

    // Probe atlas: Texture2DArray 128x64, 8 slices, RGBA16F
    ComPtr<ID3D12Resource>  _probeIrrArray;
    D3D12MA::Allocation*    _probeIrrAlloc    = nullptr;
    UINT _probeIrrUAVIndex = UINT_MAX;
    UINT _probeIrrSRVIndex = UINT_MAX;

    // Non-shader-visible heap for ClearUAV (3 slots: ssgi[0], ssgi[1], probeIrr)
    ComPtr<ID3D12DescriptorHeap> _ssgiNonVisUAVHeap;

    struct SSGIConstants {
        float invViewProj[16];   // 64B
        float prevViewProj[16];  // 64B
        float view[16];          // 64B
        float screenSize[2];     //  8B
        float halfResSize[2];    //  8B
        uint32_t frameCount;     //  4B
        uint32_t numRays;        //  4B
        float maxRayDist;        //  4B
        float temporalAlpha;     //  4B
        float _pad[8];           // 32B -> 256B
    };
    static_assert(sizeof(SSGIConstants) == 256, "SSGIConstants must be 256B");

    struct ProbeConstants {
        float    origin[3];   float _p0;        // 16B
        float    spacing[3];  float _p1;        // 16B
        uint32_t dims[3];     uint32_t _p2;     // 16B
        float    screenSize[2]; float _p3[2];   // 16B
        float    invViewProj[16];               // 64B
        uint32_t probeIndex;  uint32_t _pp[3];  // 16B
        float    _pad[28];                      // 112B -> 256B total
    };
    static_assert(sizeof(ProbeConstants) == 256, "ProbeConstants must be 256B");

    ComPtr<ID3D12Resource>  _ssgiCB[FRAMES_IN_FLIGHT];
    D3D12MA::Allocation*    _ssgiCBAlloc[FRAMES_IN_FLIGHT]  = {};
    void*                   _ssgiCBMapped[FRAMES_IN_FLIGHT] = {};

    ComPtr<ID3D12Resource>  _probeCB[FRAMES_IN_FLIGHT];
    D3D12MA::Allocation*    _probeCBAlloc[FRAMES_IN_FLIGHT]  = {};
    void*                   _probeCBMapped[FRAMES_IN_FLIGHT] = {};

    std::unique_ptr<class DX12Pipeline> _ssgiComputePipeline;
    std::unique_ptr<class DX12Pipeline> _probeUpdatePipeline;
    std::unique_ptr<class DX12Pipeline> _lightingPipelineGI;

    XMFLOAT3 _probeGridOrigin  = {-8.0f, 0.0f, -8.0f};
    XMFLOAT3 _probeGridSpacing = { 2.0f, 2.0f,  2.0f};

    float _giTemporalAlpha = 0.1f;
    int   _giNumRays       = 8;
    float _giMaxRayDist    = 5.0f;

    // -----------------------------------------------------------------------
    // Phase 31: Order-Independent Transparency (WBOIT)
    // -----------------------------------------------------------------------
private:
    bool CreateOITResources();
    void DestroyOITResources();
    void DrawOITForwardPass();
    void DrawOITCompositePass();

    static constexpr UINT MAX_OIT_MESHES = 256;

    struct OITMeshDraw
    {
        const Mesh* mesh;
        XMFLOAT4X4  model;
        float        alpha;
    };

    bool                     _oitReady = false;
    std::vector<OITMeshDraw> _oitMeshes;  // filled by DrawMesh, consumed by DrawOITForwardPass

    // Accum RT (RGBA16F, full-res) — additive blend ONE+ONE
    ComPtr<ID3D12Resource>       _oitAccumRT;
    D3D12MA::Allocation*         _oitAccumRTAlloc   = nullptr;
    UINT                         _oitAccumSRVIndex  = UINT_MAX;

    // Revealage RT (R8_UNORM, full-res) — multiplicative blend ZERO+SRC_COLOR
    ComPtr<ID3D12Resource>       _oitRevealRT;
    D3D12MA::Allocation*         _oitRevealRTAlloc  = nullptr;
    UINT                         _oitRevealSRVIndex = UINT_MAX;

    // 2-slot RTV heap dedicated to OIT targets
    ComPtr<ID3D12DescriptorHeap> _oitRtvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE  _oitAccumRTV  = {};
    D3D12_CPU_DESCRIPTOR_HANDLE  _oitRevealRTV = {};

    // Per-frame alpha CB pool: MAX_OIT_MESHES slots × 256B (one slot per transparent draw)
    ComPtr<ID3D12Resource>  _oitAlphaCBPool[FRAMES_IN_FLIGHT];
    D3D12MA::Allocation*    _oitAlphaCBPoolAlloc[FRAMES_IN_FLIGHT]  = {};
    void*                   _oitAlphaCBPoolMapped[FRAMES_IN_FLIGHT] = {};

    std::unique_ptr<class DX12Pipeline> _oitForwardPipeline;
    std::unique_ptr<class DX12Pipeline> _oitCompositePipeline;

    // ── Phase 32: Visibility Buffer ───────────────────────────────────────────────
    bool _visBufferReady = false;
    bool _visBufferMode  = false;  // toggle: true = visibility path, false = G-buffer path

    // Visibility RT (R32_UINT, full-res)
    ComPtr<ID3D12Resource>       _visRT;
    D3D12MA::Allocation*         _visRTAlloc    = nullptr;
    UINT                         _visRTSRVIndex = UINT_MAX;  // SRV for shade compute
    ComPtr<ID3D12DescriptorHeap> _visRtvHeap;                // non-shader-visible RTV heap (1 slot)
    D3D12_CPU_DESCRIPTOR_HANDLE  _visRTV = {};

    // Non-shader-visible UAV heap for ClearUnorderedAccessViewUint on vis RT
    ComPtr<ID3D12DescriptorHeap> _visNonVisUAVHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE  _visNonVisUAV = {};

    // G-buffer UAVs for shade compute output (reuse existing G-buffer resources as UAV)
    UINT _visGB0UAVIndex = UINT_MAX;
    UINT _visGB1UAVIndex = UINT_MAX;
    UINT _visGB2UAVIndex = UINT_MAX;

    // Merged VB/IB SRV slots for shade compute
    UINT _visMergedVBSRVIndex = UINT_MAX;
    UINT _visMergedIBSRVIndex = UINT_MAX;

    // Per-frame shade constants CB (VisShadeConstants: viewProj + screenSize)
    ComPtr<ID3D12Resource>  _visShadeCB[FRAMES_IN_FLIGHT];
    D3D12MA::Allocation*    _visShadeCBAlloc[FRAMES_IN_FLIGHT]  = {};
    void*                   _visShadeCBMapped[FRAMES_IN_FLIGHT] = {};

    std::unique_ptr<class DX12Pipeline> _visibilityPipeline;   // VS+PS: geometry → vis RT
    std::unique_ptr<class DX12Pipeline> _visShadePipeline;     // CS: vis RT → G-buffer UAVs
    ComPtr<ID3D12CommandSignature>       _visibilityCmdSignature; // per-draw overrides using VisibilityBuffer root sig

    bool CreateVisibilityResources();
    void DestroyVisibilityResources();
    void DrawVisibilityPass();       // replaces G-buffer fill in vis mode
    void DispatchVisibilityShade();  // replaces G-buffer as SRVs; outputs same G-buffer slots

    // ── S2: Camera Sensor Offscreen Rendering ────────────────────────────────────
    // Per-camera GPU resources (G-buffer, lighting RT, distortion RT, readback, cull buffers)
    struct DX12CameraResources
    {
        // G-buffer + depth (camera viewpoint)
        ComPtr<ID3D12Resource>       gbuffer[3];
        D3D12MA::Allocation*         gbAlloc[3]  = {};
        ComPtr<ID3D12Resource>       depth;
        D3D12MA::Allocation*         depthAlloc  = nullptr;
        // Lit HDR output (IBL-only deferred)
        ComPtr<ID3D12Resource>       litRT;
        D3D12MA::Allocation*         litAlloc    = nullptr;
        // Distorted + noise + sRGB output
        ComPtr<ID3D12Resource>       distortRT;
        D3D12MA::Allocation*         distortAlloc = nullptr;
        // CPU readback buffers
        ComPtr<ID3D12Resource>       readbackRGB;
        ComPtr<ID3D12Resource>       readbackDepth;
        // Descriptor heaps
        ComPtr<ID3D12DescriptorHeap> rtvHeap;  // 4 slots: GB0,1,2 + litRT
        ComPtr<ID3D12DescriptorHeap> dsvHeap;  // 1 slot: depth
        // SRV/UAV indices in _imGuiSrvHeap
        UINT gbSRVIndex[3]   = {UINT_MAX, UINT_MAX, UINT_MAX};
        UINT depthSRVIndex   = UINT_MAX;
        UINT litSRVIndex     = UINT_MAX;
        UINT distortSRVIndex = UINT_MAX;  // GPU handle for ImGui::Image()
        UINT distortUAVIndex = UINT_MAX;
        // Per-frame distortion constants CBV (double-buffered)
        ComPtr<ID3D12Resource>       distortCB[2];
        D3D12MA::Allocation*         distortCBAlloc[2] = {};
        void*                        distortCBMapped[2] = {};
        // GPU frustum cull resources
        ComPtr<ID3D12Resource>       sensorArgBuffer;   // indirect draw args
        D3D12MA::Allocation*         sensorArgAlloc    = nullptr;
        ComPtr<ID3D12Resource>       sensorDrawCount;   // UINT draw count
        D3D12MA::Allocation*         sensorDrawAlloc   = nullptr;
        ComPtr<ID3D12Resource>       sensorCullCB[2];  // CullConstants CBV
        D3D12MA::Allocation*         sensorCullCBAlloc[2] = {};
        void*                        sensorCullCBMapped[2] = {};
        // Tracking
        uint32_t width       = 0;
        uint32_t height      = 0;
        bool     ready       = false;
        bool     firstRender = true;  // true until first RenderCameraSensorInternal completes
    };

    std::unordered_map<class CameraSensor*, DX12CameraResources> _cameraRTs;

    std::unique_ptr<class DX12Pipeline> _sensorLightingPipeline;
    std::unique_ptr<class DX12Pipeline> _cameraDistortPipeline;

    bool InitCameraResources(class CameraSensor* cam);
    void DestroyCameraResources(class CameraSensor* cam);
    void RenderCameraSensorInternal(class CameraSensor* cam);
    void RenderCameraSensors() override;

    // ── S3: LiDAR Sensor GPU Raycasting ──────────────────────────────────────
    struct DX12LiDARResources
    {
        // Sensor-space ray directions (uploaded once at init)
        ComPtr<ID3D12Resource>   rayDirBuffer;
        D3D12MA::Allocation*     rayDirAlloc  = nullptr;
        // Per-frame sensor CB (world matrix + params)
        ComPtr<ID3D12Resource>   sensorCB[2];
        D3D12MA::Allocation*     sensorCBAlloc[2] = {};
        void*                    sensorCBMapped[2] = {};
        // GPU output (UAV)
        ComPtr<ID3D12Resource>   outputBuffer;
        D3D12MA::Allocation*     outputAlloc  = nullptr;
        // CPU readback
        ComPtr<ID3D12Resource>   readbackBuffer;
        // Point cloud viewport VB (UPLOAD, persistently mapped, updated from pointCloud each frame)
        ComPtr<ID3D12Resource>   pointCloudVB;
        void*                    pointCloudVBMapped = nullptr;
        // Tracking
        uint32_t rayCount   = 0;
        bool     ready      = false;
        bool     firstRender = true;
    };

    std::unordered_map<class LiDARSensor*, DX12LiDARResources> _lidarRTs;
    std::unique_ptr<class DX12Pipeline> _lidarRaycastPipeline;
    std::unique_ptr<class DX12Pipeline> _pointCloudPipeline;

    bool InitLiDARResources(class LiDARSensor* sensor);
    void DestroyLiDARResources(class LiDARSensor* sensor);
    void RenderLiDARSensorInternal(class LiDARSensor* sensor);
    void RenderLiDARSensors() override;
    void RenderLiDARPointClouds() override;
};
} // namespace Luna
