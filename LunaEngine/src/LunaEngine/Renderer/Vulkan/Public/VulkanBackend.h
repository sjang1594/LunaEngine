#pragma once

#include <LunaEngine/LunaPCH.h>
#include <unordered_map>
#include <LunaEngine/Renderer/HAL/Public/IRenderBackend.h>
#include <LunaEngine/Renderer/Vulkan/Public/VulkanGPUProfiler.h>
#include <LunaEngine/Renderer/Vulkan/Public/VulkanCore.h>
#include <LunaEngine/Renderer/Vulkan/Public/VulkanSSAO.h>
#include <LunaEngine/Renderer/Vulkan/Public/VulkanPostProcess.h>
#include <LunaEngine/Renderer/Vulkan/Public/VulkanShadows.h>
#include <LunaEngine/Renderer/Vulkan/Public/VulkanHiZ.h>
#include <LunaEngine/Renderer/Vulkan/Public/VulkanIBL.h>
#include <LunaEngine/Renderer/Vulkan/Public/VulkanAtmosphere.h>
#include <LunaEngine/Renderer/Vulkan/Public/VulkanVolumetricFog.h>
#include <LunaEngine/Renderer/Vulkan/Public/VulkanGI.h>
#include <LunaEngine/Renderer/Vulkan/Public/VulkanSwapchain.h>
#include <LunaEngine/Renderer/Vulkan/Public/VulkanGBuffer.h>

namespace Luna
{
// Forward declarations — only types actually used in the public interface
struct Mesh;        // returned by LoadMeshes; also in IRenderBackend.h (redundant but explicit)
class  VulkanDevice;
class  CameraSensor;  // S2b

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
    std::vector<XMFLOAT4X4> GetLastLoadTransforms() const override { return _lastLoadTransforms; }

    // Debug: procedural 2×2m quad (bypasses glTF, uses default white material)
    std::vector<std::shared_ptr<Mesh>> LoadDebugQuad() override;
    std::vector<std::shared_ptr<Mesh>> LoadCalibrationScene() override;

    // Phase 15B: flush accumulated DrawMesh() calls via GPU-driven indirect rendering
    void FlushDraws() override;

    // Phase 15C: load equirectangular HDR + run IBL precompute on GPU
    bool LoadHDREnvironment(const std::string& hdrPath) override;

    const char* GetBackendName() const override { return "Vulkan"; }
    IGPUProfiler* GetGPUProfiler() override { return &_gpuProfiler; }

    // Phase 30: Set GI params from UI
    void SetGIParams(const GIParams& p) override {
        _gi.temporalAlpha   = p.temporalAlpha;
        _gi.numSSGIRays     = (uint32_t)p.numSSGIRays;
        _gi.maxRayDist      = p.maxRayDist;
        _gi.probeOrigin[0]  = p.probeGridOrigin[0];
        _gi.probeOrigin[1]  = p.probeGridOrigin[1];
        _gi.probeOrigin[2]  = p.probeGridOrigin[2];
        _gi.probeSpacing[0] = p.probeGridSpacing[0];
        _gi.probeSpacing[1] = p.probeGridSpacing[1];
        _gi.probeSpacing[2] = p.probeGridSpacing[2];
    }

    // Phase 29: Set volumetric fog params from UI
    void SetVolumetricFogParams(const VolumetricFogParams& p) override {
        _volFogEnabled               = p.enabled;
        _volumetricFog.density       = p.density;
        _volumetricFog.heightFalloff = p.heightFalloff;
        _volumetricFog.baseHeight    = p.baseHeight;
        _volumetricFog.scattering    = p.scattering;
        _volumetricFog.extinction    = p.extinction;
        _volumetricFog.phaseG        = p.phaseG;
    }

    // Phase 24: Set point lights from UI
    void SetPointLights(const std::vector<PointLightDesc>& lights) override
    {
        _pointLights.resize(lights.size());
        for (size_t i = 0; i < lights.size(); i++) {
            auto& dst = _pointLights[i];
            const auto& src = lights[i];
            memcpy(dst.position, src.position, 12);
            dst.radius = src.radius;
            memcpy(dst.color, src.color, 12);
            dst.intensity = src.intensity;
        }
    }

