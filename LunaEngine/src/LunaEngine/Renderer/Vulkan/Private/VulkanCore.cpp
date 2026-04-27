#include "LunaPCH.h"
#include "LunaEngine/Renderer/Vulkan/Public/VulkanCore.h"
#include "LunaEngine/Renderer/Vulkan/Public/VulkanDevice.h"
#include "Logger/Logger.h"

#include <GLFW/glfw3.h>

namespace Luna
{

// ---------------------------------------------------------------------------
// Helper: Destroy debug messenger
// ---------------------------------------------------------------------------
static void DestroyDebugMessengerEXT(VkInstance instance, VkDebugUtilsMessengerEXT messenger,
                                     const VkAllocationCallbacks* alloc)
{
    auto fn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
    if (fn) fn(instance, messenger, alloc);
}

// ===========================================================================
// Destructor
// ===========================================================================
VulkanCore::~VulkanCore()
{
    Shutdown();
}

// ===========================================================================
// Init
// ===========================================================================
bool VulkanCore::Init(void* windowHandle, uint32_t width, uint32_t height)
{
    _ownsDevice = true;
    if (!CreateInstance()) return false;
    SetupDebugMessenger();
    if (!CreateSurface(windowHandle)) return false;

    _device = std::make_unique<VulkanDevice>();
    if (!_device->Initialize(_instance, _surface)) return false;

    if (!CreateTransferCommandPool()) return false;

    LUNA_LOG_INFO("VulkanCore: initialized");
    return true;
}

// ===========================================================================
// InitFromDevice — wrap existing VulkanDevice (non-owning)
// ===========================================================================
bool VulkanCore::InitFromDevice(VulkanDevice* device)
{
    if (!device || device->GetDevice() == VK_NULL_HANDLE)
    {
        LUNA_LOG_ERROR("VulkanCore::InitFromDevice: invalid device");
        return false;
    }

    _ownsDevice = false;
    _deviceRaw = device;
    // _device stays null — we don't own the device
    // _instance/_surface/_debugMessenger stay null — owned by caller

    if (!CreateTransferCommandPool()) return false;

    LUNA_LOG_INFO("VulkanCore: initialized from existing device");
    return true;
}

// ===========================================================================
// Shutdown
// ===========================================================================
void VulkanCore::Shutdown()
{
    VkDevice dev = GetDevice();
    
    // Destroy transfer command pool (we always create this, even in non-owning mode)
    if (dev && _transferCmdPool)
    {
        vkDeviceWaitIdle(dev);
        vkResetCommandPool(dev, _transferCmdPool, 0);
        vkDestroyCommandPool(dev, _transferCmdPool, nullptr);
        _transferCmdPool = VK_NULL_HANDLE;
    }

    // In non-owning mode, we don't destroy the device/instance/surface.
    // Don't null _deviceRaw here - the device is still valid and other cleanup
    // code may need it. VulkanBackend::_device is destroyed at the very end.
    if (!_ownsDevice)
    {
        LUNA_LOG_INFO("VulkanCore: shutdown complete (non-owning mode)");
        return;
    }

    // Owning mode — destroy all resources
    if (!_instance) return;

    if (dev)
    {
        vkDeviceWaitIdle(dev);
    }

    _device.reset();

    if (_debugMessenger)
    {
        DestroyDebugMessengerEXT(_instance, _debugMessenger, nullptr);
        _debugMessenger = VK_NULL_HANDLE;
    }

    if (_surface)
    {
        vkDestroySurfaceKHR(_instance, _surface, nullptr);
        _surface = VK_NULL_HANDLE;
    }

    if (_instance)
    {
        vkDestroyInstance(_instance, nullptr);
        _instance = VK_NULL_HANDLE;
    }

    LUNA_LOG_INFO("VulkanCore: shutdown complete");
}

// ===========================================================================
// Accessors
// ===========================================================================
VkDevice VulkanCore::GetDevice() const
{
    VulkanDevice* dev = _ownsDevice ? _device.get() : _deviceRaw;
    return dev ? dev->GetDevice() : VK_NULL_HANDLE;
}

VkPhysicalDevice VulkanCore::GetPhysicalDevice() const
{
    VulkanDevice* dev = _ownsDevice ? _device.get() : _deviceRaw;
    return dev ? dev->GetPhysicalDevice() : VK_NULL_HANDLE;
}

VkQueue VulkanCore::GetGraphicsQueue() const
{
    VulkanDevice* dev = _ownsDevice ? _device.get() : _deviceRaw;
    return dev ? dev->GetGraphicsQueue() : VK_NULL_HANDLE;
}

VkQueue VulkanCore::GetComputeQueue() const
{
    VulkanDevice* dev = _ownsDevice ? _device.get() : _deviceRaw;
    return dev ? dev->GetComputeQueue() : VK_NULL_HANDLE;
}

uint32_t VulkanCore::GetGraphicsQueueFamily() const
{
    VulkanDevice* dev = _ownsDevice ? _device.get() : _deviceRaw;
    return dev ? dev->GetGraphicsQueueFamily() : 0;
}

uint32_t VulkanCore::GetComputeQueueFamily() const
{
    VulkanDevice* dev = _ownsDevice ? _device.get() : _deviceRaw;
    return dev ? dev->GetComputeQueueFamily() : 0;
}

bool VulkanCore::IsRTSupported() const
{
    VulkanDevice* dev = _ownsDevice ? _device.get() : _deviceRaw;
    return dev ? dev->IsRTSupported() : false;
}

// ===========================================================================
// CreateInstance
// ===========================================================================
bool VulkanCore::CreateInstance()
{
    uint32_t extCount = 0;
    const char** glfwExts = glfwGetRequiredInstanceExtensions(&extCount);
    if (!glfwExts)
    {
        LUNA_LOG_ERROR("Vulkan not supported");
        return false;
    }

    std::vector<const char*> extensions(glfwExts, glfwExts + extCount);

#if defined(_DEBUG)
    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    const char* validationLayer = "VK_LAYER_KHRONOS_validation";
#endif

    VkApplicationInfo appInfo{};
    appInfo.sType       = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pEngineName = "Luna";
    appInfo.apiVersion  = VK_API_VERSION_1_3;

    VkInstanceCreateInfo createInfo{};
    createInfo.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo        = &appInfo;
    createInfo.enabledExtensionCount   = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

#if defined(_DEBUG)
    createInfo.enabledLayerCount   = 1;
    createInfo.ppEnabledLayerNames = &validationLayer;
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
// SetupDebugMessenger
// ===========================================================================
bool VulkanCore::SetupDebugMessenger()
{
#if defined(_DEBUG)
    VkDebugUtilsMessengerCreateInfoEXT createInfo{};
    createInfo.sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    createInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                               | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    createInfo.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                               | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    createInfo.pfnUserCallback = [](VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                    VkDebugUtilsMessageTypeFlagsEXT,
                                    const VkDebugUtilsMessengerCallbackDataEXT* data,
                                    void*) -> VkBool32 {
        if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
            LUNA_LOG_ERROR("[VkVal] %s", data->pMessage);
        return VK_FALSE;
    };

    auto createFn = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(_instance, "vkCreateDebugUtilsMessengerEXT"));
    
    if (createFn)
        createFn(_instance, &createInfo, nullptr, &_debugMessenger);

    LUNA_LOG_INFO("Vulkan debug messenger active");
#endif
    return true;
}

// ===========================================================================
// CreateSurface
// ===========================================================================
bool VulkanCore::CreateSurface(void* windowHandle)
{
    return glfwCreateWindowSurface(_instance,
        static_cast<GLFWwindow*>(windowHandle), nullptr, &_surface) == VK_SUCCESS;
}

// ===========================================================================
// CreateTransferCommandPool
// ===========================================================================
bool VulkanCore::CreateTransferCommandPool()
{
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = GetGraphicsQueueFamily();
    poolInfo.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;

    if (vkCreateCommandPool(GetDevice(), &poolInfo, nullptr, &_transferCmdPool) != VK_SUCCESS)
    {
        LUNA_LOG_ERROR("VK: Failed to create transfer command pool");
        return false;
    }
    return true;
}

// ===========================================================================
// Single-time commands
// ===========================================================================
VkCommandBuffer VulkanCore::BeginSingleTimeCommands()
{
    if (_deviceLost) return VK_NULL_HANDLE;

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool        = _transferCmdPool;
    allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkResult result = vkAllocateCommandBuffers(GetDevice(), &allocInfo, &cmd);

    if (result == VK_ERROR_DEVICE_LOST)
    {
        LUNA_LOG_ERROR("VK: Device lost during command buffer allocation");
        _deviceLost = true;
        return VK_NULL_HANDLE;
    }

    if (result != VK_SUCCESS)
    {
        LUNA_LOG_ERROR("VK: Failed to allocate command buffer: %d", static_cast<int>(result));
        return VK_NULL_HANDLE;
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    return cmd;
}

void VulkanCore::EndSingleTimeCommands(VkCommandBuffer cmd)
{
    if (cmd == VK_NULL_HANDLE || _deviceLost) return;

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo{};
    submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers    = &cmd;

    VkResult submitResult = vkQueueSubmit(GetGraphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE);
    if (submitResult == VK_ERROR_DEVICE_LOST)
    {
        LUNA_LOG_ERROR("VK: Device lost during queue submit (single-time cmd)");
        _deviceLost = true;
    }

    VkResult waitResult = vkQueueWaitIdle(GetGraphicsQueue());
    if (waitResult == VK_ERROR_DEVICE_LOST)
    {
        LUNA_LOG_ERROR("VK: Device lost during queue wait (single-time cmd)");
        _deviceLost = true;
    }

    vkFreeCommandBuffers(GetDevice(), _transferCmdPool, 1, &cmd);
}

// ===========================================================================
// Memory helpers
// ===========================================================================
uint32_t VulkanCore::FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(GetPhysicalDevice(), &memProps);

    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
    {
        if ((typeFilter & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & properties) == properties)
        {
            return i;
        }
    }
    return UINT32_MAX;
}

bool VulkanCore::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                              VkMemoryPropertyFlags props,
                              VkBuffer& outBuffer, VkDeviceMemory& outMemory)
{
    VkDevice dev = GetDevice();

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size        = size;
    bufferInfo.usage       = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(dev, &bufferInfo, nullptr, &outBuffer) != VK_SUCCESS)
        return false;

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(dev, outBuffer, &memReqs);

    // When buffer needs a device address, include the flag
    VkMemoryAllocateFlagsInfo flagsInfo{};
    flagsInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    flagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize  = memReqs.size;
    allocInfo.memoryTypeIndex = FindMemoryType(memReqs.memoryTypeBits, props);

    if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)
        allocInfo.pNext = &flagsInfo;

    if (vkAllocateMemory(dev, &allocInfo, nullptr, &outMemory) != VK_SUCCESS)
        return false;

    vkBindBufferMemory(dev, outBuffer, outMemory, 0);
    return true;
}

