#pragma once

#include <LunaEngine/LunaPCH.h>
#include <LunaEngine/Renderer/HAL/Public/IRenderBackend.h>

namespace Luna
{
// Forward declarations — only types actually used in the public interface
struct Mesh;        // returned by LoadMeshes; also in IRenderBackend.h (redundant but explicit)
class  VulkanDevice;

class VulkanBackend : public IRenderBackend
{
  public:
    VulkanBackend();
    virtual ~VulkanBackend() override;

    bool Init(void* windowHandler, uint32_t width, uint32_t height) override;
    void Shutdown() override;

    void BeginFrame()     override;
    void DrawFrame()      override;
    void CompositeFrame() override;     // deferred: end G-buffer pass → lighting pass → begin ImGui pass
    void EndFrame()       override;

    void InitImGui(void* windowHandler) override;
    void StartImGui()    override;
    void RenderImGui()   override;
    void ShutdownImGui() override;
    void Resize(uint32_t width, uint32_t height) override;

    void SetVSync(bool vsync) override { _vsync = vsync; }

    void Draw(uint32_t vertexCount) override;
    void SetVertexBuffer(class IBuffer* buffer) override;
    void BindPipeline(class IPipeline* pipeline) override;

    // IRenderBackend non-pure virtual overrides — signatures must match exactly
    void UpdateMVP(const XMFLOAT4X4& model, const XMFLOAT4X4& view,
                   const XMFLOAT4X4& proj) override;
    void DrawMesh(const Mesh* mesh, const XMFLOAT4X4& model) override;

    // Load all primitives from a glTF/GLB file
    std::vector<std::shared_ptr<Mesh>> LoadMeshes(const std::string& path) override;

    // Debug: procedural 2×2m quad (bypasses glTF, uses default white material)
    std::vector<std::shared_ptr<Mesh>> LoadDebugQuad() override;

    // Phase 15B: flush accumulated DrawMesh() calls via GPU-driven indirect rendering
    void FlushDraws() override;

    // Phase 15C: load equirectangular HDR + run IBL precompute on GPU
    bool LoadHDREnvironment(const std::string& hdrPath) override;

    const char* GetBackendName() const override { return "Vulkan"; }

  private:
    // ---------------------------------------------------------------------------
    // Init helpers
    // ---------------------------------------------------------------------------
    bool CreateInstance();
    bool SetupDebugMessenger();
    bool CreateSurface(void* windowHandle);
    bool CreateImGuiDescriptorPool();
    bool CreateDepthResources();
    void DestroyDepthResources();

    // G-buffer resources (created/destroyed alongside swapchain)
    bool CreateGBufferResources();
    void DestroyGBufferResources();
    void UpdateDeferredGbufDescriptors();  // called after G-buffer image (re)creation

    // Deferred lighting pipeline
    bool CreateDeferredPipeline();
    void DestroyDeferredPipeline();

    // CSM (Cascaded Shadow Maps)
    bool CreateCSMResources();
    void DestroyCSMResources();
    void UpdateCSMMatrices(const XMFLOAT4X4& view, const XMFLOAT4X4& proj);
    void DrawCSMPass(VkCommandBuffer cmd);

    // SSAO
    bool CreateSSAOResources();
    void DestroySSAOResources();
    void DrawSSAOPass(VkCommandBuffer cmd);
    void DrawSSAOBlurPass(VkCommandBuffer cmd);

    // Swapchain lifecycle
    bool CreateSwapchain(uint32_t width, uint32_t height);
    void DestroySwapchain();
    void RecreateSwapchain();

    bool CreateRenderPass();
    bool CreateFramebuffers();
    bool CreateFrameResources();

    // Pipeline & descriptors
    bool CreatePipeline();
    void DestroyPipeline();
    bool CreateSceneDescriptorPool();

    // Buffer / image helpers
    bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags props,
                      VkBuffer& outBuf, VkDeviceMemory& outMem);
    bool CopyBuffer(VkBuffer src, VkBuffer dst, VkDeviceSize size);
    bool CreateImage(uint32_t w, uint32_t h, VkFormat fmt,
                     VkImageTiling tiling, VkImageUsageFlags usage,
                     VkMemoryPropertyFlags props,
                     VkImage& outImg, VkDeviceMemory& outMem);
    bool TransitionImageLayout(VkImage img, VkImageLayout oldLayout, VkImageLayout newLayout);
    bool CopyBufferToImage(VkBuffer buf, VkImage img, uint32_t w, uint32_t h);
    VkImageView CreateImageView(VkImage img, VkFormat fmt, VkImageAspectFlags aspect);
    uint32_t FindMemoryType(uint32_t filter, VkMemoryPropertyFlags props);

    VkCommandBuffer BeginSingleTimeCommands();
    void            EndSingleTimeCommands(VkCommandBuffer cmd);

    // ---------------------------------------------------------------------------
    // Core Vulkan objects
    // ---------------------------------------------------------------------------
    VkInstance   _instance = VK_NULL_HANDLE;
    VkSurfaceKHR _surface  = VK_NULL_HANDLE;

    std::unique_ptr<VulkanDevice> _device;

    VkDebugUtilsMessengerEXT _debugMessenger = VK_NULL_HANDLE;

