#include "LunaPCH.h"
#include "LunaEngine/Renderer/Vulkan/Public/VulkanBackend.h"
#include "Logger/Logger.h"
#include "Renderer/Vulkan/Public/VulkanDevice.h"

namespace Luna
{

VulkanBackend::VulkanBackend()
    : _instance(VK_NULL_HANDLE), _surface(VK_NULL_HANDLE), _imguiDescriptorPool(VK_NULL_HANDLE)
{
}

VulkanBackend::~VulkanBackend()
{
    VulkanBackend::Shutdown();
}

bool VulkanBackend::Init(void *windowHandler, uint32_t width, uint32_t height)
{
    if (!CreateInstance()) return false;
    if (!CreateSurface(windowHandler)) return false;

    _device = std::make_unique<VulkanDevice>();
    if (!_device->Initialize(_instance, _surface)) return false;

    if (!CreateImGuiDescriptorPool()) return false;

    int w = static_cast<int>(width);
    int h = static_cast<int>(height);
    _windowData = ImGui_ImplVulkanH_Window();
    SetupVulkanWindow(&_windowData, _surface, w, h);

    InitImGui(windowHandler);
    return true;
}

void VulkanBackend::Shutdown()
{
    if (_device && _device->GetDevice() != VK_NULL_HANDLE)
        vkDeviceWaitIdle(_device->GetDevice());

    ShutdownImGui();

    if (_device)
        _device->ShutDown();

    if (_surface != VK_NULL_HANDLE)
    {
        vkDestroySurfaceKHR(_instance, _surface, nullptr);
        _surface = VK_NULL_HANDLE;
    }

    if (_instance != VK_NULL_HANDLE)
    {
        vkDestroyInstance(_instance, nullptr);
        _instance = VK_NULL_HANDLE;
    }
}

// ---------------------------------------------------------------------------
// Frame (stubs — requires swapchain implementation)
// ---------------------------------------------------------------------------
void VulkanBackend::BeginFrame()
{
    // TODO: acquire swapchain image, begin command buffer
}

void VulkanBackend::DrawFrame()
{
    // TODO: record draw commands
}

void VulkanBackend::EndFrame()
{
    // TODO: submit command buffer, present swapchain image
}

// ---------------------------------------------------------------------------
// ImGui
// ---------------------------------------------------------------------------
void VulkanBackend::InitImGui(void *windowHandler)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForVulkan(static_cast<GLFWwindow *>(windowHandler), true);

    ImGui_ImplVulkan_InitInfo initInfo = {};
    initInfo.Instance        = _instance;
    initInfo.PhysicalDevice  = _device->GetPhysicalDevice();
    initInfo.Device          = _device->GetDevice();
    initInfo.Queue           = _device->GetGraphicsQueue();
    initInfo.QueueFamily     = _device->GetGraphicsQueueFamily();
    initInfo.PipelineCache   = VK_NULL_HANDLE;
    initInfo.DescriptorPool  = _imguiDescriptorPool;
    initInfo.Subpass         = 0;
    initInfo.MinImageCount   = _minImageCount;
    initInfo.ImageCount      = _windowData.ImageCount;
    initInfo.MSAASamples     = VK_SAMPLE_COUNT_1_BIT;
    initInfo.Allocator       = nullptr;
    initInfo.CheckVkResultFn = [](VkResult err) {
        if (err != VK_SUCCESS)
            LUNA_LOG_ERROR("ImGui Vulkan error: VkResult %d", static_cast<int>(err));
    };

    ImGui_ImplVulkan_Init(&initInfo, _windowData.RenderPass);
}

void VulkanBackend::StartImGui()
{
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame(); // must come after backend NewFrame
    ImGui::NewFrame();
}

void VulkanBackend::RenderImGui()
{
    ImGui::Render();
    // TODO: submit ImGui_ImplVulkan_RenderDrawData inside the active command buffer
    // once BeginFrame/EndFrame have proper command recording
}

void VulkanBackend::ShutdownImGui()
{
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();

    if (_device && _device->GetDevice() != VK_NULL_HANDLE)
        ImGui_ImplVulkanH_DestroyWindow(_instance, _device->GetDevice(), &_windowData, nullptr);

    if (_imguiDescriptorPool != VK_NULL_HANDLE && _device)
    {
        vkDestroyDescriptorPool(_device->GetDevice(), _imguiDescriptorPool, nullptr);
        _imguiDescriptorPool = VK_NULL_HANDLE;
    }

    ImGui::DestroyContext();
}