bool VulkanCore::CreateImage(uint32_t width, uint32_t height, VkFormat format,
                             VkImageTiling tiling, VkImageUsageFlags usage,
                             VkMemoryPropertyFlags props,
                             VkImage& outImage, VkDeviceMemory& outMemory)
{
    VkDevice dev = GetDevice();

    VkImageCreateInfo imageInfo{};
    imageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType     = VK_IMAGE_TYPE_2D;
    imageInfo.format        = format;
    imageInfo.extent        = { width, height, 1 };
    imageInfo.mipLevels     = 1;
    imageInfo.arrayLayers   = 1;
    imageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling        = tiling;
    imageInfo.usage         = usage;
    imageInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(dev, &imageInfo, nullptr, &outImage) != VK_SUCCESS)
        return false;

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(dev, outImage, &memReqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize  = memReqs.size;
    allocInfo.memoryTypeIndex = FindMemoryType(memReqs.memoryTypeBits, props);

    if (vkAllocateMemory(dev, &allocInfo, nullptr, &outMemory) != VK_SUCCESS)
        return false;

    vkBindImageMemory(dev, outImage, outMemory, 0);
    return true;
}

VkImageView VulkanCore::CreateImageView(VkImage image, VkFormat format, VkImageAspectFlags aspect)
{
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image                           = image;
    viewInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format                          = format;
    viewInfo.subresourceRange.aspectMask     = aspect;
    viewInfo.subresourceRange.baseMipLevel   = 0;
    viewInfo.subresourceRange.levelCount     = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount     = 1;

    VkImageView view = VK_NULL_HANDLE;
    vkCreateImageView(GetDevice(), &viewInfo, nullptr, &view);
    return view;
}

