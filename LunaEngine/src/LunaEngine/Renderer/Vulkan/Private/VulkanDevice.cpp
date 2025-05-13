#include "LunaPCH.h"
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
    uint32_t _physicalDeviceCount = 0; // # of gpu
    vkEnumeratePhysicalDevices(instance, &_physicalDeviceCount, nullptr);
    if (_physicalDeviceCount == 0)
    {
        LUNA_LOG_ERROR("No Vulkan-Support GPUs with Vulkan support");
        return false;
    }

    std::vector<VkPhysicalDevice> devices(_physicalDeviceCount);
    
    devices.resize(_physicalDeviceCount);
    vkEnumeratePhysicalDevices(instance, &_physicalDeviceCount, devices.data());

    for (const auto& device : devices)
    {
        if (FindQueueFamilies(device, surface))
        {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(device, &props);
            LUNA_LOG_INFO("Found GPU: %s", props.deviceName);
            _physicalDevice = device;
            return true;
        }
    }

    return false;
}

bool VulkanDevice::CreateLogicalDevice()
{
    float queuePriority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    VkDeviceQueueCreateInfo queueCreateInfo {};
    queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreateInfo.queueFamilyIndex = _graphicsQueueFamily;
    queueCreateInfo.queueCount = 1;
    queueCreateInfo.pQueuePriorities = &queuePriority;
    queueCreateInfos.push_back(queueCreateInfo);

    if (_graphicsQueueFamily != _presentQueueFamily)
    {
        queueCreateInfo.queueFamilyIndex = _presentQueueFamily;
        queueCreateInfos.push_back(queueCreateInfo);
    }

    // TODO: Comments
    VkPhysicalDeviceFeatures deviceFeatures {};
    deviceFeatures.samplerAnisotropy = VK_TRUE;

    VkDeviceCreateInfo createInfo {};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();
    createInfo.pEnabledFeatures = &deviceFeatures;

    const char* deviceExtensions[] = { "VK_KHR_swapchain" };
    createInfo.enabledExtensionCount = 1;
    createInfo.ppEnabledExtensionNames = deviceExtensions;

    if (vkCreateDevice(_physicalDevice, &createInfo, nullptr, &_device) != VK_SUCCESS)
    {
        LUNA_LOG_ERROR("Failed to create logical device");
        return false;
    }
    return true;
}

bool VulkanDevice::FindQueueFamilies(VkPhysicalDevice device, VkSurfaceKHR surface)
{
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);

    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

    bool graphicsFound = false;
    bool presentFound = false;

    for (uint32_t i = 0; i < queueFamilies.size(); i++)
    {
        if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
        {
            _graphicsQueueFamily = i;
            graphicsFound = true;
        }

        VkBool32 presentSupport = false;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &presentSupport);
        if (presentSupport) {
            _presentQueueFamily = i;
            presentFound = true;
        }

        if (graphicsFound && presentFound) {
            return true;
        }
    }

    return false;
}

}