void VulkanBackend::Resize(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0) return;

    vkDeviceWaitIdle(_device->GetDevice());

    // Destroy old window resources
    ImGui_ImplVulkanH_DestroyWindow(_instance, _device->GetDevice(), &_windowData, nullptr);

    // Destroy old descriptor pool before creating a new one
    if (_imguiDescriptorPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(_device->GetDevice(), _imguiDescriptorPool, nullptr);
        _imguiDescriptorPool = VK_NULL_HANDLE;
    }

    _windowData.Width  = static_cast<int>(width);
    _windowData.Height = static_cast<int>(height);

    ImGui_ImplVulkanH_CreateOrResizeWindow(
        _instance, _device->GetPhysicalDevice(), _device->GetDevice(),
        &_windowData, _device->GetGraphicsQueueFamily(), nullptr,
        width, height, _minImageCount);

    CreateImGuiDescriptorPool();
}

// ---------------------------------------------------------------------------
// Draw calls (stubs)
// ---------------------------------------------------------------------------
void VulkanBackend::Draw(uint32_t /*vertexCount*/)
{
    // TODO: record vkCmdDraw into active command buffer
}

void VulkanBackend::SetVertexBuffer(class IBuffer * /*buffer*/)
{
    // TODO: vkCmdBindVertexBuffers
}

void VulkanBackend::BindPipeline(class IPipeline * /*pipeline*/)
{
    // TODO: vkCmdBindPipeline
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------
bool VulkanBackend::CreateInstance()
{
    // Collect extensions required by GLFW (VK_KHR_surface + platform surface)
    uint32_t glfwExtCount = 0;
    const char **glfwExt  = glfwGetRequiredInstanceExtensions(&glfwExtCount);
    if (!glfwExt)
    {
        LUNA_LOG_ERROR("glfwGetRequiredInstanceExtensions returned null — Vulkan may not be supported");
        return false;
    }

    std::vector<const char *> extensions(glfwExt, glfwExt + glfwExtCount);

#if defined(_DEBUG)
    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    const char *validationLayer = "VK_LAYER_KHRONOS_validation";
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
    createInfo.enabledLayerCount = 0;
#endif

    if (vkCreateInstance(&createInfo, nullptr, &_instance) != VK_SUCCESS)
    {
        LUNA_LOG_ERROR("Failed to create Vulkan instance");
        return false;
    }

    LUNA_LOG_INFO("Vulkan instance created");
    return true;
}

bool VulkanBackend::CreateSurface(void *windowHandle)
{
    if (glfwCreateWindowSurface(_instance, static_cast<GLFWwindow *>(windowHandle),
                                nullptr, &_surface) != VK_SUCCESS)
    {
        LUNA_LOG_ERROR("Failed to create window surface");
        return false;
    }
    return true;
}

bool VulkanBackend::CreateImGuiDescriptorPool()
{
    VkDescriptorPoolSize poolSizes[] = {
        {VK_DESCRIPTOR_TYPE_SAMPLER,                1000},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,          1000},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          1000},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         1000},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         1000},
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

void VulkanBackend::SetupVulkanWindow(ImGui_ImplVulkanH_Window *wd, VkSurfaceKHR surface,
                                      int width, int height)
{
    wd->Surface = surface;

    VkBool32 surfaceSupported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(_device->GetPhysicalDevice(),
                                          _device->GetGraphicsQueueFamily(),
                                          surface, &surfaceSupported);
    if (surfaceSupported != VK_TRUE)
        throw std::runtime_error("Selected queue family does not support the window surface");

    constexpr VkFormat preferredFormats[] = {
        VK_FORMAT_B8G8R8A8_UNORM,
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_FORMAT_B8G8R8_UNORM,
        VK_FORMAT_R8G8B8_UNORM,
    };
    wd->SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(
        _device->GetPhysicalDevice(), wd->Surface,
        preferredFormats, static_cast<int>(std::size(preferredFormats)),
        VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);

#ifdef IMGUI_UNLIMITED_FRAME_RATE
    VkPresentModeKHR presentModes[] = {
        VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_FIFO_KHR};
#else
    VkPresentModeKHR presentModes[] = {VK_PRESENT_MODE_FIFO_KHR};
#endif
    wd->PresentMode = ImGui_ImplVulkanH_SelectPresentMode(
        _device->GetPhysicalDevice(), wd->Surface,
        presentModes, static_cast<int>(std::size(presentModes)));

    IM_ASSERT(_minImageCount >= 2);
    ImGui_ImplVulkanH_CreateOrResizeWindow(
        _instance, _device->GetPhysicalDevice(), _device->GetDevice(),
        wd, _device->GetGraphicsQueueFamily(), nullptr,
        width, height, _minImageCount);
}

} // namespace Luna