    // ---------------------------------------------------------------------------
    // Swapchain
    // ---------------------------------------------------------------------------
    VkSwapchainKHR            _swapchain       = VK_NULL_HANDLE;
    VkFormat                  _swapchainFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D                _swapchainExtent = {};
    std::vector<VkImage>      _swapchainImages;
    std::vector<VkImageView>  _swapchainImageViews;
    std::vector<VkFence>      _imagesInFlight;  // Per swapchain-image fence tracking

    // ---------------------------------------------------------------------------
    // Depth buffer
    // ---------------------------------------------------------------------------
    VkImage        _depthImage  = VK_NULL_HANDLE;
    VkDeviceMemory _depthMemory = VK_NULL_HANDLE;
    VkImageView    _depthView   = VK_NULL_HANDLE;

    // ---------------------------------------------------------------------------
    // Render pass + framebuffers
    // ---------------------------------------------------------------------------
    VkRenderPass               _renderPass = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> _framebuffers;

    // ---------------------------------------------------------------------------
    // Per-frame resources (ring buffer, N = FRAMES_IN_FLIGHT)
    // Must be >= swapchain image count to avoid semaphore reuse issues
    // ---------------------------------------------------------------------------
    static constexpr uint32_t FRAMES_IN_FLIGHT = 3;

    struct VkFrameResource
    {
        VkCommandPool   cmdPool    = VK_NULL_HANDLE;
        VkCommandBuffer cmdBuffer  = VK_NULL_HANDLE;
        VkFence         fence      = VK_NULL_HANDLE;
        VkSemaphore     imageReady = VK_NULL_HANDLE;
        VkSemaphore     renderDone = VK_NULL_HANDLE;

        // Per-frame MVP uniform buffer (host-visible, persistently mapped)
        // Layout: float4x4 model[MAX_DRAWS] + view + proj (dynamic offset per draw)
        // Bug #6 fix: allocate MAX_DRAWS model matrix slots so each draw gets its own MVP
        static constexpr uint32_t MAX_DRAWS = 64;
        VkBuffer        mvpBuffer  = VK_NULL_HANDLE;
        VkDeviceMemory  mvpMemory  = VK_NULL_HANDLE;
        void*           mvpMapped  = nullptr;
        VkDescriptorSet mvpDescSet = VK_NULL_HANDLE;

        // Phase 5C: Per-frame scene UBO (eyePosition + directional light) — set=0, binding=1
        VkBuffer       sceneBuffer = VK_NULL_HANDLE;
        VkDeviceMemory sceneMemory = VK_NULL_HANDLE;
        void*          sceneMapped = nullptr;
    };
    VkFrameResource _frames[FRAMES_IN_FLIGHT];

    uint32_t _frameIndex    = 0;
    uint32_t _imageIndex    = 0;
    bool     _frameActive   = false;
    uint32_t _drawCallIndex = 0;   // incremented per DrawMesh call within a frame

    // ---------------------------------------------------------------------------
    // Pipeline & descriptors
    // ---------------------------------------------------------------------------
    VkDescriptorSetLayout _mvpDescLayout      = VK_NULL_HANDLE;  // set=0: MVP + Scene UBOs
    VkDescriptorSetLayout _materialDescLayout = VK_NULL_HANDLE;  // set=1: material
    VkPipelineLayout      _pipelineLayout     = VK_NULL_HANDLE;
    VkPipeline            _graphicsPipeline   = VK_NULL_HANDLE;  // legacy forward (unused in deferred path)
    VkPipeline            _gbPipeline         = VK_NULL_HANDLE;  // G-buffer geometry fill
    VkDescriptorPool      _sceneDescPool      = VK_NULL_HANDLE;

    // Sampler shared across all material textures
    VkSampler _linearSampler = VK_NULL_HANDLE;

    // ---------------------------------------------------------------------------
    // G-buffer images (full-res, recreated on resize)
    // ---------------------------------------------------------------------------
    VkImage        _gbAlbedoImage     = VK_NULL_HANDLE;   // RGBA8_UNORM
    VkDeviceMemory _gbAlbedoMemory    = VK_NULL_HANDLE;
    VkImageView    _gbAlbedoView      = VK_NULL_HANDLE;

    VkImage        _gbNormalImage     = VK_NULL_HANDLE;   // RGBA16F
    VkDeviceMemory _gbNormalMemory    = VK_NULL_HANDLE;
    VkImageView    _gbNormalView      = VK_NULL_HANDLE;

    VkImage        _gbMetalRoughImage = VK_NULL_HANDLE;   // RGBA8_UNORM
    VkDeviceMemory _gbMetalRoughMemory= VK_NULL_HANDLE;
    VkImageView    _gbMetalRoughView  = VK_NULL_HANDLE;

    VkRenderPass   _gbRenderPass      = VK_NULL_HANDLE;   // 3 colour + depth G-buffer render pass
    VkFramebuffer  _gbFramebuffer     = VK_NULL_HANDLE;

    // ---------------------------------------------------------------------------
    // Deferred lighting pipeline
    // ---------------------------------------------------------------------------
    struct DeferredSceneUBO                    // 464 bytes, allocated as 512 B
    {
        float    invViewProj[16];              //  64 B
        float    eyePosition[3]; float _p0;    //  16 B
        float    lightDir[3];    float lightIntensity;  //  16 B
        float    lightColor[3];  float _p1;    //  16 B
        float    viewMatrix[16];               //  64 B — camera view matrix
        float    lightVP[4][16];               // 256 B — per-cascade light VP
        float    cascadeSplits[4];             //  16 B
        uint32_t rtEnabled;    uint32_t _p2[3]; //  16 B — Phase 18D RT shadow flag
    };

