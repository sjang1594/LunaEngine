#pragma once

#include <vulkan/vulkan.h>
#include <vector>

namespace Luna
{

class VulkanCore;

/**
 * @brief Manages Vulkan swapchain lifecycle, depth buffer, and present render pass.
 * 
 * Responsibilities:
 * - Swapchain creation/destruction/recreation
 * - Depth buffer management
 * - Present render pass and framebuffers
 * - VSync mode selection
 * - Image-in-flight fence tracking
 * 
 * Thread Safety: NOT thread-safe. Call from main render thread only.
 */
class VulkanSwapchain
{
public:
    struct CreateInfo
    {
        VulkanCore*  core    = nullptr;
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        uint32_t     width   = 0;
        uint32_t     height  = 0;
        bool         vsync   = false;
    };

    VulkanSwapchain() = default;
    ~VulkanSwapchain();

    // Non-copyable
    VulkanSwapchain(const VulkanSwapchain&) = delete;
    VulkanSwapchain& operator=(const VulkanSwapchain&) = delete;

    /**
     * @brief Create swapchain, depth buffer, render pass, and framebuffers.
     */
    bool Create(const CreateInfo& info);

    /**
     * @brief Destroy all swapchain resources.
     */
    void Destroy();

    /**
     * @brief Request deferred resize. Actual recreation happens in RecreateIfNeeded().
     */
    void RequestResize(uint32_t width, uint32_t height);

    /**
     * @brief Recreate swapchain if resize was requested or OUT_OF_DATE occurred.
     * @return true if frame can proceed, false if should skip this frame
     */
    bool RecreateIfNeeded();

    /**
     * @brief Acquire next swapchain image.
     * @param imageReadySemaphore Semaphore to signal when image is ready
     * @param outImageIndex Output: acquired image index
     * @return VK_SUCCESS, VK_SUBOPTIMAL_KHR, or VK_ERROR_OUT_OF_DATE_KHR
     */
    VkResult AcquireNextImage(VkSemaphore imageReadySemaphore, uint32_t* outImageIndex);

    /**
     * @brief Present the rendered image.
     * @param queue Queue to present on (typically graphics/present queue)
     * @param waitSemaphore Semaphore to wait on before present
     * @param imageIndex Image index to present
     * @return VK_SUCCESS or error
     */
    VkResult Present(VkQueue queue, VkSemaphore waitSemaphore, uint32_t imageIndex);

    // === Accessors ===
    
    VkExtent2D     GetExtent()     const { return _extent; }
    VkFormat       GetFormat()     const { return _format; }
    VkFormat       GetDepthFormat() const { return _depthFormat; }
    VkRenderPass   GetRenderPass() const { return _renderPass; }
    VkImageView    GetDepthView()  const { return _depthView; }
    VkImage        GetDepthImage() const { return _depthImage; }
    uint32_t       GetImageCount() const { return static_cast<uint32_t>(_images.size()); }
    
    VkImage        GetImage(uint32_t index) const;
    VkImageView    GetImageView(uint32_t index) const;
    VkFramebuffer  GetFramebuffer(uint32_t index) const;

    /**
     * @brief Track fence associated with a swapchain image.
     * Used to ensure GPU work on an image completes before reuse.
     */
    void    SetImageFence(uint32_t imageIndex, VkFence fence);
    VkFence GetImageFence(uint32_t imageIndex) const;

    bool NeedsRecreate() const { return _needsRecreate; }
    void SetVSync(bool vsync);

private:
    bool CreateSwapchain();
    void DestroySwapchain();
    
    bool CreateDepthResources();
    void DestroyDepthResources();
    
    bool CreateRenderPass();
    bool CreateFramebuffers();
    
    VkFormat SelectDepthFormat();
    VkPresentModeKHR SelectPresentMode();
    VkSurfaceFormatKHR SelectSurfaceFormat();

    // === Core references ===
    VulkanCore*  _core    = nullptr;
    VkSurfaceKHR _surface = VK_NULL_HANDLE;

    // === Swapchain ===
    VkSwapchainKHR _swapchain = VK_NULL_HANDLE;
    VkFormat       _format    = VK_FORMAT_UNDEFINED;
    VkExtent2D     _extent    = {};
    bool           _vsync     = false;

    std::vector<VkImage>     _images;
    std::vector<VkImageView> _imageViews;
    std::vector<VkFence>     _imagesInFlight;  // Per-swapchain-image fence tracking

    // === Depth buffer ===
    VkImage        _depthImage  = VK_NULL_HANDLE;
    VkDeviceMemory _depthMemory = VK_NULL_HANDLE;
    VkImageView    _depthView   = VK_NULL_HANDLE;
    VkFormat       _depthFormat = VK_FORMAT_UNDEFINED;

    // === Render pass + framebuffers ===
    VkRenderPass               _renderPass = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> _framebuffers;

    // === Deferred resize ===
    bool     _needsRecreate = false;
    uint32_t _pendingWidth  = 0;
    uint32_t _pendingHeight = 0;
};

} // namespace Luna

