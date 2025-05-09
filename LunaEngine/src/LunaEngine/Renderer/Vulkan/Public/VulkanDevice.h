#pragma once
#include "Renderer/HAL/Public/IRenderDevice.h"
#include "vulkan/vulkan.h"
#include "vulkan/vulkan_core.h"
namespace Luna
{
class VulkanDevice : public IRenderDevice
{
public:
    VulkanDevice(VkInstance instance);
    ~VulkanDevice() override;
    const char* GetDeviceName() const override { return "Vulkan"; }
    VkDevice GetDevice() const { return _device; }
    VkPhysicalDevice GetPhysicalDevice() const { return _physicalDevice; }
    VkQueue GetGraphicsQueue() const { return _graphicsQueue; }
    VkQueue GetPresentQueue() const { return _presentQueue; }
    
private:
    void PickPhysicalDevice();
    void CreateLogicalDevice();
    void InitializeQueues();
    
    VkDevice                        _device = VK_NULL_HANDLE;
    VkPhysicalDevice                _physicalDevice = VK_NULL_HANDLE; // gpu
    vector<VkPhysicalDevice>        _physicalDevices;
    VkInstance                      _vkInstance;
    VkQueue                         _graphicsQueue = VK_NULL_HANDLE;
    VkQueue                         _presentQueue = VK_NULL_HANDLE;
};
}