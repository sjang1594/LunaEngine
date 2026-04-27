#include "LunaPCH.h"
#include "LunaEngine/Renderer/Vulkan/Public/VulkanSwapchain.h"
#include "LunaEngine/Renderer/Vulkan/Public/VulkanCore.h"
#include "LunaEngine/Renderer/Vulkan/Public/VulkanDevice.h"
#include "Logger/Logger.h"

#include <algorithm>

namespace Luna
{

// ===========================================================================
// Destructor
// ===========================================================================
VulkanSwapchain::~VulkanSwapchain()
{
    Destroy();
}

// ===========================================================================
// Create
// ===========================================================================
bool VulkanSwapchain::Create(const CreateInfo& info)
{
    if (!info.core || info.surface == VK_NULL_HANDLE)
    {
        LUNA_LOG_ERROR("VulkanSwapchain: invalid CreateInfo");
        return false;
    }

    _core    = info.core;
    _surface = info.surface;
    _vsync   = info.vsync;
    _pendingWidth  = info.width;
    _pendingHeight = info.height;

    // Select depth format with fallback
    _depthFormat = SelectDepthFormat();
    if (_depthFormat == VK_FORMAT_UNDEFINED)
    {
        LUNA_LOG_ERROR("VulkanSwapchain: no suitable depth format found");
        return false;
    }

    if (!CreateSwapchain())      return false;
    if (!CreateDepthResources()) return false;
    if (!CreateRenderPass())     return false;
    if (!CreateFramebuffers())   return false;

    LUNA_LOG_INFO("VulkanSwapchain: created %ux%u, %u images, depth=%d",
                  _extent.width, _extent.height, GetImageCount(), _depthFormat);
    return true;
}

// ===========================================================================
// Destroy
// ===========================================================================
void VulkanSwapchain::Destroy()
{
    if (!_core) return;

    VkDevice dev = _core->GetDevice();
    if (!dev) return;

    vkDeviceWaitIdle(dev);

    // Framebuffers
    for (auto fb : _framebuffers)
        if (fb) vkDestroyFramebuffer(dev, fb, nullptr);
    _framebuffers.clear();

    // Render pass
    if (_renderPass)
    {
        vkDestroyRenderPass(dev, _renderPass, nullptr);
        _renderPass = VK_NULL_HANDLE;
    }

    DestroyDepthResources();
    DestroySwapchain();

    _core = nullptr;
    LUNA_LOG_INFO("VulkanSwapchain: destroyed");
}

// ===========================================================================
// RequestResize
// ===========================================================================
void VulkanSwapchain::RequestResize(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0) return;  // Minimized
    
    _pendingWidth  = width;
    _pendingHeight = height;
    _needsRecreate = true;
}

// ===========================================================================
// RecreateIfNeeded
// ===========================================================================
bool VulkanSwapchain::RecreateIfNeeded()
{
    if (!_needsRecreate) return true;

    VkDevice dev = _core->GetDevice();
    vkDeviceWaitIdle(dev);

    // Destroy old resources
    for (auto fb : _framebuffers)
        if (fb) vkDestroyFramebuffer(dev, fb, nullptr);
    _framebuffers.clear();

    // Keep render pass — format doesn't change
    DestroyDepthResources();
    DestroySwapchain();

    // Recreate
    if (!CreateSwapchain())
    {
        LUNA_LOG_ERROR("VulkanSwapchain: recreate swapchain failed");
        return false;
    }
    if (!CreateDepthResources())
    {
        LUNA_LOG_ERROR("VulkanSwapchain: recreate depth failed");
        return false;
    }
    if (!CreateFramebuffers())
    {
        LUNA_LOG_ERROR("VulkanSwapchain: recreate framebuffers failed");
        return false;
    }

    _needsRecreate = false;
    LUNA_LOG_INFO("VulkanSwapchain: recreated %ux%u", _extent.width, _extent.height);
    return true;
}