    VkDescriptorSetLayout _deferredSceneLayout = VK_NULL_HANDLE; // set=0: scene UBO
    VkDescriptorSetLayout _deferredGbufLayout  = VK_NULL_HANDLE; // set=1: 4 textures + sampler
    VkPipelineLayout      _deferredPipeLayout  = VK_NULL_HANDLE;
    VkPipeline            _deferredPipeline    = VK_NULL_HANDLE;
    VkDescriptorPool      _deferredDescPool    = VK_NULL_HANDLE;
    VkSampler             _pointClampSampler   = VK_NULL_HANDLE;

    // Per-frame deferred scene constant buffers + descriptor sets
    VkBuffer        _deferredSceneCB[FRAMES_IN_FLIGHT]       = {};
    VkDeviceMemory  _deferredSceneCBMem[FRAMES_IN_FLIGHT]    = {};
    void*           _deferredSceneCBMapped[FRAMES_IN_FLIGHT] = {};
    VkDescriptorSet _deferredSceneDescSet[FRAMES_IN_FLIGHT]  = {};

    // Static G-buffer descriptor set (updated on resize)
    VkDescriptorSet _deferredGbufDescSet = VK_NULL_HANDLE;

    // Cached view/proj matrices for deferred scene UBO (set in UpdateMVP, used in CompositeFrame)
    XMFLOAT4X4 _deferredView{};
    XMFLOAT4X4 _deferredProj{};

    // ---------------------------------------------------------------------------
    // CSM (Cascaded Shadow Maps) — 4 cascades, 2048×2048 each
    // ---------------------------------------------------------------------------
    static constexpr uint32_t CSM_CASCADE_COUNT = 4;
    static constexpr uint32_t CSM_SHADOW_SIZE   = 2048;

    VkImage        _csmImage       = VK_NULL_HANDLE;   // D32_SFLOAT, 2048², 4 layers
    VkDeviceMemory _csmMemory      = VK_NULL_HANDLE;
    VkImageView    _csmLayerView[4]= {};               // per-layer for framebuffers
    VkImageView    _csmArrayView   = VK_NULL_HANDLE;   // full array for deferred sampling
    VkRenderPass   _csmRenderPass  = VK_NULL_HANDLE;   // depth-only
    VkFramebuffer  _csmFramebuffers[4] = {};
    VkPipelineLayout _csmPipelineLayout = VK_NULL_HANDLE; // push constant (64B lightMVP)
    VkPipeline     _csmPipeline    = VK_NULL_HANDLE;
    VkSampler      _csmSampler     = VK_NULL_HANDLE;   // point-clamp for shadow reads

    XMFLOAT4X4 _csmLightVP[4]     = {};
    float      _csmSplits[4]      = {};
    std::vector<XMFLOAT4X4> _lastMeshModels;            // cached from DrawMesh, used next frame

    // ---------------------------------------------------------------------------
    // SSAO — half-res R8_UNORM (raw + blurred)
    // ---------------------------------------------------------------------------
    static constexpr uint32_t SSAO_SAMPLE_COUNT = 16;
    static constexpr uint32_t NOISE_SIZE        = 4;

    struct SSAOConstants  // 464 bytes, alloc 512
    {
        XMFLOAT4   samples[SSAO_SAMPLE_COUNT]; // 256
        XMFLOAT4X4 projection;                 //  64
        XMFLOAT4X4 invProjection;              //  64
        XMFLOAT4X4 view;                       //  64
        XMFLOAT2   noiseScale;                 //   8
        float      radius;                     //   4
        float      bias;                       //   4
    };
    SSAOConstants _ssaoKernel{};

    // Images
    VkImage        _ssaoRTImage        = VK_NULL_HANDLE;  // raw SSAO (R8_UNORM, half-res)
    VkDeviceMemory _ssaoRTMemory       = VK_NULL_HANDLE;
    VkImageView    _ssaoRTView         = VK_NULL_HANDLE;

    VkImage        _ssaoBlurImage      = VK_NULL_HANDLE;  // blurred SSAO
    VkDeviceMemory _ssaoBlurMemory     = VK_NULL_HANDLE;
    VkImageView    _ssaoBlurView       = VK_NULL_HANDLE;

    VkImage        _ssaoNoiseImage     = VK_NULL_HANDLE;  // 4×4 R8G8_UNORM noise
    VkDeviceMemory _ssaoNoiseMemory    = VK_NULL_HANDLE;
    VkImageView    _ssaoNoiseView      = VK_NULL_HANDLE;

    // Render passes / framebuffers (half-res)
    VkRenderPass   _ssaoRenderPass     = VK_NULL_HANDLE;  // R8_UNORM single colour
    VkFramebuffer  _ssaoFramebuffer    = VK_NULL_HANDLE;
    VkFramebuffer  _ssaoBlurFramebuffer= VK_NULL_HANDLE;

