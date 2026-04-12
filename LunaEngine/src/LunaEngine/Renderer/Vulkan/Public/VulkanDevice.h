#pragma once
#ifdef LUNA_VULKAN_ENABLED

#include "Renderer/HAL/Public/IRenderDevice.h"
// vulkan.h is already included by LunaPCH.h when LUNA_VULKAN_ENABLED is defined.

namespace Luna
{
class VulkanDevice : public IRenderDevice
{
public:
    VulkanDevice() = default;
    ~VulkanDevice() override;

    bool Initialize(VkInstance instance, VkSurfaceKHR surface) override;
    void ShutDown();

    const char* GetDeviceName() const override { return "Vulkan"; }
    VkDevice         GetDevice()             const { return _device; }
    VkPhysicalDevice GetPhysicalDevice()     const { return _physicalDevice; }
    VkQueue          GetGraphicsQueue()      const { return _graphicsQueue; }
    VkQueue          GetPresentQueue()       const { return _presentQueue; }
    uint32_t         GetGraphicsQueueFamily()const { return _graphicsQueueFamily; }
    uint32_t         GetPresentQueueFamily() const { return _presentQueueFamily; }

private:
    bool PickPhysicalDevice(VkInstance instance, VkSurfaceKHR surface);
    bool CreateLogicalDevice();
    bool FindQueueFamilies(VkPhysicalDevice device, VkSurfaceKHR surface);

    VkPhysicalDevice _physicalDevice    = VK_NULL_HANDLE;
    VkDevice         _device            = VK_NULL_HANDLE;
    VkQueue          _graphicsQueue     = VK_NULL_HANDLE;
    VkQueue          _presentQueue      = VK_NULL_HANDLE;
    uint32_t         _graphicsQueueFamily = 0;
    uint32_t         _presentQueueFamily  = 0;
};
} // namespace Luna

#endif // LUNA_VULKAN_ENABLED