// ===========================================================================
// AcquireNextImage
// ===========================================================================
VkResult VulkanSwapchain::AcquireNextImage(VkSemaphore imageReadySemaphore, uint32_t* outImageIndex)
{
    VkResult result = vkAcquireNextImageKHR(
        _core->GetDevice(),
        _swapchain,
        UINT64_MAX,
        imageReadySemaphore,
        VK_NULL_HANDLE,
        outImageIndex
    );

    if (result == VK_ERROR_OUT_OF_DATE_KHR)
    {
        _needsRecreate = true;
    }
    else if (result == VK_SUBOPTIMAL_KHR)
    {
        // Image acquired but suboptimal — continue but schedule recreate
        _needsRecreate = true;
        result = VK_SUCCESS;  // Treat as success for caller
    }

    return result;
}

// ===========================================================================
// Present
// ===========================================================================
VkResult VulkanSwapchain::Present(VkQueue queue, VkSemaphore waitSemaphore, uint32_t imageIndex)
{
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores    = &waitSemaphore;
    presentInfo.swapchainCount     = 1;
    presentInfo.pSwapchains        = &_swapchain;
    presentInfo.pImageIndices      = &imageIndex;

    VkResult result = vkQueuePresentKHR(queue, &presentInfo);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
    {
        _needsRecreate = true;
    }

    return result;
}

// ===========================================================================
// Accessors
// ===========================================================================
VkImage VulkanSwapchain::GetImage(uint32_t index) const
{
    return (index < _images.size()) ? _images[index] : VK_NULL_HANDLE;
}

VkImageView VulkanSwapchain::GetImageView(uint32_t index) const
{
    return (index < _imageViews.size()) ? _imageViews[index] : VK_NULL_HANDLE;
}

VkFramebuffer VulkanSwapchain::GetFramebuffer(uint32_t index) const
{
    return (index < _framebuffers.size()) ? _framebuffers[index] : VK_NULL_HANDLE;
}

void VulkanSwapchain::SetImageFence(uint32_t imageIndex, VkFence fence)
{
    if (imageIndex < _imagesInFlight.size())
        _imagesInFlight[imageIndex] = fence;
}

VkFence VulkanSwapchain::GetImageFence(uint32_t imageIndex) const
{
    return (imageIndex < _imagesInFlight.size()) ? _imagesInFlight[imageIndex] : VK_NULL_HANDLE;
}

void VulkanSwapchain::SetVSync(bool vsync)
{
    if (_vsync != vsync)
    {
        _vsync = vsync;
        _needsRecreate = true;
    }
}