  private:
    // ---------------------------------------------------------------------------
    // Init helpers
    // ---------------------------------------------------------------------------
    bool CreateInstance();
    bool SetupDebugMessenger();
    bool CreateSurface(void* windowHandle);
    bool CreateImGuiDescriptorPool();

    // G-buffer resources (delegated to VulkanGBuffer)
    void UpdateDeferredGbufDescriptors();  // called after G-buffer image (re)creation

    // Deferred lighting pipeline
    bool CreateDeferredPipeline();
    void DestroyDeferredPipeline();

    // SSAO
    bool CreateSSAOResources();
    void DestroySSAOResources();
    void DrawSSAOPass(VkCommandBuffer cmd);
    void DrawSSAOBlurPass(VkCommandBuffer cmd);

    // Swapchain lifecycle (delegated to VulkanSwapchain)
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
    // Core Vulkan infrastructure (instance, surface, device, helpers)
    // All subsystems (VulkanSSAO, VulkanPostProcess, etc.) depend on this.
    // ---------------------------------------------------------------------------
    VulkanCore _core;

    // ---------------------------------------------------------------------------
    // Extracted Subsystems
    // ---------------------------------------------------------------------------
    VulkanSSAO _ssao;  // SSAO subsystem (replaces inline SSAO code)
    VulkanPostProcess _postProcess;  // Post-process subsystem (replaces inline PP code)
    VulkanShadows _shadows;  // CSM subsystem (replaces inline CSM code)
    VulkanHiZ _hiZ;  // Hi-Z pyramid subsystem (replaces inline Hi-Z code)
    VulkanIBL _ibl;  // IBL subsystem (replaces inline IBL precompute code)
    VulkanAtmosphere    _atmosphere;     // Phase 28: physically-based sky rendering
    VulkanVolumetricFog _volumetricFog;  // Phase 29: froxel-based volumetric fog
    bool                _volFogEnabled = false;
    VulkanGI            _gi;             // Phase 30: screen-space GI + irradiance probes
    VulkanSwapchain _vkSwapchain;  // Swapchain + depth + present render pass
    VulkanGBuffer _gBuffer;  // G-buffer images + render passes

    // ---------------------------------------------------------------------------
    // Legacy Core Vulkan objects (being migrated to VulkanCore)
    // TODO: Remove after full migration to VulkanCore
    // ---------------------------------------------------------------------------
    VkInstance   _instance = VK_NULL_HANDLE;
    VkSurfaceKHR _surface  = VK_NULL_HANDLE;

    std::unique_ptr<VulkanDevice> _device;

    VkDebugUtilsMessengerEXT _debugMessenger = VK_NULL_HANDLE;

    // Device lost flag - once set, all frame operations bail out early
    bool _deviceLost = false;

    // Dedicated command pool for single-time/transfer commands (avoids race with frame pools)
    VkCommandPool _transferCmdPool = VK_NULL_HANDLE;


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
        uint32_t rtEnabled;    uint32_t numPointLights; uint32_t _p2[2]; //  16 B — Phase 18D RT shadow flag + Phase 24 light count
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

    // S2b: cached scene lighting for sensor renders
    XMFLOAT3 _cachedLightDir   = {0.408f, 0.816f, 0.408f};
    XMFLOAT4 _cachedLightColor = {1.0f, 1.0f, 1.0f, 3.0f};

    // ---------------------------------------------------------------------------
    // CSM — mesh model cache (used to build ShadowDraw list for VulkanShadows)
    // ---------------------------------------------------------------------------
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


    bool CreateIndirectResources();
    void DestroyIndirectResources();
    void BuildMergedGeometry(const std::vector<std::vector<struct PBRVertex>>& allVerts,
                              const std::vector<std::vector<uint32_t>>& allIdxs);

    // ---------------------------------------------------------------------------
    // Phase 27: Vulkan Mesh Shaders (VK_EXT_mesh_shader)
    // ---------------------------------------------------------------------------
    bool _meshShaderReady = false;

    bool CreateMeshShaderResources();
    void DestroyMeshShaderResources();

