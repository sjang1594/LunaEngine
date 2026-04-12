#include "LunaPCH.h"
#ifdef LUNA_VULKAN_ENABLED

#include "LunaEngine/Renderer/Vulkan/Public/VulkanBackend.h"
#include "Logger/Logger.h"
#include "Renderer/Vulkan/Public/VulkanDevice.h"

#include <algorithm> // std::clamp

namespace Luna
{

// ---------------------------------------------------------------------------
// Static helper — vkDestroyDebugUtilsMessengerEXT must be loaded at runtime
// ---------------------------------------------------------------------------
static void DestroyDebugMessengerEXT(VkInstance instance,
                                     VkDebugUtilsMessengerEXT messenger,
                                     const VkAllocationCallbacks* pAllocator)
{
    auto func = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
    if (func)
        func(instance, messenger, pAllocator);
}

// ===========================================================================
// Construction / Destruction
// ===========================================================================
VulkanBackend::VulkanBackend() = default;

VulkanBackend::~VulkanBackend()
{
    VulkanBackend::Shutdown();
}

// ===========================================================================
// Init
// ===========================================================================
bool VulkanBackend::Init(void* windowHandler, uint32_t width, uint32_t height)
{
    _width  = width;
    _height = height;

    if (!CreateInstance())           return false;
    SetupDebugMessenger();           // best-effort; non-fatal if extension unavailable
    if (!CreateSurface(windowHandler)) return false;

    _device = std::make_unique<VulkanDevice>();
    if (!_device->Initialize(_instance, _surface)) return false;

    if (!CreateImGuiDescriptorPool()) return false;
    if (!CreateSwapchain(width, height)) return false;
    if (!CreateRenderPass())          return false;
    if (!CreateFramebuffers())        return false;
    if (!CreateFrameResources())      return false;

    InitImGui(windowHandler);
    return true;
}

// ===========================================================================
// Shutdown
// ===========================================================================
void VulkanBackend::Shutdown()
{
    if (!_device || _device->GetDevice() == VK_NULL_HANDLE)
        return;

    vkDeviceWaitIdle(_device->GetDevice());

    ShutdownImGui();

    if (_imguiDescriptorPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(_device->GetDevice(), _imguiDescriptorPool, nullptr);
        _imguiDescriptorPool = VK_NULL_HANDLE;
    }

    for (auto& frame : _frames)
    {
        if (frame.cmdPool    != VK_NULL_HANDLE)
            vkDestroyCommandPool(_device->GetDevice(), frame.cmdPool, nullptr);
        if (frame.fence      != VK_NULL_HANDLE)
            vkDestroyFence(_device->GetDevice(), frame.fence, nullptr);
        if (frame.imageReady != VK_NULL_HANDLE)
            vkDestroySemaphore(_device->GetDevice(), frame.imageReady, nullptr);
        if (frame.renderDone != VK_NULL_HANDLE)
            vkDestroySemaphore(_device->GetDevice(), frame.renderDone, nullptr);
    }

    DestroySwapchain();

    if (_renderPass != VK_NULL_HANDLE)
    {
        vkDestroyRenderPass(_device->GetDevice(), _renderPass, nullptr);
        _renderPass = VK_NULL_HANDLE;
    }

    _device->ShutDown();
    _device.reset();

    if (_surface != VK_NULL_HANDLE)
    {
        vkDestroySurfaceKHR(_instance, _surface, nullptr);
        _surface = VK_NULL_HANDLE;
    }

    if (_debugMessenger != VK_NULL_HANDLE)
    {
        DestroyDebugMessengerEXT(_instance, _debugMessenger, nullptr);
        _debugMessenger = VK_NULL_HANDLE;
    }

    if (_instance != VK_NULL_HANDLE)
    {
        vkDestroyInstance(_instance, nullptr);
        _instance = VK_NULL_HANDLE;
    }
}

// ===========================================================================
// Frame loop
// ===========================================================================
void VulkanBackend::BeginFrame()
{
    _frameActive = false;

    auto&    frame = _frames[_frameIndex];
    VkDevice dev   = _device->GetDevice();

    // Wait for this frame slot to finish on the GPU before reusing it
    vkWaitForFences(dev, 1, &frame.fence, VK_TRUE, UINT64_MAX);

    // Acquire swapchain image — signals frame.imageReady when ready
    VkResult result = vkAcquireNextImageKHR(dev, _swapchain, UINT64_MAX,
                                             frame.imageReady, VK_NULL_HANDLE, &_imageIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR)
    {
        // Swapchain invalidated (resize between frames); recreate and skip this frame
        RecreateSwapchain();
        return;
    }
    // VK_SUBOPTIMAL_KHR: still usable — continue, recreate at EndFrame

    vkResetFences(dev, 1, &frame.fence);

    // Record commands
    vkResetCommandBuffer(frame.cmdBuffer, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(frame.cmdBuffer, &beginInfo);

    // Begin render pass — clears to dark grey
    VkClearValue clearColor = {{{ 0.1f, 0.1f, 0.1f, 1.0f }}};

    VkRenderPassBeginInfo rpInfo{};
    rpInfo.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpInfo.renderPass        = _renderPass;
    rpInfo.framebuffer       = _framebuffers[_imageIndex];
    rpInfo.renderArea.offset = { 0, 0 };
    rpInfo.renderArea.extent = _swapchainExtent;
    rpInfo.clearValueCount   = 1;
    rpInfo.pClearValues      = &clearColor;

    vkCmdBeginRenderPass(frame.cmdBuffer, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

    // Dynamic viewport and scissor
    VkViewport viewport{};
    viewport.x        = 0.0f;
    viewport.y        = 0.0f;
    viewport.width    = static_cast<float>(_swapchainExtent.width);
    viewport.height   = static_cast<float>(_swapchainExtent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(frame.cmdBuffer, 0, 1, &viewport);

    VkRect2D scissor{ { 0, 0 }, _swapchainExtent };
    vkCmdSetScissor(frame.cmdBuffer, 0, 1, &scissor);

    _frameActive = true;
}

void VulkanBackend::DrawFrame()
{
    // Geometry draw calls go here once VulkanPipeline + MeshRenderer are wired.
    // For now the render pass stays open; ImGui is drawn in RenderImGui().
}

void VulkanBackend::EndFrame()
{
    if (!_frameActive)
        return;

    auto& frame = _frames[_frameIndex];

    vkCmdEndRenderPass(frame.cmdBuffer);
    vkEndCommandBuffer(frame.cmdBuffer);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSubmitInfo submitInfo{};
    submitInfo.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount   = 1;
    submitInfo.pWaitSemaphores      = &frame.imageReady;
    submitInfo.pWaitDstStageMask    = &waitStage;
    submitInfo.commandBufferCount   = 1;
    submitInfo.pCommandBuffers      = &frame.cmdBuffer;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores    = &frame.renderDone;

    vkQueueSubmit(_device->GetGraphicsQueue(), 1, &submitInfo, frame.fence);

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores    = &frame.renderDone;
    presentInfo.swapchainCount     = 1;
    presentInfo.pSwapchains        = &_swapchain;
    presentInfo.pImageIndices      = &_imageIndex;

    VkResult result = vkQueuePresentKHR(_device->GetPresentQueue(), &presentInfo);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
    {
        vkDeviceWaitIdle(_device->GetDevice());
        RecreateSwapchain();
    }

    _frameIndex = (_frameIndex + 1) % FRAMES_IN_FLIGHT;
}

// ===========================================================================
// ImGui
// ===========================================================================
void VulkanBackend::InitImGui(void* windowHandler)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForVulkan(static_cast<GLFWwindow*>(windowHandler), true);

    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.Instance        = _instance;
    initInfo.PhysicalDevice  = _device->GetPhysicalDevice();
    initInfo.Device          = _device->GetDevice();
    initInfo.Queue           = _device->GetGraphicsQueue();
    initInfo.QueueFamily     = _device->GetGraphicsQueueFamily();
    initInfo.PipelineCache   = VK_NULL_HANDLE;
    initInfo.DescriptorPool  = _imguiDescriptorPool;
    initInfo.Subpass         = 0;
    initInfo.MinImageCount   = 2;
    initInfo.ImageCount      = static_cast<uint32_t>(_swapchainImages.size());
    initInfo.MSAASamples     = VK_SAMPLE_COUNT_1_BIT;
    initInfo.CheckVkResultFn = [](VkResult err) {
        if (err != VK_SUCCESS)
            LUNA_LOG_ERROR("ImGui Vulkan error: VkResult %d", static_cast<int>(err));
    };

    ImGui_ImplVulkan_Init(&initInfo, _renderPass);

    // Upload font atlas using a one-shot command buffer on frame-slot 0
    vkResetCommandPool(_device->GetDevice(), _frames[0].cmdPool, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(_frames[0].cmdBuffer, &beginInfo);

    vkEndCommandBuffer(_frames[0].cmdBuffer);

    VkSubmitInfo submit{};
    submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers    = &_frames[0].cmdBuffer;
    vkQueueSubmit(_device->GetGraphicsQueue(), 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(_device->GetGraphicsQueue());

    ImGui_ImplVulkan_CreateFontsTexture();
}

void VulkanBackend::StartImGui()
{
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void VulkanBackend::RenderImGui()
{
    ImGui::Render();
    if (!_frameActive)
        return; // frame was skipped due to swapchain recreation — don't submit draw data
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), _frames[_frameIndex].cmdBuffer);
}

void VulkanBackend::ShutdownImGui()
{
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

// ===========================================================================
// Resize
// ===========================================================================
void VulkanBackend::Resize(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0)
        return;

    _width  = width;
    _height = height;

    vkDeviceWaitIdle(_device->GetDevice());
    RecreateSwapchain();
    ImGui_ImplVulkan_SetMinImageCount(2);
}

// ===========================================================================
// Draw call stubs (wired up once VulkanPipeline exists)
// ===========================================================================
void VulkanBackend::Draw(uint32_t /*vertexCount*/)  {}
void VulkanBackend::SetVertexBuffer(class IBuffer*) {}
void VulkanBackend::BindPipeline(class IPipeline*)  {}

// ===========================================================================
// Private — Instance
// ===========================================================================
bool VulkanBackend::CreateInstance()
{
    uint32_t    glfwExtCount = 0;
    const char** glfwExt    = glfwGetRequiredInstanceExtensions(&glfwExtCount);
    if (!glfwExt)
    {
        LUNA_LOG_ERROR("glfwGetRequiredInstanceExtensions returned null — Vulkan may not be supported");
        return false;
    }

    std::vector<const char*> extensions(glfwExt, glfwExt + glfwExtCount);

#if defined(_DEBUG)
    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    const char* validationLayer = "VK_LAYER_KHRONOS_validation";
#endif

    VkApplicationInfo appInfo{};
    appInfo.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName   = "LunaEngine";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName        = "Luna";
    appInfo.engineVersion      = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion         = VK_API_VERSION_1_3;

    VkInstanceCreateInfo createInfo{};
    createInfo.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo        = &appInfo;
    createInfo.enabledExtensionCount   = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();
#if defined(_DEBUG)
    createInfo.enabledLayerCount   = 1;
    createInfo.ppEnabledLayerNames = &validationLayer;
#else
    createInfo.enabledLayerCount   = 0;
#endif

    if (vkCreateInstance(&createInfo, nullptr, &_instance) != VK_SUCCESS)
    {
        LUNA_LOG_ERROR("Failed to create Vulkan instance");
        return false;
    }

    LUNA_LOG_INFO("Vulkan instance created (API 1.3)");
    return true;
}

// ===========================================================================
// Private — Debug messenger
// ===========================================================================
bool VulkanBackend::SetupDebugMessenger()
{
#if defined(_DEBUG)
    VkDebugUtilsMessengerCreateInfoEXT createInfo{};
    createInfo.sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    createInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                               | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    createInfo.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                               | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    createInfo.pfnUserCallback = [](
        VkDebugUtilsMessageSeverityFlagBitsEXT      severity,
        VkDebugUtilsMessageTypeFlagsEXT             /*type*/,
        const VkDebugUtilsMessengerCallbackDataEXT* data,
        void*                                       /*user*/) -> VkBool32
    {
        if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
            LUNA_LOG_ERROR("[Vulkan Validation] %s", data->pMessage);
        return VK_FALSE;
    };

    auto func = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(_instance, "vkCreateDebugUtilsMessengerEXT"));
    if (func && func(_instance, &createInfo, nullptr, &_debugMessenger) == VK_SUCCESS)
    {
        LUNA_LOG_INFO("Vulkan debug messenger active");
        return true;
    }

    LUNA_LOG_ERROR("Could not create Vulkan debug messenger");
#endif
    return true; // non-fatal in release
}

// ===========================================================================
// Private — Surface
// ===========================================================================
bool VulkanBackend::CreateSurface(void* windowHandle)
{
    if (glfwCreateWindowSurface(_instance, static_cast<GLFWwindow*>(windowHandle),
                                nullptr, &_surface) != VK_SUCCESS)
    {
        LUNA_LOG_ERROR("Failed to create Vulkan window surface");
        return false;
    }
    return true;
}

// ===========================================================================
// Private — ImGui descriptor pool
// ===========================================================================
bool VulkanBackend::CreateImGuiDescriptorPool()
{
    VkDescriptorPoolSize poolSizes[] = {
        { VK_DESCRIPTOR_TYPE_SAMPLER,                1000 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,          1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         1000 },
    };

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.poolSizeCount = static_cast<uint32_t>(std::size(poolSizes));
    poolInfo.pPoolSizes    = poolSizes;
    poolInfo.maxSets       = 1000;

    if (vkCreateDescriptorPool(_device->GetDevice(), &poolInfo, nullptr, &_imguiDescriptorPool) != VK_SUCCESS)
    {
        LUNA_LOG_ERROR("Failed to create ImGui descriptor pool");
        return false;
    }
    return true;
}

// ===========================================================================
// Private — Swapchain
// ===========================================================================
bool VulkanBackend::CreateSwapchain(uint32_t width, uint32_t height)
{
    VkPhysicalDevice gpu = _device->GetPhysicalDevice();
    VkDevice         dev = _device->GetDevice();

    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, _surface, &caps);

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, _surface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, _surface, &formatCount, formats.data());

    VkSurfaceFormatKHR chosen = formats[0];
    for (const auto& f : formats)
    {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        { chosen = f; break; }
    }

    VkExtent2D extent;
    if (caps.currentExtent.width != UINT32_MAX)
    {
        extent = caps.currentExtent;
    }
    else
    {
        extent.width  = std::clamp(width,  caps.minImageExtent.width,  caps.maxImageExtent.width);
        extent.height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount)
        imageCount = caps.maxImageCount;

    VkSwapchainCreateInfoKHR info{};
    info.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface          = _surface;
    info.minImageCount    = imageCount;
    info.imageFormat      = chosen.format;
    info.imageColorSpace  = chosen.colorSpace;
    info.imageExtent      = extent;
    info.imageArrayLayers = 1;
    info.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    uint32_t queueFamilies[] = { _device->GetGraphicsQueueFamily(),
                                 _device->GetPresentQueueFamily() };
    if (_device->GetGraphicsQueueFamily() != _device->GetPresentQueueFamily())
    {
        info.imageSharingMode      = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = 2;
        info.pQueueFamilyIndices   = queueFamilies;
    }
    else
    {
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    info.preTransform   = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode    = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped        = VK_TRUE;

    if (vkCreateSwapchainKHR(dev, &info, nullptr, &_swapchain) != VK_SUCCESS)
    {
        LUNA_LOG_ERROR("Failed to create Vulkan swapchain");
        return false;
    }

    _swapchainFormat = chosen.format;
    _swapchainExtent = extent;

    uint32_t count = 0;
    vkGetSwapchainImagesKHR(dev, _swapchain, &count, nullptr);
    _swapchainImages.resize(count);
    vkGetSwapchainImagesKHR(dev, _swapchain, &count, _swapchainImages.data());

    _swapchainImageViews.resize(count);
    for (uint32_t i = 0; i < count; i++)
    {
        VkImageViewCreateInfo ivInfo{};
        ivInfo.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ivInfo.image    = _swapchainImages[i];
        ivInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivInfo.format   = _swapchainFormat;
        ivInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

        if (vkCreateImageView(dev, &ivInfo, nullptr, &_swapchainImageViews[i]) != VK_SUCCESS)
        {
            LUNA_LOG_ERROR("Failed to create swapchain image view %u", i);
            return false;
        }
    }

    LUNA_LOG_INFO("Swapchain: %ux%u, %u images", extent.width, extent.height, count);
    return true;
}

void VulkanBackend::DestroySwapchain()
{
    VkDevice dev = _device->GetDevice();

    for (auto fb : _framebuffers)
        vkDestroyFramebuffer(dev, fb, nullptr);
    _framebuffers.clear();

    for (auto iv : _swapchainImageViews)
        vkDestroyImageView(dev, iv, nullptr);
    _swapchainImageViews.clear();
    _swapchainImages.clear();

    if (_swapchain != VK_NULL_HANDLE)
    {
        vkDestroySwapchainKHR(dev, _swapchain, nullptr);
        _swapchain = VK_NULL_HANDLE;
    }
}

void VulkanBackend::RecreateSwapchain()
{
    DestroySwapchain();
    CreateSwapchain(_width, _height);
    CreateFramebuffers();
}

// ===========================================================================
// Private — Render pass
// ===========================================================================
bool VulkanBackend::CreateRenderPass()
{
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format         = _swapchainFormat;
    colorAttachment.samples        = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments    = &colorRef;

    VkSubpassDependency dep{};
    dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass    = 0;
    dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpInfo{};
    rpInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = 1;
    rpInfo.pAttachments    = &colorAttachment;
    rpInfo.subpassCount    = 1;
    rpInfo.pSubpasses      = &subpass;
    rpInfo.dependencyCount = 1;
    rpInfo.pDependencies   = &dep;

    if (vkCreateRenderPass(_device->GetDevice(), &rpInfo, nullptr, &_renderPass) != VK_SUCCESS)
    {
        LUNA_LOG_ERROR("Failed to create Vulkan render pass");
        return false;
    }
    return true;
}

// ===========================================================================
// Private — Framebuffers
// ===========================================================================
bool VulkanBackend::CreateFramebuffers()
{
    _framebuffers.resize(_swapchainImageViews.size());
    for (size_t i = 0; i < _swapchainImageViews.size(); i++)
    {
        VkFramebufferCreateInfo fbInfo{};
        fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass      = _renderPass;
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments    = &_swapchainImageViews[i];
        fbInfo.width           = _swapchainExtent.width;
        fbInfo.height          = _swapchainExtent.height;
        fbInfo.layers          = 1;

        if (vkCreateFramebuffer(_device->GetDevice(), &fbInfo, nullptr, &_framebuffers[i]) != VK_SUCCESS)
        {
            LUNA_LOG_ERROR("Failed to create framebuffer %zu", i);
            return false;
        }
    }
    return true;
}

// ===========================================================================
// Private — Per-frame resources
// ===========================================================================
bool VulkanBackend::CreateFrameResources()
{
    VkDevice dev = _device->GetDevice();

    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++)
    {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = _device->GetGraphicsQueueFamily();
        poolInfo.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

        if (vkCreateCommandPool(dev, &poolInfo, nullptr, &_frames[i].cmdPool) != VK_SUCCESS)
        {
            LUNA_LOG_ERROR("Failed to create command pool for frame slot %u", i);
            return false;
        }

        VkCommandBufferAllocateInfo cbInfo{};
        cbInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbInfo.commandPool        = _frames[i].cmdPool;
        cbInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbInfo.commandBufferCount = 1;

        if (vkAllocateCommandBuffers(dev, &cbInfo, &_frames[i].cmdBuffer) != VK_SUCCESS)
        {
            LUNA_LOG_ERROR("Failed to allocate command buffer for frame slot %u", i);
            return false;
        }

        VkFenceCreateInfo fenceInfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                                     nullptr, VK_FENCE_CREATE_SIGNALED_BIT };
        if (vkCreateFence(dev, &fenceInfo, nullptr, &_frames[i].fence) != VK_SUCCESS)
        {
            LUNA_LOG_ERROR("Failed to create fence for frame slot %u", i);
            return false;
        }

        VkSemaphoreCreateInfo semInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        if (vkCreateSemaphore(dev, &semInfo, nullptr, &_frames[i].imageReady) != VK_SUCCESS ||
            vkCreateSemaphore(dev, &semInfo, nullptr, &_frames[i].renderDone) != VK_SUCCESS)
        {
            LUNA_LOG_ERROR("Failed to create semaphores for frame slot %u", i);
            return false;
        }
    }
    return true;
}

} // namespace Luna

#endif // LUNA_VULKAN_ENABLED
