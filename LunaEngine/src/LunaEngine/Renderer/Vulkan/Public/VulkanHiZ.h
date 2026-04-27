#pragma once

#include <vulkan/vulkan.h>

namespace Luna
{

class VulkanCore;

/**
 * @brief Hi-Z pyramid for occlusion culling.
 *
 * Creates a full-resolution R32_SFLOAT mip chain from the depth buffer.
 * Mip 0 = 1:1 copy from depth, mips 1+ = 2×2 min downsample via compute.
 *
 * Thread Safety: NOT thread-safe. Call from main render thread only.
 */
class VulkanHiZ
{
public:
    static constexpr uint32_t MAX_MIPS = 13;

    struct CreateInfo
    {
        VulkanCore* core      = nullptr;
        VkExtent2D  extent    = {};       // swapchain resolution
        VkImageView depthView = VK_NULL_HANDLE;  // depth buffer view for mip-0 descriptor
    };

    VulkanHiZ() = default;
    ~VulkanHiZ();

    // Non-copyable
    VulkanHiZ(const VulkanHiZ&) = delete;
    VulkanHiZ& operator=(const VulkanHiZ&) = delete;

    /**
     * @brief Create Hi-Z image, views, sampler, descriptor sets, compute pipeline, and params UBO.
     */
    bool Create(const CreateInfo& info);

    /**
     * @brief Destroy all Hi-Z resources.
     */
    void Destroy();

    /**
     * @brief Recreate after resize. Calls Destroy() then Create().
     */
    bool Resize(VkExtent2D extent, VkImageView depthView);

    /**
     * @brief Build the mip chain from depth buffer via compute dispatches.
     * 
     * Transitions depth image: ATTACHMENT_OPTIMAL → READ_ONLY → ATTACHMENT_OPTIMAL.
     * Caller is responsible for GPU profiler timestamps.
     *
     * @param cmd Active command buffer
     * @param depthImage Depth image handle (for layout transitions)
     * @param extent Swapchain extent (source resolution)
     */
    void BuildPyramid(VkCommandBuffer cmd, VkImage depthImage, VkExtent2D extent);

    // === Accessors ===

    bool         IsReady()        const { return _ready; }
    uint32_t     GetMipCount()    const { return _mipCount; }
    VkImageView  GetFullView()    const { return _fullView; }
    VkSampler    GetSampler()     const { return _sampler; }
    VkBuffer     GetParamsBuffer() const { return _paramsBuffer; }
    void*        GetParamsMapped() const { return _paramsMapped; }

private:
    bool CreateImage(VkExtent2D extent);
    bool CreateViews();
    bool CreateSampler();
    bool CreateDescriptors(VkImageView depthView);
    bool CreatePipeline();
    bool CreateParamsUBO();

    VulkanCore* _core = nullptr;

    // Hi-Z image (R32_SFLOAT, multi-mip)
    VkImage        _image      = VK_NULL_HANDLE;
    VkDeviceMemory _memory     = VK_NULL_HANDLE;
    VkImageView    _mipView[MAX_MIPS] = {};  // per-mip views for compute read/write
    VkImageView    _fullView   = VK_NULL_HANDLE;  // all-mip view for cull shader sampling
    uint32_t       _mipCount   = 0;
    bool           _ready      = false;

    // Sampler (point-clamp)
    VkSampler _sampler = VK_NULL_HANDLE;

    // Compute pipeline for mip generation
    VkDescriptorSetLayout _descLayout = VK_NULL_HANDLE;
    VkDescriptorPool      _descPool   = VK_NULL_HANDLE;
    VkDescriptorSet       _descSet[MAX_MIPS] = {};
    VkPipelineLayout      _pipeLayout = VK_NULL_HANDLE;
    VkPipeline            _pipeline   = VK_NULL_HANDLE;

    // Params UBO for cull shader (viewProj + screenSize)
    VkBuffer       _paramsBuffer = VK_NULL_HANDLE;
    VkDeviceMemory _paramsMem    = VK_NULL_HANDLE;
    void*          _paramsMapped = nullptr;
};

} // namespace Luna