    // Meshlet GPU buffers (uploaded alongside merged geometry in BuildMergedGeometry)
    VkBuffer       _vkMeshletBuffer       = VK_NULL_HANDLE;  VkDeviceMemory _vkMeshletMem       = VK_NULL_HANDLE;
    VkBuffer       _vkMeshletBoundsBuffer = VK_NULL_HANDLE;  VkDeviceMemory _vkMeshletBoundsMem = VK_NULL_HANDLE;
    VkBuffer       _vkMeshletVertBuffer   = VK_NULL_HANDLE;  VkDeviceMemory _vkMeshletVertMem   = VK_NULL_HANDLE;
    VkBuffer       _vkMeshletTriBuffer    = VK_NULL_HANDLE;  VkDeviceMemory _vkMeshletTriMem    = VK_NULL_HANDLE;

    // Per-mesh meshlet metadata (populated during BuildMergedGeometry)
    std::vector<uint32_t> _vkMeshMeshletOffsets;   // per-mesh: first meshlet index
    std::vector<uint32_t> _vkMeshMeshletCounts;    // per-mesh: meshlet count

    // Descriptor set (set=0: 6 SSBOs — objects, meshlets, bounds, vertices, meshletVerts, meshletTris)
    VkDescriptorSetLayout _meshShaderDescLayout = VK_NULL_HANDLE;
    VkDescriptorPool      _meshShaderDescPool   = VK_NULL_HANDLE;
    VkDescriptorSet       _meshShaderDescSet    = VK_NULL_HANDLE;

    // Pipeline
    VkPipelineLayout _meshShaderPipeLayout = VK_NULL_HANDLE;
    VkPipeline       _meshShaderPipeline   = VK_NULL_HANDLE;

    // Function pointer for vkCmdDrawMeshTasksEXT
    PFN_vkCmdDrawMeshTasksEXT pfn_vkCmdDrawMeshTasksEXT = nullptr;

    // ---------------------------------------------------------------------------
    // Phase 20: Vulkan Async Compute
    // ---------------------------------------------------------------------------
    bool _asyncComputeReady = false;
    bool _computeSubmittedThisFrame = false;  // per-frame flag for EndFrame semaphore wait

    struct VkComputeFrameResource
    {
        VkCommandPool   cmdPool   = VK_NULL_HANDLE;
        VkCommandBuffer cmdBuffer = VK_NULL_HANDLE;
        VkSemaphore     doneSemaphore = VK_NULL_HANDLE;  // signaled by compute submit, waited by graphics
        VkFence         fence     = VK_NULL_HANDLE;       // CPU wait for compute completion
    };
    VkComputeFrameResource _computeFrames[FRAMES_IN_FLIGHT];

    bool CreateAsyncComputeResources();
    void DestroyAsyncComputeResources();
    void DispatchCullAsync();  // record + submit cull on compute queue


    // ---------------------------------------------------------------------------
    // Phase 15C: IBL deferred lighting pipeline
    // ---------------------------------------------------------------------------
    // IBL deferred lighting pipeline (replaces _deferredPipeline when IBL is ready)
    VkPipeline _deferredIBLPipeline = VK_NULL_HANDLE;

    // ---------------------------------------------------------------------------
    // Phase 30: GI deferred lighting pipeline (set=3: ssgiTex + probeIrr + UBO)
    // ---------------------------------------------------------------------------
    struct ProbeGridUBO {
        float    origin[4];    // xyz + pad
        float    spacing[4];   // xyz + pad
        uint32_t dims[4];      // xyz + pad  → 48 bytes
    };
    static_assert(sizeof(ProbeGridUBO) == 48);

    VkDescriptorSetLayout _giDescLayout           = VK_NULL_HANDLE;
    VkDescriptorPool      _giDescPool             = VK_NULL_HANDLE;
    VkDescriptorSet       _giDescSet[FRAMES_IN_FLIGHT] = {};
    VkPipelineLayout      _deferredGIPipeLayout    = VK_NULL_HANDLE;
    VkPipeline            _deferredGIPipeline      = VK_NULL_HANDLE;
    VkSampler             _giSampler               = VK_NULL_HANDLE;
    VkBuffer              _giProbeGridUBO[FRAMES_IN_FLIGHT]    = {};
    VkDeviceMemory        _giProbeGridMem[FRAMES_IN_FLIGHT]    = {};
    void*                 _giProbeGridMapped[FRAMES_IN_FLIGHT] = {};

