#include "LunaPCH.h"
#include "LunaEngine/Renderer/Vulkan/Public/VulkanGBuffer.h"
#include "LunaEngine/Renderer/Vulkan/Public/VulkanCore.h"
#include "Logger/Logger.h"

namespace Luna
{

// ===========================================================================
// Destructor
// ===========================================================================
VulkanGBuffer::~VulkanGBuffer()
{
    Destroy();
}

// ===========================================================================
// Create
// ===========================================================================
bool VulkanGBuffer::Create(const CreateInfo& info)
{
    if (!info.core) { LUNA_LOG_ERROR("VulkanGBuffer: null core"); return false; }
    if (info.extent.width == 0 || info.extent.height == 0) return false;

    _core = info.core;
    _extent = info.extent;
    _externalDepthView = info.depthView;
    _depthFormat = info.depthFormat;

    if (!CreateImages())       { Destroy(); return false; }
    if (!CreateRenderPasses()) { Destroy(); return false; }
    if (!CreateFramebuffers()) { Destroy(); return false; }

    LUNA_LOG_INFO("VulkanGBuffer: created (%ux%u)", _extent.width, _extent.height);
    return true;
}

// ===========================================================================
// Destroy
// ===========================================================================
void VulkanGBuffer::Destroy()
{
    if (!_core) return;
    VkDevice dev = _core->GetDevice();
    if (!dev) return;

    if (_framebufferLoad) { vkDestroyFramebuffer(dev, _framebufferLoad, nullptr); _framebufferLoad = VK_NULL_HANDLE; }
    if (_framebuffer)     { vkDestroyFramebuffer(dev, _framebuffer, nullptr);     _framebuffer     = VK_NULL_HANDLE; }

    if (_renderPassLoad) { vkDestroyRenderPass(dev, _renderPassLoad, nullptr); _renderPassLoad = VK_NULL_HANDLE; }
    if (_renderPass)     { vkDestroyRenderPass(dev, _renderPass, nullptr);     _renderPass     = VK_NULL_HANDLE; }

    auto dt = [&](VkImageView& v, VkImage& i, VkDeviceMemory& m) {
        if (v) { vkDestroyImageView(dev, v, nullptr); v = VK_NULL_HANDLE; }
        if (i) { vkDestroyImage(dev, i, nullptr);     i = VK_NULL_HANDLE; }
        if (m) { vkFreeMemory(dev, m, nullptr);        m = VK_NULL_HANDLE; }
    };
    dt(_albedoView,     _albedoImage,     _albedoMemory);
    dt(_normalView,     _normalImage,     _normalMemory);
    dt(_metalRoughView, _metalRoughImage, _metalRoughMemory);

    _extent = {};
}

// ===========================================================================
// Resize
// ===========================================================================
bool VulkanGBuffer::Resize(VkExtent2D extent, VkImageView depthView)
{
    VulkanCore* core = _core;
    VkFormat depthFmt = _depthFormat;
    Destroy();

    CreateInfo info;
    info.core = core;
    info.extent = extent;
    info.depthView = depthView;
    info.depthFormat = depthFmt;
    return Create(info);
}

// ===========================================================================
// CreateImages
// ===========================================================================
bool VulkanGBuffer::CreateImages()
{
    uint32_t W = _extent.width, H = _extent.height;

    auto mkGB = [&](VkFormat fmt, VkImage& img, VkDeviceMemory& mem, VkImageView& view) -> bool {
        if (!_core->CreateImage(W, H, fmt, VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, img, mem))
            return false;
        view = _core->CreateImageView(img, fmt, VK_IMAGE_ASPECT_COLOR_BIT);
        return view != VK_NULL_HANDLE;
    };

    if (!mkGB(VK_FORMAT_R8G8B8A8_UNORM,      _albedoImage,     _albedoMemory,     _albedoView))     return false;
    if (!mkGB(VK_FORMAT_R16G16B16A16_SFLOAT,  _normalImage,     _normalMemory,     _normalView))     return false;
    if (!mkGB(VK_FORMAT_R8G8B8A8_UNORM,       _metalRoughImage, _metalRoughMemory, _metalRoughView)) return false;

    return true;
}

// ===========================================================================
// CreateRenderPasses
// ===========================================================================
bool VulkanGBuffer::CreateRenderPasses()
{
    VkDevice dev = _core->GetDevice();

    // --- CLEAR render pass ---
    {
        VkAttachmentDescription atts[4]{};

        // att[0]: albedo (RGBA8_UNORM)
        atts[0].format         = VK_FORMAT_R8G8B8A8_UNORM;
        atts[0].samples        = VK_SAMPLE_COUNT_1_BIT;
        atts[0].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        atts[0].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        atts[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[0].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        atts[0].finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        // att[1]: normal (RGBA16F)
        atts[1]        = atts[0];
        atts[1].format = VK_FORMAT_R16G16B16A16_SFLOAT;

        // att[2]: metalRough (RGBA8_UNORM)
        atts[2]        = atts[0];
        atts[2].format = VK_FORMAT_R8G8B8A8_UNORM;

        // att[3]: depth
        atts[3].format         = _depthFormat;
        atts[3].samples        = VK_SAMPLE_COUNT_1_BIT;
        atts[3].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        atts[3].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        atts[3].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[3].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[3].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        atts[3].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        VkAttachmentReference cr[3]{
            { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
            { 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
            { 2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
        };
        VkAttachmentReference dr{ 3, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };

        VkSubpassDescription sp{};
        sp.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sp.colorAttachmentCount    = 3;
        sp.pColorAttachments       = cr;
        sp.pDepthStencilAttachment = &dr;

        VkRenderPassCreateInfo rpi{};
        rpi.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpi.attachmentCount = 4;
        rpi.pAttachments    = atts;
        rpi.subpassCount    = 1;
        rpi.pSubpasses      = &sp;

        if (vkCreateRenderPass(dev, &rpi, nullptr, &_renderPass) != VK_SUCCESS)
        {
            LUNA_LOG_ERROR("VulkanGBuffer: failed to create CLEAR render pass");
            return false;
        }
    }

    // --- LOAD render pass (re-open after GPU cull) ---
    {
        VkAttachmentDescription atts[4]{};

        atts[0].format         = VK_FORMAT_R8G8B8A8_UNORM;
        atts[0].samples        = VK_SAMPLE_COUNT_1_BIT;
        atts[0].loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
        atts[0].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        atts[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[0].initialLayout  = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        atts[0].finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        atts[1]        = atts[0];
        atts[1].format = VK_FORMAT_R16G16B16A16_SFLOAT;

        atts[2]        = atts[0];
        atts[2].format = VK_FORMAT_R8G8B8A8_UNORM;

        atts[3].format         = _depthFormat;
        atts[3].samples        = VK_SAMPLE_COUNT_1_BIT;
        atts[3].loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
        atts[3].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        atts[3].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[3].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[3].initialLayout  = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        atts[3].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        VkAttachmentReference cr[3]{
            { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
            { 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
            { 2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
        };
        VkAttachmentReference dr{ 3, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };

        VkSubpassDescription sp{};
        sp.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sp.colorAttachmentCount    = 3;
        sp.pColorAttachments       = cr;
        sp.pDepthStencilAttachment = &dr;

        VkRenderPassCreateInfo rpi{};
        rpi.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpi.attachmentCount = 4;
        rpi.pAttachments    = atts;
        rpi.subpassCount    = 1;
        rpi.pSubpasses      = &sp;

        if (vkCreateRenderPass(dev, &rpi, nullptr, &_renderPassLoad) != VK_SUCCESS)
        {
            LUNA_LOG_ERROR("VulkanGBuffer: failed to create LOAD render pass");
            return false;
        }
    }

    return true;
}

// ===========================================================================
// CreateFramebuffers
// ===========================================================================
bool VulkanGBuffer::CreateFramebuffers()
{
    VkDevice dev = _core->GetDevice();

    if (!_externalDepthView)
    {
        LUNA_LOG_ERROR("VulkanGBuffer: null external depth view");
        return false;
    }

    VkImageView atts[4] = { _albedoView, _normalView, _metalRoughView, _externalDepthView };

    VkFramebufferCreateInfo fi{};
    fi.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fi.renderPass      = _renderPass;
    fi.attachmentCount = 4;
    fi.pAttachments    = atts;
    fi.width  = _extent.width;
    fi.height = _extent.height;
    fi.layers = 1;

    if (vkCreateFramebuffer(dev, &fi, nullptr, &_framebuffer) != VK_SUCCESS)
    {
        LUNA_LOG_ERROR("VulkanGBuffer: failed to create CLEAR framebuffer");
        return false;
    }

    fi.renderPass = _renderPassLoad;
    if (vkCreateFramebuffer(dev, &fi, nullptr, &_framebufferLoad) != VK_SUCCESS)
    {
        LUNA_LOG_ERROR("VulkanGBuffer: failed to create LOAD framebuffer");
        return false;
    }

    return true;
}

} // namespace Luna

