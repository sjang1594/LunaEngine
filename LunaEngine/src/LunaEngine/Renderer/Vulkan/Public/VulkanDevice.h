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
    VkQueue GetComputeQueue() const { return _computeQueue; }
    uint32_t GetGraphicsQueueFamily() const { return _graphicsQueueFamily; }
    uint32_t GetPresentQueueFamily()  const { return _presentQueueFamily; }
    uint32_t GetComputeQueueFamily()  const { return _computeQueueFamily; }
    bool     IsRTSupported()          const { return _rtSupported; }
    bool     IsAsyncComputeSupported() const { return _asyncComputeSupported; }
    
private:
    bool PickPhysicalDevice(VkInstance instance, VkSurfaceKHR surface);
    bool CreateLogicalDevice();
    bool FindQueueFamilies(VkPhysicalDevice device, VkSurfaceKHR surface);

    
    VkPhysicalDevice    _physicalDevice = VK_NULL_HANDLE;
    VkDevice            _device = VK_NULL_HANDLE;
    VkQueue             _graphicsQueue;
    VkQueue             _presentQueue;
    VkQueue             _computeQueue = VK_NULL_HANDLE;
    uint32_t            _graphicsQueueFamily;
    uint32_t            _presentQueueFamily;
    uint32_t            _computeQueueFamily = UINT32_MAX;
    bool                _rtSupported = false;
    bool                _asyncComputeSupported = false;
};
}