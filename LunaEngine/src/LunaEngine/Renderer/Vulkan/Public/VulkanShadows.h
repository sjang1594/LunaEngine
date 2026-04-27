#pragma once

#include <vulkan/vulkan.h>
#include <DirectXMath.h>
#include <vector>

using namespace DirectX;

namespace Luna
{

class VulkanCore;

/**
 * @brief Cascaded Shadow Maps (CSM) — 4 cascades, configurable resolution.
 *
 * Owns the shadow map image (D32_SFLOAT, 2D array), render pass, depth-only
 * pipeline, and per-cascade framebuffers. Computes cascade splits and light
 * view-projection matrices.
 *
 * Thread Safety: NOT thread-safe. Call from main render thread only.
 */
class VulkanShadows
{
public:
    static constexpr uint32_t CASCADE_COUNT = 4;
    static constexpr uint32_t SHADOW_SIZE   = 2048;

    struct CreateInfo
    {
        VulkanCore* core = nullptr;
    };

    /** Per-mesh draw data for shadow pass. */
    struct ShadowDraw
    {
        VkBuffer     vertexBuffer;
        VkBuffer     indexBuffer;
        uint32_t     indexCount;
        XMFLOAT4X4   model;
    };

    VulkanShadows() = default;
    ~VulkanShadows();

    // Non-copyable
    VulkanShadows(const VulkanShadows&) = delete;
    VulkanShadows& operator=(const VulkanShadows&) = delete;

    bool Create(const CreateInfo& info);
    void Destroy();

    /**
     * @brief Compute cascade splits and per-cascade light VP matrices.
     */
    void UpdateMatrices(const XMFLOAT4X4& view, const XMFLOAT4X4& proj);

    /**
     * @brief Render all meshes into the 4-cascade shadow map.
     */
    void DrawPass(VkCommandBuffer cmd, const std::vector<ShadowDraw>& draws);

    // === Accessors ===

    VkImageView        GetArrayView()          const { return _arrayView; }
    VkSampler          GetSampler()            const { return _sampler; }
    const XMFLOAT4X4*  GetLightVPs()           const { return _lightVP; }
    const float*       GetCascadeSplits()       const { return _splits; }

private:
    bool CreateImage();
    bool CreateViews();
    bool CreateRenderPass();
    bool CreateFramebuffers();
    bool CreateSampler();
    bool CreatePipeline();

    VulkanCore* _core = nullptr;

    // Shadow map image (D32_SFLOAT, 2D array, CASCADE_COUNT layers)
    VkImage        _image     = VK_NULL_HANDLE;
    VkDeviceMemory _memory    = VK_NULL_HANDLE;
    VkImageView    _layerView[CASCADE_COUNT] = {};  // per-layer for framebuffers
    VkImageView    _arrayView = VK_NULL_HANDLE;      // full array for deferred sampling

    // Render pass + framebuffers
    VkRenderPass  _renderPass = VK_NULL_HANDLE;
    VkFramebuffer _framebuffers[CASCADE_COUNT] = {};

    // Pipeline
    VkPipelineLayout _pipeLayout = VK_NULL_HANDLE;
    VkPipeline       _pipeline   = VK_NULL_HANDLE;

    // Sampler (point-clamp)
    VkSampler _sampler = VK_NULL_HANDLE;

    // Cascade matrices
    XMFLOAT4X4 _lightVP[CASCADE_COUNT] = {};
    float      _splits[CASCADE_COUNT]  = {};
};

} // namespace Luna