    // Pipelines
    VkDescriptorSetLayout _ssaoSceneLayout   = VK_NULL_HANDLE; // set=0: UBO
    VkDescriptorSetLayout _ssaoTexLayout     = VK_NULL_HANDLE; // set=1: depth+normal+noise+samplers
    VkPipelineLayout      _ssaoPipeLayout    = VK_NULL_HANDLE;
    VkPipeline            _ssaoPipeline      = VK_NULL_HANDLE;

    VkDescriptorSetLayout _ssaoBlurLayout    = VK_NULL_HANDLE; // set=0: raw ssao + sampler
    VkPipelineLayout      _ssaoBlurPipeLayout= VK_NULL_HANDLE;
    VkPipeline            _ssaoBlurPipeline  = VK_NULL_HANDLE;

    // Descriptor pool + sets
    VkDescriptorPool _ssaoDescPool     = VK_NULL_HANDLE;
    VkDescriptorSet  _ssaoSceneDescSet[FRAMES_IN_FLIGHT] = {};
    VkDescriptorSet  _ssaoTexDescSet   = VK_NULL_HANDLE;
    VkDescriptorSet  _ssaoBlurDescSet  = VK_NULL_HANDLE;

    // Per-frame UBO
    VkBuffer       _ssaoCB[FRAMES_IN_FLIGHT]       = {};
    VkDeviceMemory _ssaoCBMem[FRAMES_IN_FLIGHT]    = {};
    void*          _ssaoCBMapped[FRAMES_IN_FLIGHT]  = {};

    // Samplers
    VkSampler _ssaoPointWrap    = VK_NULL_HANDLE;
    VkSampler _ssaoBilinearClamp= VK_NULL_HANDLE;

    // ---------------------------------------------------------------------------
    // Phase 15B: GPU-driven indirect rendering
    // ---------------------------------------------------------------------------
    static constexpr uint32_t MAX_GPU_OBJECTS = 1024;

    bool _gpuDrivenReady = false;

    struct GPUObjectDataVK
    {
        XMFLOAT4X4 model;           // 64 B
        XMFLOAT4   boundingSphere;  // 16 B
        uint32_t   meshIndex;       //  4 B
        uint32_t   materialIndex;   //  4 B
        uint64_t   _unused;         //  8 B (materialCBAddr placeholder)
    };

    struct MaterialFactorsVK
    {
        float albedoR, albedoG, albedoB, albedoA;
        float metallicFactor;
        float roughnessFactor;
        float _pad[2];
    };

    // CPU-side accumulation buffer (cleared in FlushDraws)
    std::vector<GPUObjectDataVK> _cpuInstances;

    // Merged geometry
    VkBuffer       _mergedVB    = VK_NULL_HANDLE; VkDeviceMemory _mergedVBMem = VK_NULL_HANDLE;
    VkBuffer       _mergedIB    = VK_NULL_HANDLE; VkDeviceMemory _mergedIBMem = VK_NULL_HANDLE;
    VkBuffer       _meshInfoBuf = VK_NULL_HANDLE; VkDeviceMemory _meshInfoMem = VK_NULL_HANDLE;

    // Object data SSBO (HOST_VISIBLE|COHERENT, persistently mapped)
    VkBuffer       _objectDataBuffer = VK_NULL_HANDLE;
    VkDeviceMemory _objectDataMem    = VK_NULL_HANDLE;
    void*          _objectDataMapped = nullptr;

    // Per-frame indirect args + draw count (DEVICE_LOCAL)
    VkBuffer       _indirectArgBuffer[FRAMES_IN_FLIGHT] = {};
    VkDeviceMemory _indirectArgMem[FRAMES_IN_FLIGHT]    = {};
    VkBuffer       _drawCountBuffer[FRAMES_IN_FLIGHT]   = {};
    VkDeviceMemory _drawCountMem[FRAMES_IN_FLIGHT]      = {};

    // Material factor SSBO (scalar albedo/metallic/roughness, uploaded once)
    VkBuffer       _matFactorBuffer = VK_NULL_HANDLE;
    VkDeviceMemory _matFactorMem    = VK_NULL_HANDLE;

    // Bindless descriptor set (set=1: material SSBO + 3 texture arrays + sampler)
    VkDescriptorSetLayout _indirectMaterialLayout = VK_NULL_HANDLE;
    VkDescriptorPool      _indirectDescPool       = VK_NULL_HANDLE;
    VkDescriptorSet       _indirectMaterialSet    = VK_NULL_HANDLE;

    // Indirect G-buffer pipeline (set=0: ViewProj UBO + ObjectData SSBO; set=1: bindless)
    VkDescriptorSetLayout _indirectVSLayout     = VK_NULL_HANDLE;
    VkDescriptorPool      _indirectVSDescPool   = VK_NULL_HANDLE;
    VkDescriptorSet       _indirectVSDescSet    = VK_NULL_HANDLE;
    VkBuffer              _indirectViewProjBuf  = VK_NULL_HANDLE;
    VkDeviceMemory        _indirectViewProjMem  = VK_NULL_HANDLE;
    void*                 _indirectViewProjMapped = nullptr;

    VkPipelineLayout _indirectPipeLayout   = VK_NULL_HANDLE;
    VkPipeline       _indirectGBufPipeline = VK_NULL_HANDLE;