// ===========================================================================
// CreateSwapchain
// ===========================================================================
bool VulkanSwapchain::CreateSwapchain()
{
    VkDevice         dev = _core->GetDevice();
    VkPhysicalDevice gpu = _core->GetPhysicalDevice();

    // Query surface capabilities
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, _surface, &caps);

    // Select surface format
    VkSurfaceFormatKHR surfaceFormat = SelectSurfaceFormat();
    _format = surfaceFormat.format;

    // Determine extent
    if (caps.currentExtent.width != UINT32_MAX)
    {
        _extent = caps.currentExtent;
    }
    else
    {
        _extent.width  = std::clamp(_pendingWidth,  caps.minImageExtent.width,  caps.maxImageExtent.width);
        _extent.height = std::clamp(_pendingHeight, caps.minImageExtent.height, caps.maxImageExtent.height);
    }

    // Image count: prefer triple buffering
    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount)
        imageCount = caps.maxImageCount;

    // Select present mode
    VkPresentModeKHR presentMode = SelectPresentMode();

    // Create swapchain
    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface          = _surface;
    createInfo.minImageCount    = imageCount;
    createInfo.imageFormat      = surfaceFormat.format;
    createInfo.imageColorSpace  = surfaceFormat.colorSpace;
    createInfo.imageExtent      = _extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                                | VK_IMAGE_USAGE_TRANSFER_SRC_BIT   // For blits/copies
                                | VK_IMAGE_USAGE_TRANSFER_DST_BIT;  // For vkCmdClearColorImage

    // Queue family sharing
    VulkanDevice* vkDev = _core->GetVulkanDevice();
    uint32_t queueFamilies[] = { vkDev->GetGraphicsQueueFamily(), vkDev->GetPresentQueueFamily() };
    if (queueFamilies[0] != queueFamilies[1])
    {
        createInfo.imageSharingMode      = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = 2;
        createInfo.pQueueFamilyIndices   = queueFamilies;
    }
    else
    {
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    createInfo.preTransform   = caps.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode    = presentMode;
    createInfo.clipped        = VK_TRUE;

    if (vkCreateSwapchainKHR(dev, &createInfo, nullptr, &_swapchain) != VK_SUCCESS)
    {
        LUNA_LOG_ERROR("VulkanSwapchain: failed to create swapchain");
        return false;
    }

    // Get swapchain images
    uint32_t actualImageCount = 0;
    vkGetSwapchainImagesKHR(dev, _swapchain, &actualImageCount, nullptr);
    _images.resize(actualImageCount);
    vkGetSwapchainImagesKHR(dev, _swapchain, &actualImageCount, _images.data());

    // Create image views
    _imageViews.resize(actualImageCount);
    for (uint32_t i = 0; i < actualImageCount; ++i)
    {
        _imageViews[i] = _core->CreateImageView(_images[i], _format, VK_IMAGE_ASPECT_COLOR_BIT);
        if (_imageViews[i] == VK_NULL_HANDLE)
        {
            LUNA_LOG_ERROR("VulkanSwapchain: failed to create image view %u", i);
            return false;
        }
    }

    // Initialize image-in-flight fence tracking
    _imagesInFlight.resize(actualImageCount, VK_NULL_HANDLE);

    return true;
}

// ===========================================================================
// DestroySwapchain
// ===========================================================================
void VulkanSwapchain::DestroySwapchain()
{
    VkDevice dev = _core->GetDevice();

    for (auto iv : _imageViews)
        if (iv) vkDestroyImageView(dev, iv, nullptr);
    _imageViews.clear();
    _images.clear();
    _imagesInFlight.clear();

    if (_swapchain)
    {
        vkDestroySwapchainKHR(dev, _swapchain, nullptr);
        _swapchain = VK_NULL_HANDLE;
    }
}

// ===========================================================================
// CreateDepthResources
// ===========================================================================
bool VulkanSwapchain::CreateDepthResources()
{
    bool result = _core->CreateImage(
        _extent.width, _extent.height,
        _depthFormat,
        VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
            | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,  // For Hi-Z pyramid blit
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        _depthImage, _depthMemory
    );

    if (!result)
    {
        LUNA_LOG_ERROR("VulkanSwapchain: failed to create depth image");
        return false;
    }

    _depthView = _core->CreateImageView(_depthImage, _depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT);
    return _depthView != VK_NULL_HANDLE;
}

// ===========================================================================
// DestroyDepthResources
// ===========================================================================
void VulkanSwapchain::DestroyDepthResources()
{
    VkDevice dev = _core->GetDevice();

    if (_depthView)
    {
        vkDestroyImageView(dev, _depthView, nullptr);
        _depthView = VK_NULL_HANDLE;
    }
    if (_depthImage)
    {
        vkDestroyImage(dev, _depthImage, nullptr);
        _depthImage = VK_NULL_HANDLE;
    }
    if (_depthMemory)
    {
        vkFreeMemory(dev, _depthMemory, nullptr);
        _depthMemory = VK_NULL_HANDLE;
    }
}

