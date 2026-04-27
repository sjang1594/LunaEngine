#pragma once

#include <vulkan/vulkan.h>
#include <DirectXMath.h>
#include <vector>
#include <memory>

using namespace DirectX;

namespace Luna
{

class VulkanCore;
struct PBRVertex;

/**
 * @brief GPU-driven indirect rendering subsystem.
 *
 * Owns merged geometry, compute culling pipeline, and indirect draw resources.
 * Provides building blocks for GPU culling + indirect draw; frame loop
 * orchestration stays in VulkanBackend.
 *
 * Thread Safety: NOT thread-safe. Call from main render thread only.
 */
class VulkanGPUDriven
{
public:
    static constexpr uint32_t MAX_GPU_OBJECTS = 1024;
    static constexpr uint32_t FRAMES_IN_FLIGHT = 2;

    // Material data for bindless rendering (simple passthrough, no VkMaterial dependency)
    struct MaterialData
    {
        float albedoFactor[4];
        float metallicFactor;
        float roughnessFactor;
        VkImageView albedoView;
        VkImageView normalView;
        VkImageView metalRoughView;
        VkImageView emissiveView;
    };

    // Per-object data uploaded to GPU
    struct alignas(16) GPUObjectData
    {
        XMFLOAT4X4 model;           // 64 B
        XMFLOAT4   boundingSphere;  // 16 B
        uint32_t   meshIndex;       //  4 B
        uint32_t   materialIndex;   //  4 B
        uint64_t   _unused;         //  8 B (placeholder for D3D12 materialCBAddr)
    };
    static_assert(sizeof(GPUObjectData) == 96, "GPUObjectData must be 96 bytes");

    // Material scalar factors for bindless rendering
    struct MaterialFactors
    {
        float albedoR, albedoG, albedoB, albedoA;
        float metallicFactor;
        float roughnessFactor;
        float _pad[2];
    };
    static_assert(sizeof(MaterialFactors) == 32, "MaterialFactors must be 32 bytes");

    // Push constants for cull shader
    struct CullConstants
    {
        float    frustumPlanes[6][4];  // 96 B
        uint32_t objectCount;          //  4 B
        uint32_t enableHiZ;            //  4 B
        uint32_t hizMipCount;          //  4 B
        uint32_t _pad0;                //  4 B
        float    projParams[4];        // 16 B  (m11, m22, m33, m43)
    };
    static_assert(sizeof(CullConstants) == 128, "CullConstants must be 128 bytes");

    struct CreateInfo
    {
        VulkanCore*  core           = nullptr;
        VkRenderPass gbRenderPassLoad = VK_NULL_HANDLE;  // G-buffer pass with LOAD_OP_LOAD
        VkSampler    linearSampler  = VK_NULL_HANDLE;    // for bindless textures
        const std::vector<MaterialData>* materials = nullptr;  // material data for bindless textures
    };

    VulkanGPUDriven() = default;
    ~VulkanGPUDriven();

    // Non-copyable
    VulkanGPUDriven(const VulkanGPUDriven&) = delete;
    VulkanGPUDriven& operator=(const VulkanGPUDriven&) = delete;

    /**
     * @brief Create indirect resources (SSBOs, pipelines, async compute).
     * @note Must be called AFTER BuildMergedGeometry and scene meshes are ready.
     */
    bool Create(const CreateInfo& info);
    void Destroy();

    /**
     * @brief Build merged vertex/index buffers from scene meshes.
     * @param allVerts Per-mesh vertex arrays
     * @param allIdxs Per-mesh index arrays
     * @param rtSupported If true, add RT usage flags for BLAS building
     */
    void BuildMergedGeometry(
        const std::vector<std::vector<PBRVertex>>& allVerts,
        const std::vector<std::vector<uint32_t>>& allIdxs,
        bool rtSupported);

    // === Instance Recording ===

    void RecordInstance(const GPUObjectData& data);
    void ClearInstances();
    uint32_t GetInstanceCount() const { return (uint32_t)_cpuInstances.size(); }

    // === ViewProj UBO ===

    void UpdateViewProj(const XMFLOAT4X4& view, const XMFLOAT4X4& proj);

    // === Culling ===

    /**
     * @brief Dispatch GPU cull on graphics queue (synchronous path).
     *
     * Assumes command buffer is in recording state outside a render pass.
     * Includes barriers before/after dispatch.
     */
    void DispatchCullSync(
        VkCommandBuffer cmd,
        uint32_t frameIndex,
        const CullConstants& cullConst);

    /**
     * @brief Dispatch GPU cull on async compute queue.
     *
     * Records + submits compute command buffer, signals semaphore + fence.
     * Call WaitForComputeFence() on next frame to ensure completion.
     */
    void DispatchCullAsync(
        uint32_t frameIndex,
        const CullConstants& cullConst);

    /**
     * @brief Wait for async compute fence (CPU-side).
     */
    void WaitForComputeFence(uint32_t frameIndex);

    /**
     * @brief Record queue ownership acquire barriers (after async dispatch).
     */
    void RecordAcquireBarriers(VkCommandBuffer cmd, uint32_t frameIndex);

    // === Indirect Draw ===