    // Compute cull pipeline
    VkDescriptorSetLayout _vkCullDescLayout = VK_NULL_HANDLE;
    VkDescriptorPool      _vkCullDescPool   = VK_NULL_HANDLE;
    VkDescriptorSet       _vkCullDescSet[FRAMES_IN_FLIGHT] = {};
    VkPipelineLayout      _vkCullPipeLayout = VK_NULL_HANDLE;
    VkPipeline            _vkCullPipeline   = VK_NULL_HANDLE;

    // G-buffer render pass variant with LOAD_OP_LOAD (for FlushDraws re-open)
    VkRenderPass  _gbRenderPassLoad  = VK_NULL_HANDLE;
    VkFramebuffer _gbFramebufferLoad = VK_NULL_HANDLE;

    bool CreateIndirectResources();
    void DestroyIndirectResources();
    void BuildMergedGeometry(const std::vector<std::vector<struct PBRVertex>>& allVerts,
                              const std::vector<std::vector<uint32_t>>& allIdxs);

    // ---------------------------------------------------------------------------
    // Phase 15C: IBL environment lighting
    // ---------------------------------------------------------------------------
    bool _iblReady = false;

    static constexpr uint32_t VK_ENV_CUBE_SIZE      = 512;
    static constexpr uint32_t VK_IRR_CUBE_SIZE       = 32;
    static constexpr uint32_t VK_PREFILTER_CUBE_SIZE = 128;
    static constexpr uint32_t VK_PREFILTER_MIP_COUNT = 5;
    static constexpr uint32_t VK_BRDF_LUT_SIZE       = 512;

    VkImage        _vkEnvCubemap      = VK_NULL_HANDLE;
    VkDeviceMemory _vkEnvCubemapMem   = VK_NULL_HANDLE;
    VkImageView    _vkEnvCubemapView  = VK_NULL_HANDLE;  // CUBE view for sampling
    VkImageView    _vkEnvCubemapArray = VK_NULL_HANDLE;  // 2D_ARRAY view for compute writes

    VkImage        _vkIrrCubemap      = VK_NULL_HANDLE;
    VkDeviceMemory _vkIrrCubemapMem   = VK_NULL_HANDLE;
    VkImageView    _vkIrrCubemapView  = VK_NULL_HANDLE;
    VkImageView    _vkIrrCubemapArray = VK_NULL_HANDLE;

    VkImage        _vkPrefilterCubemap      = VK_NULL_HANDLE;
    VkDeviceMemory _vkPrefilterCubemapMem   = VK_NULL_HANDLE;
    VkImageView    _vkPrefilterCubemapView  = VK_NULL_HANDLE;  // CUBE view, all mips
    VkImageView    _vkPrefilterMipView[VK_PREFILTER_MIP_COUNT] = {};  // per-mip 2D_ARRAY

    VkImage        _vkBrdfLUT      = VK_NULL_HANDLE;
    VkDeviceMemory _vkBrdfLUTMem   = VK_NULL_HANDLE;
    VkImageView    _vkBrdfLUTView  = VK_NULL_HANDLE;

    VkSampler      _vkIBLSampler  = VK_NULL_HANDLE;   // trilinear clamp, maxLOD=5
    VkSampler      _vkBrdfSampler = VK_NULL_HANDLE;   // bilinear clamp

    // IBL compute pipelines (one per stage) + their DSLs (kept alive until DestroyIBLResources)
    VkDescriptorSetLayout _vkEquirectDSL     = VK_NULL_HANDLE;
    VkPipelineLayout      _vkEquirectPipeLayout  = VK_NULL_HANDLE;
    VkPipeline            _vkEquirectPipeline    = VK_NULL_HANDLE;
    VkDescriptorSetLayout _vkIrrConvDSL      = VK_NULL_HANDLE;
    VkPipelineLayout      _vkIrrConvPipeLayout   = VK_NULL_HANDLE;
    VkPipeline            _vkIrrConvPipeline     = VK_NULL_HANDLE;
    VkDescriptorSetLayout _vkPrefilterDSL    = VK_NULL_HANDLE;
    VkPipelineLayout      _vkPrefilterPipeLayout = VK_NULL_HANDLE;
    VkPipeline            _vkPrefilterPipeline   = VK_NULL_HANDLE;
    VkDescriptorSetLayout _vkBrdfLutDSL      = VK_NULL_HANDLE;
    VkPipelineLayout      _vkBrdfLutPipeLayout   = VK_NULL_HANDLE;
    VkPipeline            _vkBrdfLutPipeline     = VK_NULL_HANDLE;

    // IBL deferred lighting pipeline (replaces _deferredPipeline when IBL is ready)
    VkPipeline _deferredIBLPipeline = VK_NULL_HANDLE;

    bool CreateIBLResources();
    bool DispatchIBLPrecompute(VkImage equirectSrc, VkImageView equirectView);
    void DestroyIBLResources();

    // ---------------------------------------------------------------------------
    // Scene meshes
    // ---------------------------------------------------------------------------
    struct VkTexture
    {
        VkImage        image  = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView    view   = VK_NULL_HANDLE;
    };

