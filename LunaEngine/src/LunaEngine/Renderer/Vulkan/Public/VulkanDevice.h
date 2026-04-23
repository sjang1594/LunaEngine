#pragma once
#include "Renderer/HAL/Public/IRenderDevice.h"
#include "vulkan/vulkan.h"
#include "vulkan/vulkan_core.h"
namespace Luna
{
class VulkanDevice : public IRenderDevice
{
public:
    VulkanDevice() = default;
    ~VulkanDevice() override;
    
    bool Initialize(VkInstance instance, VkSurfaceKHR surface);  // Vulkan-specific, not an override
    void ShutDown();
    
    const char* GetDeviceName() const override { return "Vulkan"; }
    VkDevice GetDevice() const { return _device; }
    VkPhysicalDevice GetPhysicalDevice() const { return _physicalDevice; }
    VkQueue GetGraphicsQueue() const { return _graphicsQueue; }
    VkQueue GetPresentQueue() const { return _presentQueue; }
    uint32_t GetGraphicsQueueFamily() const { return _graphicsQueueFamily; }
    uint32_t GetPresentQueueFamily()  const { return _presentQueueFamily; }
    bool     IsRTSupported()          const { return _rtSupported; }  // Phase 18D
    
private:
    bool PickPhysicalDevice(VkInstance instance, VkSurfaceKHR surface);
    bool CreateLogicalDevice();
    bool FindQueueFamilies(VkPhysicalDevice device, VkSurfaceKHR surface);

    
    VkPhysicalDevice    _physicalDevice = VK_NULL_HANDLE;
    VkDevice            _device = VK_NULL_HANDLE;
    VkQueue             _graphicsQueue;
    VkQueue             _presentQueue;
    uint32_t            _graphicsQueueFamily;
    uint32_t            _presentQueueFamily;
    bool                _rtSupported = false;  // Phase 18D: set in CreateLogicalDevice
};
}