    /**
     * @brief Bind pipeline + descriptors + merged VB/IB, issue indirect draw.
     *
     * Assumes render pass is active (with LOAD_OP_LOAD variant).
     */
    void DrawIndirect(VkCommandBuffer cmd, uint32_t frameIndex);

    // === Accessors ===

    bool         IsReady()               const { return _ready; }
    bool         IsAsyncComputeReady()   const { return _asyncComputeReady; }
    VkSemaphore  GetComputeDoneSemaphore(uint32_t frameIndex) const;
    VkBuffer     GetMergedVB()           const { return _mergedVB; }
    VkBuffer     GetMergedIB()           const { return _mergedIB; }

    // For Hi-Z descriptor updates (VulkanBackend reads these to write cull desc)
    VkDescriptorSet GetCullDescSet(uint32_t frameIndex) const { return _cullDescSet[frameIndex]; }

private:
    bool CreateIndirectResources(const std::vector<MaterialData>& materials);
    bool CreateAsyncComputeResources();
    void DestroyAsyncComputeResources();
    void DestroyMergedGeometry();

    // Fill CullConstants from view/proj matrices (Gribb-Hartmann extraction)
    static void BuildFrustumPlanes(const XMMATRIX& VP, CullConstants& out);

    VulkanCore* _core = nullptr;
    bool _ready = false;

    // --- Merged Geometry ---
    VkBuffer       _mergedVB    = VK_NULL_HANDLE;
    VkDeviceMemory _mergedVBMem = VK_NULL_HANDLE;
    VkBuffer       _mergedIB    = VK_NULL_HANDLE;
    VkDeviceMemory _mergedIBMem = VK_NULL_HANDLE;
    VkBuffer       _meshInfoBuf = VK_NULL_HANDLE;
    VkDeviceMemory _meshInfoMem = VK_NULL_HANDLE;
    bool _geometryBuilt = false;

    // --- CPU Instance Buffer (cleared each frame via ClearInstances) ---
    std::vector<GPUObjectData> _cpuInstances;

    // --- Object Data SSBO (HOST_VISIBLE, persistently mapped) ---
    VkBuffer       _objectDataBuffer = VK_NULL_HANDLE;
    VkDeviceMemory _objectDataMem    = VK_NULL_HANDLE;
    void*          _objectDataMapped = nullptr;

    // --- Per-Frame Indirect Args + Draw Count ---
    VkBuffer       _indirectArgBuffer[FRAMES_IN_FLIGHT] = {};
    VkDeviceMemory _indirectArgMem[FRAMES_IN_FLIGHT]    = {};
    VkBuffer       _drawCountBuffer[FRAMES_IN_FLIGHT]   = {};
    VkDeviceMemory _drawCountMem[FRAMES_IN_FLIGHT]      = {};

    // --- Material Factor SSBO ---
    VkBuffer       _matFactorBuffer = VK_NULL_HANDLE;
    VkDeviceMemory _matFactorMem    = VK_NULL_HANDLE;

    // --- Bindless Material Descriptor Set (set=1) ---
    VkDescriptorSetLayout _materialLayout  = VK_NULL_HANDLE;
    VkDescriptorPool      _materialPool    = VK_NULL_HANDLE;
    VkDescriptorSet       _materialDescSet = VK_NULL_HANDLE;
    VkSampler             _linearSampler   = VK_NULL_HANDLE;  // external ref

    // --- Indirect VS Descriptor Set (set=0: ViewProj UBO + ObjectData SSBO) ---
    VkDescriptorSetLayout _vsLayout      = VK_NULL_HANDLE;
    VkDescriptorPool      _vsPool        = VK_NULL_HANDLE;
    VkDescriptorSet       _vsDescSet     = VK_NULL_HANDLE;
    VkBuffer              _viewProjBuf   = VK_NULL_HANDLE;
    VkDeviceMemory        _viewProjMem   = VK_NULL_HANDLE;
    void*                 _viewProjMapped = nullptr;

    // --- Indirect G-Buffer Pipeline ---
    VkPipelineLayout _indirectPipeLayout   = VK_NULL_HANDLE;
    VkPipeline       _indirectGBufPipeline = VK_NULL_HANDLE;
    VkRenderPass     _gbRenderPassLoad     = VK_NULL_HANDLE;  // external ref

    // --- Cull Compute Pipeline ---
    VkDescriptorSetLayout _cullDescLayout = VK_NULL_HANDLE;
    VkDescriptorPool      _cullDescPool   = VK_NULL_HANDLE;
    VkDescriptorSet       _cullDescSet[FRAMES_IN_FLIGHT] = {};
    VkPipelineLayout      _cullPipeLayout = VK_NULL_HANDLE;
    VkPipeline            _cullPipeline   = VK_NULL_HANDLE;

    // --- Async Compute Resources ---
    bool _asyncComputeReady = false;

    struct ComputeFrameResource
    {
        VkCommandPool   cmdPool       = VK_NULL_HANDLE;
        VkCommandBuffer cmdBuffer     = VK_NULL_HANDLE;
        VkSemaphore     doneSemaphore = VK_NULL_HANDLE;
        VkFence         fence         = VK_NULL_HANDLE;
    };
    ComputeFrameResource _computeFrames[FRAMES_IN_FLIGHT];
};

} // namespace Luna