    struct VkMaterial
    {
        VkTexture       albedo, normalMap, metalRough, emissive;
        VkBuffer        ubo       = VK_NULL_HANDLE;
        VkDeviceMemory  uboMem    = VK_NULL_HANDLE;
        void*           uboMapped = nullptr;
        VkDescriptorSet descSet   = VK_NULL_HANDLE;
        uint32_t        bindlessIndex = 0;  // Phase 15B: index into bindless texture arrays
        // Scalar PBR factors (for GPU-driven material SSBO)
        float albedoFactor[4]  = {1,1,1,1};
        float metallicFactor   = 0.0f;
        float roughnessFactor  = 0.5f;
    };

    struct VkSceneMesh
    {
        VkBuffer        vertexBuffer = VK_NULL_HANDLE;
        VkDeviceMemory  vertexMemory = VK_NULL_HANDLE;
        VkBuffer        indexBuffer  = VK_NULL_HANDLE;
        VkDeviceMemory  indexMemory  = VK_NULL_HANDLE;
        uint32_t        indexCount   = 0;
        XMFLOAT4        boundingSphere = {0,0,0,0};  // Phase 15B: object-space bounding sphere
        std::shared_ptr<VkMaterial> material;
    };

    std::vector<std::shared_ptr<VkSceneMesh>> _vkSceneMeshes;

    // Phase 18D: per-mesh info cache for AS build (populated in BuildMergedGeometry)
    struct MeshASInfo { uint32_t indexCount; uint32_t firstIndex; int32_t vertexOffset; uint32_t vertexCount; };
    std::vector<MeshASInfo> _meshASInfoCache;

    // Cached camera matrices for DrawMesh (XMFLOAT4X4 = DirectX::XMFLOAT4X4 via IRenderBackend.h)
    XMFLOAT4X4 _lastView{};
    XMFLOAT4X4 _lastProj{};

    // ---------------------------------------------------------------------------
    // ImGui descriptor pool
    // ---------------------------------------------------------------------------
    VkDescriptorPool _imguiDescriptorPool = VK_NULL_HANDLE;

    // ---------------------------------------------------------------------------
    // Window / vsync
    // ---------------------------------------------------------------------------
    uint32_t _width  = 0;
    uint32_t _height = 0;
    bool     _vsync  = false;  // false=MAILBOX/IMMEDIATE, true=FIFO

    // ---------------------------------------------------------------------------
    // Phase 10: Post-process stack — HDR, TAA, Bloom, ACES
    // ---------------------------------------------------------------------------
    struct VKTAAConstants        // must match taa.frag.hlsl cbuffer (160 B)
    {
        float invViewProj[16];   // 64 B
        float prevViewProj[16];  // 64 B
        float jitter[2];         //  8 B
        float prevJitter[2];     //  8 B
        float alpha;             //  4 B
        float _pad[3];           // 12 B  → 160 B total → 256-byte aligned buffer
    };

    bool     CreatePPResources();
    void     DestroyPPResources();
    void     UpdatePPDescriptors();  // called after fence wait in BeginFrame
    void     DrawVKTAAPass();
    void     DrawVKBloomBrightPass();
    void     DrawVKBloomBlurPass(bool horizontal);
    void     DrawVKTonemapPass();

    bool     _vkPPResourcesValid  = false;
    int      _vkTaaHistoryIndex   = 0;   // ping-pong write index
    uint32_t _vkFrameCount        = 0;
    float    _vkCurJitter[2]          = {};
    float    _vkPrevJitter[2]         = {};
    float    _vkPrevVP[16]            = {};  // legacy (kept for compat)
    float    _vkUnjitteredVP[16]      = {};  // current frame unjittered VP
    float    _vkPrevUnjitteredVP[16]  = {};  // previous frame unjittered VP (for TAA reprojection)

    // HDR intermediate (R16G16B16A16_SFLOAT, full-res)
    VkImage        _hdrImage       = VK_NULL_HANDLE;
    VkDeviceMemory _hdrMemory      = VK_NULL_HANDLE;
    VkImageView    _hdrView        = VK_NULL_HANDLE;
    VkRenderPass   _hdrRenderPass  = VK_NULL_HANDLE;   // forward PBR → HDR
    VkFramebuffer  _hdrFramebuffer = VK_NULL_HANDLE;

    // TAA history (R16G16B16A16_SFLOAT, full-res × 2)
    VkImage        _taaHistoryImage[2]  = {};
    VkDeviceMemory _taaHistoryMemory[2] = {};
    VkImageView    _taaHistoryView[2]   = {};
    VkFramebuffer  _taaFramebuffer[2]   = {};

    // Bloom half-res (R16G16B16A16_SFLOAT)
    VkImage        _bloomBrightImage  = VK_NULL_HANDLE;
    VkDeviceMemory _bloomBrightMemory = VK_NULL_HANDLE;
    VkImageView    _bloomBrightView   = VK_NULL_HANDLE;
    VkFramebuffer  _bloomBrightFramebuffer = VK_NULL_HANDLE;

    VkImage        _bloomBlurImage    = VK_NULL_HANDLE;
    VkDeviceMemory _bloomBlurMemory   = VK_NULL_HANDLE;
    VkImageView    _bloomBlurView     = VK_NULL_HANDLE;
    VkFramebuffer  _bloomBlurFramebuffer = VK_NULL_HANDLE;