    bool CreateGIDeferredResources();
    void DestroyGIDeferredResources();
    void UpdateGIDescriptorSet(uint32_t frameIndex);


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
        float alpha            = 1.0f;  // Phase 31: OIT — <1.0 routes mesh to transparent pass
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
    std::vector<XMFLOAT4X4>                  _lastLoadTransforms; // Phase 21

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

    // Deferred resize — applied at the start of BeginFrame to avoid
    // destroying framebuffers while a command buffer is recording.
    bool     _pendingResize  = false;
    uint32_t _pendingResizeW = 0;
    uint32_t _pendingResizeH = 0;

    // Frames since last resize — RT is disabled for a few frames after resize to stabilize
    uint32_t _framesSinceResize = 100;  // Start high so RT is enabled immediately

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

    // ---------------------------------------------------------------------------
    // Phase 24: Clustered Lighting
    // ---------------------------------------------------------------------------
    static constexpr uint32_t CLUSTER_X = 16;
    static constexpr uint32_t CLUSTER_Y = 9;
    static constexpr uint32_t CLUSTER_Z = 24;
    static constexpr uint32_t CLUSTER_COUNT = CLUSTER_X * CLUSTER_Y * CLUSTER_Z;
    static constexpr uint32_t MAX_LIGHTS_PER_CLUSTER = 128;
    static constexpr uint32_t MAX_POINT_LIGHTS = 1024;

    struct GPUPointLight  // 32 bytes — matches GLSL std430
    {
        float position[3]; float radius;
        float color[3];    float intensity;
    };

    struct ClusterParamsUBO  // 96 bytes → alloc 256 B
    {
        float invProj[16];   // 64 B  (row-major inverse projection)
        float nearZ;         //  4 B
        float farZ;          //  4 B
        float screenW;       //  4 B
        float screenH;       //  4 B
        uint32_t numLights;  //  4 B
        uint32_t _pad[3];    // 12 B
    };

    std::vector<GPUPointLight> _pointLights;  // CPU-side light list
    bool _clusteredLightingReady = false;

    // Light SSBO (host-visible, updated per-frame)
    VkBuffer       _lightSSBO     = VK_NULL_HANDLE;
    VkDeviceMemory _lightSSBOMem  = VK_NULL_HANDLE;
    void*          _lightSSBOMapped = nullptr;

    // Cluster counts SSBO (device-local, written by compute)
    VkBuffer       _clusterCountsSSBO    = VK_NULL_HANDLE;
    VkDeviceMemory _clusterCountsSSBOMem = VK_NULL_HANDLE;

    // Cluster light indices SSBO (device-local, written by compute)
    VkBuffer       _clusterIndicesSSBO    = VK_NULL_HANDLE;
    VkDeviceMemory _clusterIndicesSSBOMem = VK_NULL_HANDLE;

    // Cluster params UBO (host-visible, updated per-frame)
    VkBuffer       _clusterParamsCB     = VK_NULL_HANDLE;
    VkDeviceMemory _clusterParamsCBMem  = VK_NULL_HANDLE;
    void*          _clusterParamsCBMapped = nullptr;

    // Cluster assign compute pipeline
    VkDescriptorSetLayout _clusterCompDescLayout = VK_NULL_HANDLE;
    VkDescriptorPool      _clusterCompDescPool   = VK_NULL_HANDLE;
    VkDescriptorSet       _clusterCompDescSet    = VK_NULL_HANDLE;
    VkPipelineLayout      _clusterCompPipeLayout = VK_NULL_HANDLE;
    VkPipeline            _clusterCompPipeline   = VK_NULL_HANDLE;

    // Deferred lighting set=2 (light SSBO + cluster SSBOs + cluster params)
    VkDescriptorSetLayout _clusterLightLayout = VK_NULL_HANDLE;
    VkDescriptorSet       _clusterLightDescSet = VK_NULL_HANDLE;

    bool CreateClusteredLightingResources();
    void DestroyClusteredLightingResources();
    void DispatchClusterAssign(VkCommandBuffer cmd);

