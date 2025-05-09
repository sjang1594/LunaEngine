#include "LunaPCH.h"
#include "LunaEngine/Renderer/Vulkan/Public/VulkanDevice.h"

namespace Luna
{

VulkanDevice::VulkanDevice(VkInstance instance) : _vkInstance(instance)
{
    PickPhysicalDevice();
    CreateLogicalDevice();
}

VulkanDevice::~VulkanDevice()
{
    if (_device)
    {
        vkDestroyDevice(_device, nullptr);
    }
}

void VulkanDevice::PickPhysicalDevice()
{
    VkResult res;
    uint32_t _physicalDeviceCount = 0; // # of gpu
    res = vkEnumeratePhysicalDevices(_vkInstance, &_physicalDeviceCount, nullptr);
    CheckIfVKFailed(res);
    _physicalDevices.resize(_physicalDeviceCount);
    res = vkEnumeratePhysicalDevices(_vkInstance, &_physicalDeviceCount, _physicalDevices.data());
    CheckIfVKFailed(res);
    for (const auto& device : _physicalDevices)
    {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(device, &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
        {
            _physicalDevice = device;
            break;
        }
    }
    CheckIfVKFailed(res);
}

void VulkanDevice::CreateLogicalDevice()
{
    VkResult res;
    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    res = vkCreateDevice(_physicalDevice, &createInfo, nullptr, &_device);
    CheckIfVKFailed(res);
}

void VulkanDevice::InitializeQueues()
{
}
}