    // Shared render passes
    VkRenderPass   _ppRenderPass      = VK_NULL_HANDLE;  // R16G16B16A16_SFLOAT → SHADER_READ_ONLY
    VkRenderPass   _tonemapRenderPass = VK_NULL_HANDLE;  // swapchain → PRESENT (replaces _renderPass for tone map)

    // Swapchain-facing tone map framebuffers (one per swapchain image)
    std::vector<VkFramebuffer> _tonemapFramebuffers;

    // PP descriptor set layouts
    VkDescriptorSetLayout _vkPP1SRVLayout = VK_NULL_HANDLE; // 1 SAMPLED_IMAGE + 1 SAMPLER
    VkDescriptorSetLayout _vkPP2SRVLayout = VK_NULL_HANDLE; // 2 SAMPLED_IMAGE + 1 SAMPLER
    VkDescriptorSetLayout _vkTAALayout    = VK_NULL_HANDLE; // UBO + 3 SAMPLED_IMAGE + 2 SAMPLER

    // PP descriptor pool
    VkDescriptorPool _vkPPDescPool = VK_NULL_HANDLE;

    // PP descriptor sets
    VkDescriptorSet _vkBloomBrightDescSet[2] = {};  // [i] reads from _taaHistoryView[i]
    VkDescriptorSet _vkBloomBlurHDescSet  = VK_NULL_HANDLE;
    VkDescriptorSet _vkBloomBlurVDescSet  = VK_NULL_HANDLE;
    VkDescriptorSet _vkTonemapDescSet[2]  = {};             // [0/1] for taaHistory[0/1]
    VkDescriptorSet _vkTAADescSet[FRAMES_IN_FLIGHT] = {};   // per-frame (updated each frame)

    // PP samplers
    VkSampler _vkLinearSampler = VK_NULL_HANDLE;
    VkSampler _vkPointSampler  = VK_NULL_HANDLE;

    // PP pipeline layouts + pipelines
    VkPipelineLayout _vkBloomPipelineLayout   = VK_NULL_HANDLE; // PP1SRV + push 16B
    VkPipelineLayout _vkTonemapPipelineLayout = VK_NULL_HANDLE; // PP2SRV + push 16B
    VkPipelineLayout _vkTAAPipelineLayout     = VK_NULL_HANDLE; // TAA layout only

    VkPipeline _vkTAAPipeline         = VK_NULL_HANDLE;
    VkPipeline _vkBloomBrightPipeline = VK_NULL_HANDLE;
    VkPipeline _vkBloomBlurPipeline   = VK_NULL_HANDLE;  // shared H+V
    VkPipeline _vkTonemapPipeline     = VK_NULL_HANDLE;

    // Per-frame TAA UBOs
    VkBuffer       _vkTaaCB[FRAMES_IN_FLIGHT]       = {};
    VkDeviceMemory _vkTaaCBMemory[FRAMES_IN_FLIGHT] = {};
    void*          _vkTaaCBMapped[FRAMES_IN_FLIGHT] = {};

    // ---------------------------------------------------------------------------
    // Phase 16C: SSR + HDR RT intermediate
    // ---------------------------------------------------------------------------
    // SSR image (R16G16B16A16_SFLOAT, STORAGE | SAMPLED, kept in GENERAL)
    VkImage        _ssrImage  = VK_NULL_HANDLE;
    VkDeviceMemory _ssrMemory = VK_NULL_HANDLE;
    VkImageView    _ssrView   = VK_NULL_HANDLE;

    // Framebuffer for the deferred pass targeting _hdrImage via _ppRenderPass
    VkFramebuffer  _deferredHDRFramebuffer = VK_NULL_HANDLE;

    // SSR compute pipeline
    VkDescriptorSetLayout _vkSSRLayout     = VK_NULL_HANDLE;
    VkDescriptorPool      _vkSSRDescPool   = VK_NULL_HANDLE;
    VkDescriptorSet       _vkSSRDescSet[FRAMES_IN_FLIGHT] = {};
    VkPipelineLayout      _vkSSRPipeLayout = VK_NULL_HANDLE;
    VkPipeline            _vkSSRPipeline   = VK_NULL_HANDLE;

    // SSR constants UBO (per-frame, HOST_COHERENT)
    VkBuffer       _vkSSRCB[FRAMES_IN_FLIGHT]       = {};
    VkDeviceMemory _vkSSRCBMem[FRAMES_IN_FLIGHT]    = {};
    void*          _vkSSRCBMapped[FRAMES_IN_FLIGHT] = {};

    // SSR tonemap pipeline (reads _hdrImage + _ssrImage → swapchain)
    VkDescriptorSetLayout _vkSSRTonemapLayout     = VK_NULL_HANDLE;
    VkDescriptorPool      _vkSSRTonemapDescPool   = VK_NULL_HANDLE;
    VkDescriptorSet       _vkSSRTonemapDescSet    = VK_NULL_HANDLE;
    VkPipelineLayout      _vkSSRTonemapPipeLayout = VK_NULL_HANDLE;
    VkPipeline            _vkSSRTonemapPipeline   = VK_NULL_HANDLE;