    // ── GPU Profiler ──
    VulkanGPUProfiler _gpuProfiler;

    // ---------------------------------------------------------------------------
    // Phase 31: Order-Independent Transparency (WBOIT)
    // ---------------------------------------------------------------------------
private:
    static constexpr uint32_t MAX_OIT_MESHES = 256;

    struct VKOITMeshDraw
    {
        const VkSceneMesh* mesh;
        XMFLOAT4X4          model;
        float               alpha;
    };

    bool _vkOitReady = false;
    std::vector<VKOITMeshDraw> _vkOitMeshes;

    bool CreateVKOITResources();
    void DestroyVKOITResources();
    void DrawVKOITForward(VkCommandBuffer cmd);
    void DrawVKOITComposite(VkCommandBuffer cmd);

    // OIT accum image (RGBA16F) and revealage image (R8_UNORM)
    VkImage        _oitAccumImage  = VK_NULL_HANDLE;
    VkDeviceMemory _oitAccumMem    = VK_NULL_HANDLE;
    VkImageView    _oitAccumView   = VK_NULL_HANDLE;

    VkImage        _oitRevealImage = VK_NULL_HANDLE;
    VkDeviceMemory _oitRevealMem   = VK_NULL_HANDLE;
    VkImageView    _oitRevealView  = VK_NULL_HANDLE;

    // OIT Forward render pass: 2 color (accum+revealage) + depth READ_ONLY
    VkRenderPass  _oitFwdRenderPass  = VK_NULL_HANDLE;
    VkFramebuffer _oitFwdFramebuffer = VK_NULL_HANDLE;

    // OIT Composite render pass: 1 color (HDR, LOAD_OP_LOAD blend)
    VkRenderPass  _oitCmpRenderPass  = VK_NULL_HANDLE;
    VkFramebuffer _oitCmpFramebuffer = VK_NULL_HANDLE;

    // Scene UBO for OIT forward pass (view + proj + lightDir + lightColor, 160B → 256B buffer)
    struct OITSceneData
    {
        float view[16];        //  64 B
        float proj[16];        //  64 B
        float lightDir[3];     //  12 B
        float _p0;             //   4 B
        float lightColor[4];   //  16 B → 160 B total, stored in 256 B buffer
    };

    VkBuffer        _oitSceneUBO[FRAMES_IN_FLIGHT]       = {};
    VkDeviceMemory  _oitSceneUBOMem[FRAMES_IN_FLIGHT]    = {};
    void*           _oitSceneUBOMapped[FRAMES_IN_FLIGHT] = {};

    // Descriptor set layouts
    VkDescriptorSetLayout _oitSceneLayout  = VK_NULL_HANDLE;  // set=0: OITSceneUBO
    VkDescriptorSetLayout _oitAlbedoLayout = VK_NULL_HANDLE;  // set=1: albedoTex (combined sampler)
    VkDescriptorSetLayout _oitCmpLayout    = VK_NULL_HANDLE;  // set=0 composite: accum+revealage

    // Descriptor pool (scene per frame + albedo MAX_OIT_MESHES×FRAMES + composite)
    VkDescriptorPool _oitDescPool                              = VK_NULL_HANDLE;
    VkDescriptorSet  _oitSceneDescSet[FRAMES_IN_FLIGHT]        = {};
    VkDescriptorSet  _oitAlbedoDescSets[FRAMES_IN_FLIGHT][256] = {};  // [frame][meshSlot]
    VkDescriptorSet  _oitCmpDescSet                            = VK_NULL_HANDLE;

    VkSampler _oitLinearSampler = VK_NULL_HANDLE;  // for albedo in OIT forward
    VkSampler _oitPointSampler  = VK_NULL_HANDLE;  // for accum/revealage in OIT composite

    // Pipeline layouts and pipelines
    VkPipelineLayout _oitFwdPipeLayout = VK_NULL_HANDLE;
    VkPipeline       _oitFwdPipeline   = VK_NULL_HANDLE;
    VkPipelineLayout _oitCmpPipeLayout = VK_NULL_HANDLE;
    VkPipeline       _oitCmpPipeline   = VK_NULL_HANDLE;

