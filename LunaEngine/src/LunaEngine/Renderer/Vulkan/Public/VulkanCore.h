#pragma once

#include <vulkan/vulkan.h>
#include <memory>

namespace Luna
{

class VulkanDevice;

/**
 * @brief Core Vulkan infrastructure: Instance, Surface, Device, Debug.
 * 
 * Extracted from VulkanBackend to reduce god-object complexity.
 * All other Vulkan subsystems depend on this.
 */
class VulkanCore
{
public:
    VulkanCore() = default;
    ~VulkanCore();

    // Non-copyable
    VulkanCore(const VulkanCore&) = delete;
    VulkanCore& operator=(const VulkanCore&) = delete;

    /**
     * @brief Initialize Vulkan instance, surface, device, and debug messenger.
     * @param windowHandle GLFW window handle (GLFWwindow*)
     * @param width Initial framebuffer width
     * @param height Initial framebuffer height
     * @return true on success
     */
    bool Init(void* windowHandle, uint32_t width, uint32_t height);

    /**
     * @brief Initialize from existing VulkanDevice (non-owning).
     * 
     * Used by VulkanBackend to share its device with subsystems without
     * duplicating instance/surface/device creation. VulkanCore becomes a
     * thin wrapper that provides helper methods to subsystems.
     * 
     * @param device Existing VulkanDevice (VulkanCore does NOT own this)
     * @return true on success (creates transfer command pool only)
     */
    bool InitFromDevice(VulkanDevice* device);

    /**
     * @brief Shutdown and release all Vulkan resources.
     */
    void Shutdown();

    // === Accessors ===
    
    VkInstance   GetInstance() const { return _instance; }
    VkSurfaceKHR GetSurface()  const { return _surface; }
    VkDevice     GetDevice()   const;
    VkPhysicalDevice GetPhysicalDevice() const;
    
    VulkanDevice* GetVulkanDevice() const { return _ownsDevice ? _device.get() : _deviceRaw; }

    VkQueue GetGraphicsQueue() const;
    VkQueue GetComputeQueue() const;
    uint32_t GetGraphicsQueueFamily() const;
    uint32_t GetComputeQueueFamily() const;

    bool IsDeviceLost() const { return _deviceLost; }
    void SetDeviceLost() { _deviceLost = true; }

    bool IsRTSupported() const;

    // === Single-time command helpers ===
    
    /**
     * @brief Allocate and begin a single-use command buffer.
     * Uses dedicated transfer command pool to avoid race with frame pools.
     */
    VkCommandBuffer BeginSingleTimeCommands();

    /**
     * @brief End, submit, wait, and free a single-use command buffer.
     */
    void EndSingleTimeCommands(VkCommandBuffer cmd);

    // === Memory helpers ===
    
    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);

    bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags props,
                      VkBuffer& outBuffer, VkDeviceMemory& outMemory);

    bool CreateImage(uint32_t width, uint32_t height, VkFormat format,
                     VkImageTiling tiling, VkImageUsageFlags usage,
                     VkMemoryPropertyFlags props,
                     VkImage& outImage, VkDeviceMemory& outMemory);

    VkImageView CreateImageView(VkImage image, VkFormat format, VkImageAspectFlags aspect);

    bool TransitionImageLayout(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout);
    
    bool CopyBuffer(VkBuffer src, VkBuffer dst, VkDeviceSize size);
    bool CopyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height);

private:
    bool CreateInstance();
    bool SetupDebugMessenger();
    bool CreateSurface(void* windowHandle);
    bool CreateTransferCommandPool();

    // === Core objects ===
    VkInstance               _instance       = VK_NULL_HANDLE;
    VkSurfaceKHR             _surface        = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT _debugMessenger = VK_NULL_HANDLE;

    // Device ownership: either owned (unique_ptr) or borrowed (raw ptr)
    std::unique_ptr<VulkanDevice> _device;      // Owned device (used with Init)
    VulkanDevice*                 _deviceRaw = nullptr;  // Borrowed device (used with InitFromDevice)
    bool _ownsDevice = true;  // true if _device owns the VulkanDevice

    // Dedicated command pool for single-time/transfer commands
    // Avoids race condition with per-frame command pools
    VkCommandPool _transferCmdPool = VK_NULL_HANDLE;

    // Device lost flag - once set, all operations bail out early
    bool _deviceLost = false;
};

} // namespace Luna