    // ---------------------------------------------------------------------------
    // Phase 18B: Screen-Space Motion Blur (Vulkan)
    // ---------------------------------------------------------------------------
    struct VKMotionBlurConstants  // 152 B → padded to 256 B in UBO
    {
        float invViewProj[16];    // 64 B
        float prevViewProj[16];   // 64 B
        float screenSizeX;        //  4 B
        float screenSizeY;        //  4 B
        float shutterScale;       //  4 B
        int   numSamples;         //  4 B
        float _pad[2];            //  8 B → 152 B (stored in 256 B UBO)
    };

    void DrawVKMotionBlurPass();

    VkImage        _mbImage  = VK_NULL_HANDLE;
    VkDeviceMemory _mbMemory = VK_NULL_HANDLE;
    VkImageView    _mbView   = VK_NULL_HANDLE;
    VkFramebuffer  _mbFB     = VK_NULL_HANDLE;

    VkDescriptorSetLayout _vkMBLayout   = VK_NULL_HANDLE;
    VkDescriptorPool      _vkMBDescPool = VK_NULL_HANDLE;
    VkDescriptorSet       _vkMBDescSet[FRAMES_IN_FLIGHT] = {};
    VkPipelineLayout      _vkMBPipeLayout = VK_NULL_HANDLE;
    VkPipeline            _vkMBPipeline   = VK_NULL_HANDLE;

    VkBuffer       _vkMBCB[FRAMES_IN_FLIGHT]       = {};
    VkDeviceMemory _vkMBCBMem[FRAMES_IN_FLIGHT]    = {};
    void*          _vkMBCBMapped[FRAMES_IN_FLIGHT] = {};

    XMFLOAT4X4     _vkMBLastVP = {};  // previous-frame VP for motion vectors

    // ---------------------------------------------------------------------------
    // Phase 18C: Vulkan Render Graph
    // ---------------------------------------------------------------------------
    // VulkanRenderGraph is used in CompositeFrame() to automatically schedule barriers.
    // Included inline here to avoid header dependency in the public interface.
    // The actual graph object is forward-declared and instantiated in the .cpp.

    // ---------------------------------------------------------------------------
    // Phase 18D: Vulkan Ray Tracing
    // ---------------------------------------------------------------------------
    struct VKAccelStruct
    {
        VkAccelerationStructureKHR as  = VK_NULL_HANDLE;
        VkBuffer                   buf = VK_NULL_HANDLE;
        VkDeviceMemory             mem = VK_NULL_HANDLE;
    };

    bool _rtSupported = false;

    // Function pointers (loaded at Init time if RT is supported)
    PFN_vkCreateAccelerationStructureKHR         pfn_vkCreateAccelerationStructureKHR         = nullptr;
    PFN_vkDestroyAccelerationStructureKHR        pfn_vkDestroyAccelerationStructureKHR        = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR  pfn_vkGetAccelerationStructureBuildSizesKHR  = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR      pfn_vkCmdBuildAccelerationStructuresKHR      = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR pfn_vkGetAccelerationStructureDeviceAddressKHR = nullptr;
    PFN_vkCreateRayTracingPipelinesKHR           pfn_vkCreateRayTracingPipelinesKHR           = nullptr;
    PFN_vkGetRayTracingShaderGroupHandlesKHR     pfn_vkGetRayTracingShaderGroupHandlesKHR     = nullptr;
    PFN_vkCmdTraceRaysKHR                        pfn_vkCmdTraceRaysKHR                        = nullptr;

    std::vector<VKAccelStruct> _vkBLASes;
    VKAccelStruct              _vkTLAS;
    VkBuffer                   _vkInstanceBuf = VK_NULL_HANDLE;
    VkDeviceMemory             _vkInstanceMem = VK_NULL_HANDLE;

    VkDescriptorSetLayout _vkRTLayout   = VK_NULL_HANDLE;
    VkDescriptorPool      _vkRTDescPool = VK_NULL_HANDLE;
    VkDescriptorSet       _vkRTDescSet[FRAMES_IN_FLIGHT] = {};
    VkPipelineLayout      _vkRTPipeLayout = VK_NULL_HANDLE;
    VkPipeline            _vkRTPipeline   = VK_NULL_HANDLE;

    // Per-frame RT scene UBO (invViewProj + lightDir + maxDist, 256B)
    VkBuffer       _vkRTSceneCB[FRAMES_IN_FLIGHT]       = {};
    VkDeviceMemory _vkRTSceneCBMem[FRAMES_IN_FLIGHT]    = {};
    void*          _vkRTSceneCBMapped[FRAMES_IN_FLIGHT] = {};

    VkImage        _vkShadowMaskImage = VK_NULL_HANDLE;
    VkDeviceMemory _vkShadowMaskMem   = VK_NULL_HANDLE;
    VkImageView    _vkShadowMaskView  = VK_NULL_HANDLE;

    VkBuffer       _vkSBTBuffer = VK_NULL_HANDLE;
    VkDeviceMemory _vkSBTMem    = VK_NULL_HANDLE;

    VkStridedDeviceAddressRegionKHR _vkRgenRegion = {};
    VkStridedDeviceAddressRegionKHR _vkMissRegion = {};
    VkStridedDeviceAddressRegionKHR _vkHitRegion  = {};
    VkStridedDeviceAddressRegionKHR _vkCallRegion = {};

    bool BuildAccelerationStructures();
    void DestroyAccelerationStructures();
    bool CreateRTPipeline();
    void DestroyRTPipeline();
};

} // namespace Luna