    // ── Phase 32: Visibility Buffer ───────────────────────────────────────────
    bool _vkVisBufferReady = false;
    bool _vkVisBufferMode  = false;

    // Visibility RT (VK_FORMAT_R32_UINT, full-res)
    VkImage        _visImage      = VK_NULL_HANDLE;
    VkDeviceMemory _visImageMem   = VK_NULL_HANDLE;
    VkImageView    _visImageView  = VK_NULL_HANDLE;

    // Render pass + framebuffer for vis pass (single R32_UINT attachment + depth)
    VkRenderPass   _visRenderPass   = VK_NULL_HANDLE;
    VkFramebuffer  _visFramebuffer  = VK_NULL_HANDLE;

    // G-buffer storage image views for shade compute UAV write
    VkImageView    _visGB0StorageView = VK_NULL_HANDLE;
    VkImageView    _visGB1StorageView = VK_NULL_HANDLE;
    VkImageView    _visGB2StorageView = VK_NULL_HANDLE;

    // Descriptor sets for shade compute
    VkDescriptorSetLayout _visShadeSet0Layout = VK_NULL_HANDLE; // UBO + vis RT
    VkDescriptorSetLayout _visShadeSet1Layout = VK_NULL_HANDLE; // VB + IB + objects + meshInfos
    VkDescriptorSetLayout _visShadeSet2Layout = VK_NULL_HANDLE; // G-buffer UAVs
    VkDescriptorPool      _visShadePool       = VK_NULL_HANDLE;
    VkDescriptorSet       _visShadeSet0[FRAMES_IN_FLIGHT] = {};
    VkDescriptorSet       _visShadeSet1       = VK_NULL_HANDLE;
    VkDescriptorSet       _visShadeSet2       = VK_NULL_HANDLE;

    // Per-frame shade constants UBO
    VkBuffer       _visShadeUBO[FRAMES_IN_FLIGHT]    = {};
    VkDeviceMemory _visShadeUBOMem[FRAMES_IN_FLIGHT] = {};
    void*          _visShadeUBOMapped[FRAMES_IN_FLIGHT] = {};

    // Sampler for shade compute (anisotropic wrap)
    VkSampler      _visShadeSampler = VK_NULL_HANDLE;

    // Vis pass pipeline (VS+PS)
    VkPipelineLayout _visPipeLayout  = VK_NULL_HANDLE;
    VkPipeline       _visPipeline    = VK_NULL_HANDLE;

    // Shade compute pipeline
    VkPipelineLayout _visShadeLayout  = VK_NULL_HANDLE;
    VkPipeline       _visShadePipeline = VK_NULL_HANDLE;

    bool CreateVKVisibilityResources();
    void DestroyVKVisibilityResources();
    void DrawVKVisibilityPass(VkCommandBuffer cmd);
    void DispatchVKVisibilityShade(VkCommandBuffer cmd);

    // ── S2b: Vulkan Camera Sensor Rendering ─────────────────────────────────
    // Per-sensor GPU resource bundle
    struct VulkanCameraResources
    {
        static constexpr uint32_t MAX_DRAWS = 512; // max meshes per sensor render

