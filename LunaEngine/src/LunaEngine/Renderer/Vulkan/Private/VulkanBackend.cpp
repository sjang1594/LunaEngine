#include "LunaPCH.h"
#include "LunaEngine/Renderer/Vulkan/Public/VulkanBackend.h"
#include "Logger/Logger.h"
#include "Renderer/Vulkan/Public/VulkanDevice.h"

namespace Luna
{

VulkanBackend::VulkanBackend() : _instance(VK_NULL_HANDLE), _surface(VK_NULL_HANDLE), _imguiDescriptorPool(VK_NULL_HANDLE){}

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
    SetupImGui();
    InitImGui(windowHandler);
    return true;
}

void VulkanBackend::Shutdown()
{
    ShutdownImGui();
    _device->ShutDown();
    vkDestroySurfaceKHR(_instance, _surface, nullptr);
    vkDestroyInstance(_instance, nullptr);
}

void VulkanBackend::BeginFrame()
{
}

void VulkanBackend::DrawFrame()
{
}

void VulkanBackend::EndFrame()
{
}

void VulkanBackend::InitImGui(void *windowHandler)
{
    ImGui::CreateContext();
    ImGui_ImplGlfw_InitForVulkan(static_cast<GLFWwindow *>(windowHandler), true);

    int w, h;
    glfwGetFramebufferSize(static_cast<GLFWwindow *>(windowHandler), &w, &h);
    _windowData = ImGui_ImplVulkanH_Window();
    SetupVulkanWindow(&_windowData, _surface, w, h);
    
    ImGui_ImplVulkan_InitInfo initInfo = {};
    initInfo.Instance = _instance;
    initInfo.PhysicalDevice = _device->GetPhysicalDevice();
    initInfo.Device = _device->GetDevice();
    initInfo.Queue = _device->GetGraphicsQueue();
    initInfo.QueueFamily = _device->GetGraphicsQueueFamily();
    initInfo.PipelineCache = VK_NULL_HANDLE;
    initInfo.DescriptorPool = _imguiDescriptorPool;
    initInfo.Subpass = 0;
    initInfo.MinImageCount = _minImageCount;
    initInfo.ImageCount = _windowData.ImageCount;
    initInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    initInfo.Allocator = nullptr;
    initInfo.CheckVkResultFn = [](VkResult err) {
        if (err != VK_SUCCESS)
        {
            std::cerr << "ImGui Vulkan Error: " << err << std::endl;
        }
    };
    ImGui_ImplVulkan_Init(&initInfo, _windowData.RenderPass);
}

void VulkanBackend::StartImGui()
{
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
}

void VulkanBackend::RenderImGui()
{
    ImGui::Render();
}

void VulkanBackend::ShutdownImGui()
{
    if (_imguiDescriptorPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(_device->GetDevice(), _imguiDescriptorPool, nullptr);
        _imguiDescriptorPool = VK_NULL_HANDLE;
    }

    ImGui_ImplVulkanH_Window();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

void VulkanBackend::Resize(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0) return;
    vkDeviceWaitIdle(_device->GetDevice());

    // Destroy Old Swap Chain & Related REsources
    ImGui_ImplVulkanH_DestroyWindow(_instance, _device->GetDevice(), &_windowData, nullptr);
    _windowData.Width = width;
    _windowData.Height = height;

    ImGui_ImplVulkanH_CreateOrResizeWindow(
        _instance, _device->GetPhysicalDevice(), _device->GetDevice(), &_windowData,
        _device->GetGraphicsQueueFamily(), nullptr, width, height, _minImageCount
        );

    SetupImGui();
}

void VulkanBackend::SetupImGui()
{
    VkDescriptorPoolSize poolSize[] = {
        {VK_DESCRIPTOR_TYPE_SAMPLER, 1000},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000},
    };

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(std::size(poolSize));
    poolInfo.pPoolSizes = poolSize;
    poolInfo.maxSets = 1000;

    if (vkCreateDescriptorPool(_device->GetDevice(), &poolInfo, nullptr, &_imguiDescriptorPool) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to create descriptor pool!");
    }
}

void VulkanBackend::SetupVulkanWindow(ImGui_ImplVulkanH_Window *wd, VkSurfaceKHR surface, int width, int height)
{
    wd->Surface = surface;
    VkBool32 res;
    vkGetPhysicalDeviceSurfaceSupportKHR(_device->GetPhysicalDevice(), _device->GetGraphicsQueueFamily(), surface, &res);
    if (res != VK_TRUE)
    {
        throw::std::runtime_error("failed to get physical device surface support!");
    }

    constexpr VkFormat requestSurfaceImageFormat[] = {
        VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
        VK_FORMAT_B8G8R8_UNORM, VK_FORMAT_R8G8B8_UNORM };

    constexpr VkColorSpaceKHR requestSurfaceColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    wd->SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(_device->GetPhysicalDevice(), wd->Surface,
        requestSurfaceImageFormat, (size_t)IM_ARRAYSIZE(requestSurfaceImageFormat), requestSurfaceColorSpace);

    // Select the Present Mode
#ifdef IMGUI_UNLIMITED_FRAME_RATE
    VkPresentModeKHR present_modes[] = { VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_FIFO_KHR };
#else
    VkPresentModeKHR present_modes[] = { VK_PRESENT_MODE_FIFO_KHR };
#endif
    wd->PresentMode = ImGui_ImplVulkanH_SelectPresentMode(_device->GetPhysicalDevice(),
        wd->Surface, &present_modes[0], IM_ARRAYSIZE(present_modes));

    IM_ASSERT(_minImageCount >= 2);
    ImGui_ImplVulkanH_CreateOrResizeWindow(_instance, _device->GetPhysicalDevice(),
        _device->GetDevice(), wd, _device->GetGraphicsQueueFamily(), nullptr, width, height, 2);
}

bool VulkanBackend::CreateInstance()
{
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "LunaEngine";
    appInfo.apiVersion = VK_API_VERSION_1_4;

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;

    if (vkCreateInstance(&createInfo, nullptr, &_instance) != VK_SUCCESS)
    {
        LUNA_LOG_ERROR("Failed to Create Instance")
        return false;
    }
    return true;
}

bool VulkanBackend::CreateSurface(void *windowHandle)
{
    if (glfwCreateWindowSurface(_instance, static_cast<GLFWwindow *>(windowHandle), nullptr, &_surface) != VK_SUCCESS)
    {
        LUNA_LOG_ERROR("Failed to Create Window Surface")
        return false;
    }
    return true;
}
}