#include "LunaPCH.h"
#ifdef LUNA_VULKAN_ENABLED

#include "LunaEngine/Renderer/Vulkan/Public/VulkanDevice.h"
#include "Logger/Logger.h"

namespace Luna
{

VulkanDevice::~VulkanDevice()
{
    ShutDown();
}

bool VulkanDevice::Initialize(VkInstance instance, VkSurfaceKHR surface)
{
    if (!PickPhysicalDevice(instance, surface))
    {
        LUNA_LOG_ERROR("Failed to pick a physical device");
        return false;
    }

    if (!CreateLogicalDevice())
    {
        LUNA_LOG_ERROR("Failed to create logical device");
        return false;
    }

    vkGetDeviceQueue(_device, _graphicsQueueFamily, 0, &_graphicsQueue);
    vkGetDeviceQueue(_device, _presentQueueFamily, 0, &_presentQueue);
    return true;
}

void VulkanDevice::ShutDown()
{
    if (_device)
    {
        vkDestroyDevice(_device, nullptr);
        _device = VK_NULL_HANDLE;
    }
}

bool VulkanDevice::PickPhysicalDevice(VkInstance instance, VkSurfaceKHR surface)
{
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
    if (deviceCount == 0)
    {
        LUNA_LOG_ERROR("No Vulkan-capable GPUs found");
        return false;
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

    for (const auto& device : devices)
    {
        if (FindQueueFamilies(device, surface))
        {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(device, &props);
            LUNA_LOG_INFO("Vulkan GPU: %s", props.deviceName);
            _physicalDevice = device;
            return true;
        }
    }

    LUNA_LOG_ERROR("No suitable Vulkan GPU found");
    return false;
}

bool VulkanDevice::CreateLogicalDevice()
{
    float queuePriority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;

    VkDeviceQueueCreateInfo queueCI{};
    queueCI.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCI.queueFamilyIndex = _graphicsQueueFamily;
    queueCI.queueCount       = 1;
    queueCI.pQueuePriorities = &queuePriority;
    queueCreateInfos.push_back(queueCI);

    if (_graphicsQueueFamily != _presentQueueFamily)
    {
        queueCI.queueFamilyIndex = _presentQueueFamily;
        queueCreateInfos.push_back(queueCI);
    }

    VkPhysicalDeviceFeatures deviceFeatures{};
    deviceFeatures.samplerAnisotropy = VK_TRUE;

    const char* deviceExtensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

    VkDeviceCreateInfo createInfo{};
    createInfo.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount    = static_cast<uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos       = queueCreateInfos.data();
    createInfo.pEnabledFeatures        = &deviceFeatures;
    createInfo.enabledExtensionCount   = 1;
    createInfo.ppEnabledExtensionNames = deviceExtensions;

    if (vkCreateDevice(_physicalDevice, &createInfo, nullptr, &_device) != VK_SUCCESS)
    {
        LUNA_LOG_ERROR("Failed to create Vulkan logical device");
        return false;
    }
    return true;
}

bool VulkanDevice::FindQueueFamilies(VkPhysicalDevice device, VkSurfaceKHR surface)
{
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());

    bool graphicsFound = false, presentFound = false;

    for (uint32_t i = 0; i < count; i++)
    {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
        {
            _graphicsQueueFamily = i;
            graphicsFound = true;
        }

        VkBool32 presentSupport = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &presentSupport);
        if (presentSupport)
        {
            _presentQueueFamily = i;
            presentFound = true;
        }

        if (graphicsFound && presentFound)
            return true;
    }
    return false;
}

} // namespace Luna

#endif // LUNA_VULKAN_ENABLED