        // G-buffer (reuse Phase 32 class)
        VulkanGBuffer gbuffer;
        // Dedicated depth
        VkImage        depthImage   = VK_NULL_HANDLE;
        VkDeviceMemory depthMemory  = VK_NULL_HANDLE;
        VkImageView    depthView    = VK_NULL_HANDLE;
        // Lit HDR output (RGBA16F)
        VkImage        litImage     = VK_NULL_HANDLE;
        VkDeviceMemory litMemory    = VK_NULL_HANDLE;
        VkImageView    litView      = VK_NULL_HANDLE;
        VkRenderPass   litRP        = VK_NULL_HANDLE;
        VkFramebuffer  litFB        = VK_NULL_HANDLE;
        // Distorted output (RGBA8, GENERAL layout)
        VkImage        distortImage = VK_NULL_HANDLE;
        VkDeviceMemory distortMemory= VK_NULL_HANDLE;
        VkImageView    distortView  = VK_NULL_HANDLE;
        // CPU readback staging (HOST_VISIBLE | HOST_COHERENT, persistently mapped)
        VkBuffer       stagingRGB   = VK_NULL_HANDLE;
        VkDeviceMemory stagingMem   = VK_NULL_HANDLE;
        void*          stagingMapped= nullptr;
        // G-buffer fill: sensor MVP UBO (MAX_DRAWS * 256B per frame, triple-buffered)
        VkBuffer       sensorMVPBuf = VK_NULL_HANDLE;
        VkDeviceMemory sensorMVPMem = VK_NULL_HANDLE;
        void*          sensorMVPMapped = nullptr;
        VkDescriptorPool gbufPool   = VK_NULL_HANDLE;
        VkDescriptorSet  gbufSets[3]= {};  // [fi]: mvpBuf (dynamic) + scene UBO
        // Sensor lighting pass
        VkBuffer       litSceneUBO[3]   = {};
        VkDeviceMemory litSceneUBOMem[3]= {};
        void*          litSceneUBOMapped[3]= {};
        VkDescriptorPool litPool        = VK_NULL_HANDLE;
        VkDescriptorSet  litSceneSets[3]= {};  // set=0: scene UBO [fi]
        VkDescriptorSet  litGBufSet     = VK_NULL_HANDLE;  // set=1: G-buffer samplers
        VkSampler        litSampler     = VK_NULL_HANDLE;  // for G-buffer + litRT sampling
        // Distortion compute
        VkBuffer       distortUBO[3]    = {};
        VkDeviceMemory distortUBOMem[3] = {};
        void*          distortUBOMapped[3] = {};
        VkDescriptorPool distortPool    = VK_NULL_HANDLE;
        VkDescriptorSet  distortUBOSets[3] = {};  // set=0: distort UBO [fi]
        VkDescriptorSet  distortLitSet  = VK_NULL_HANDLE;  // set=1: litRT sampler
        VkDescriptorSet  distortOutSet  = VK_NULL_HANDLE;  // set=2: distortRT storage
        // ImGui display (from ImGui_ImplVulkan_AddTexture)
        VkDescriptorSet  imguiSet       = VK_NULL_HANDLE;
        // Tracking
        uint32_t width  = 0, height = 0;
        bool     ready  = false;
        bool     firstRender = true;
    };

    std::unordered_map<CameraSensor*, VulkanCameraResources> _vkCameraRTs;

    // Shared sensor pipelines (created once in Init)
    VkDescriptorSetLayout _sensorSceneLayout   = VK_NULL_HANDLE; // set=0: scene UBO
    VkDescriptorSetLayout _sensorGBufLayout    = VK_NULL_HANDLE; // set=1: 4 COMBINED_IMAGE_SAMPLER
    VkDescriptorSetLayout _sensorIBLLayout     = VK_NULL_HANDLE; // set=2: 3 COMBINED_IMAGE_SAMPLER
    VkPipelineLayout      _sensorLitPipeLayout = VK_NULL_HANDLE;
    VkPipeline            _sensorLitPipeline   = VK_NULL_HANDLE;

    VkDescriptorSetLayout _sensorDistUBOLayout   = VK_NULL_HANDLE; // set=0: distort UBO
    VkDescriptorSetLayout _sensorDistInputLayout = VK_NULL_HANDLE; // set=1: litRT sampler
    VkDescriptorSetLayout _sensorDistOutLayout   = VK_NULL_HANDLE; // set=2: distortRT storage
    VkPipelineLayout      _sensorDistPipeLayout  = VK_NULL_HANDLE;
    VkPipeline            _sensorDistPipeline    = VK_NULL_HANDLE;

    VkDescriptorPool _sensorIBLPool    = VK_NULL_HANDLE;
    VkDescriptorSet  _sensorIBLSet     = VK_NULL_HANDLE; // shared IBL set for all cameras

    bool CreateSensorLightingPipeline();
    bool CreateSensorDistortPipeline();
    bool CreateSensorIBLDescriptorSet();
    bool InitVKCameraResources(CameraSensor* cam);
    void DestroyVKCameraResources(CameraSensor* cam);
    void RenderVKCameraSensorInternal(CameraSensor* cam, VkCommandBuffer cmd, uint32_t fi);
    void RenderCameraSensors() override;
};

} // namespace Luna