bool VulkanCore::TransitionImageLayout(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout)
{
    VkCommandBuffer cmd = BeginSingleTimeCommands();
    if (!cmd) return false;

    VkImageMemoryBarrier barrier{};
    barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout                       = oldLayout;
    barrier.newLayout                       = newLayout;
    barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    barrier.image                           = image;
    barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel   = 0;
    barrier.subresourceRange.levelCount     = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount     = 1;

    VkPipelineStageFlags srcStage, dstStage;

    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED)
    {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    else
    {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }

    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    EndSingleTimeCommands(cmd);
    return true;
}

bool VulkanCore::CopyBuffer(VkBuffer src, VkBuffer dst, VkDeviceSize size)
{
    VkCommandBuffer cmd = BeginSingleTimeCommands();
    if (!cmd) return false;

    VkBufferCopy copyRegion{};
    copyRegion.size = size;
    vkCmdCopyBuffer(cmd, src, dst, 1, &copyRegion);

    EndSingleTimeCommands(cmd);
    return true;
}

bool VulkanCore::CopyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height)
{
    VkCommandBuffer cmd = BeginSingleTimeCommands();
    if (!cmd) return false;

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel       = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount     = 1;
    region.imageExtent                     = { width, height, 1 };

    vkCmdCopyBufferToImage(cmd, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    EndSingleTimeCommands(cmd);
    return true;
}

} // namespace Luna