// ===========================================================================
// CreateRenderPass
// ===========================================================================
bool VulkanSwapchain::CreateRenderPass()
{
    VkAttachmentDescription attachments[2]{};

    // Color attachment — LOAD preserves tonemapped content underneath ImGui overlay
    attachments[0].format         = _format;
    attachments[0].samples        = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[0].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout  = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    attachments[0].finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    // Depth attachment — ImGui doesn't use depth; don't care about contents
    attachments[1].format         = _depthFormat;
    attachments[1].samples        = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthRef{};
    depthRef.attachment = 1;
    depthRef.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount    = 1;
    subpass.pColorAttachments       = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;

    // Subpass dependency for layout transitions
    VkSubpassDependency dependency{};
    dependency.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass    = 0;
    dependency.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                             | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                             | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                             | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo createInfo{};
    createInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    createInfo.attachmentCount = 2;
    createInfo.pAttachments    = attachments;
    createInfo.subpassCount    = 1;
    createInfo.pSubpasses      = &subpass;
    createInfo.dependencyCount = 1;
    createInfo.pDependencies   = &dependency;

    if (vkCreateRenderPass(_core->GetDevice(), &createInfo, nullptr, &_renderPass) != VK_SUCCESS)
    {
        LUNA_LOG_ERROR("VulkanSwapchain: failed to create render pass");
        return false;
    }

    return true;
}

// ===========================================================================
// CreateFramebuffers
// ===========================================================================
bool VulkanSwapchain::CreateFramebuffers()
{
    _framebuffers.resize(_images.size());

    for (size_t i = 0; i < _images.size(); ++i)
    {
        VkImageView attachments[] = { _imageViews[i], _depthView };

        VkFramebufferCreateInfo createInfo{};
        createInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        createInfo.renderPass      = _renderPass;
        createInfo.attachmentCount = 2;
        createInfo.pAttachments    = attachments;
        createInfo.width           = _extent.width;
        createInfo.height          = _extent.height;
        createInfo.layers          = 1;

        if (vkCreateFramebuffer(_core->GetDevice(), &createInfo, nullptr, &_framebuffers[i]) != VK_SUCCESS)
        {
            LUNA_LOG_ERROR("VulkanSwapchain: failed to create framebuffer %zu", i);
            return false;
        }
    }

    return true;
}

// ===========================================================================
// SelectDepthFormat
// ===========================================================================
VkFormat VulkanSwapchain::SelectDepthFormat()
{
    VkPhysicalDevice gpu = _core->GetPhysicalDevice();

    const VkFormat candidates[] = {
        VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_FORMAT_D24_UNORM_S8_UINT
    };

    for (VkFormat format : candidates)
    {
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(gpu, format, &props);

        if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
        {
            return format;
        }
    }

    return VK_FORMAT_UNDEFINED;
}

// ===========================================================================
// SelectPresentMode
// ===========================================================================
VkPresentModeKHR VulkanSwapchain::SelectPresentMode()
{
    VkPhysicalDevice gpu = _core->GetPhysicalDevice();

    // FIFO is always supported (vsync on)
    if (_vsync)
        return VK_PRESENT_MODE_FIFO_KHR;

    // Query available present modes
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, _surface, &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, _surface, &count, modes.data());

    // Prefer MAILBOX (triple-buffer, no tearing)
    for (auto mode : modes)
    {
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR)
            return VK_PRESENT_MODE_MAILBOX_KHR;
    }

    // Fallback to IMMEDIATE (may tear)
    for (auto mode : modes)
    {
        if (mode == VK_PRESENT_MODE_IMMEDIATE_KHR)
            return VK_PRESENT_MODE_IMMEDIATE_KHR;
    }

    // Final fallback
    return VK_PRESENT_MODE_FIFO_KHR;
}

// ===========================================================================
// SelectSurfaceFormat
// ===========================================================================
VkSurfaceFormatKHR VulkanSwapchain::SelectSurfaceFormat()
{
    VkPhysicalDevice gpu = _core->GetPhysicalDevice();

    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, _surface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, _surface, &count, formats.data());

    // Prefer B8G8R8A8_UNORM with SRGB color space
    for (const auto& fmt : formats)
    {
        if (fmt.format == VK_FORMAT_B8G8R8A8_UNORM &&
            fmt.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            return fmt;
        }
    }

    // Return first available
    return formats[0];
}

} // namespace Luna

