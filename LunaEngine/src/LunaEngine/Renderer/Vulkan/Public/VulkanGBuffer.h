#pragma once

#include <vulkan/vulkan.h>

namespace Luna
{

class VulkanCore;

/**
 * @brief G-buffer images (albedo, normal, metalrough) and render passes.
 *
 * Owns the 3 colour attachments and both render pass variants:
 * - CLEAR pass: standard geometry fill (LOAD_OP_CLEAR)
 * - LOAD pass: re-open after GPU cull (LOAD_OP_LOAD)
 *
 * Does NOT own the deferred lighting pipeline — that stays in VulkanBackend
 * because descriptor sets reference CSM, SSAO, IBL, and RT shadow resources.
 *
 * Thread Safety: NOT thread-safe. Call from main render thread only.
 */
class VulkanGBuffer
{
public:
    struct CreateInfo
    {
        VulkanCore*  core      = nullptr;
        VkExtent2D   extent    = {};
        VkImageView  depthView = VK_NULL_HANDLE;  // external depth buffer
        VkFormat     depthFormat = VK_FORMAT_D32_SFLOAT;
    };

    VulkanGBuffer() = default;
    ~VulkanGBuffer();

    // Non-copyable
    VulkanGBuffer(const VulkanGBuffer&) = delete;
    VulkanGBuffer& operator=(const VulkanGBuffer&) = delete;

    bool Create(const CreateInfo& info);
    void Destroy();
    bool Resize(VkExtent2D extent, VkImageView depthView);

    // === Accessors ===

    VkImage      GetAlbedoImage()     const { return _albedoImage; }
    VkImage      GetNormalImage()     const { return _normalImage; }
    VkImage      GetMetalRoughImage() const { return _metalRoughImage; }

    VkImageView  GetAlbedoView()     const { return _albedoView; }
    VkImageView  GetNormalView()     const { return _normalView; }
    VkImageView  GetMetalRoughView() const { return _metalRoughView; }

    VkRenderPass  GetRenderPass()     const { return _renderPass; }
    VkRenderPass  GetRenderPassLoad() const { return _renderPassLoad; }

    VkFramebuffer GetFramebuffer()     const { return _framebuffer; }
    VkFramebuffer GetFramebufferLoad() const { return _framebufferLoad; }

    VkExtent2D    GetExtent() const { return _extent; }

private:
    bool CreateImages();
    bool CreateRenderPasses();
    bool CreateFramebuffers();

    VulkanCore* _core = nullptr;
    VkExtent2D  _extent = {};
    VkImageView _externalDepthView = VK_NULL_HANDLE;
    VkFormat    _depthFormat = VK_FORMAT_D32_SFLOAT;

    // G-buffer images
    VkImage        _albedoImage     = VK_NULL_HANDLE;
    VkDeviceMemory _albedoMemory    = VK_NULL_HANDLE;
    VkImageView    _albedoView      = VK_NULL_HANDLE;

    VkImage        _normalImage     = VK_NULL_HANDLE;
    VkDeviceMemory _normalMemory    = VK_NULL_HANDLE;
    VkImageView    _normalView      = VK_NULL_HANDLE;

    VkImage        _metalRoughImage = VK_NULL_HANDLE;
    VkDeviceMemory _metalRoughMemory= VK_NULL_HANDLE;
    VkImageView    _metalRoughView  = VK_NULL_HANDLE;

    // Render passes
    VkRenderPass _renderPass     = VK_NULL_HANDLE;  // CLEAR variant
    VkRenderPass _renderPassLoad = VK_NULL_HANDLE;  // LOAD variant

    // Framebuffers (reference external depth view)
    VkFramebuffer _framebuffer     = VK_NULL_HANDLE;
    VkFramebuffer _framebufferLoad = VK_NULL_HANDLE;
};

} // namespace Luna

