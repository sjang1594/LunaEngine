#pragma once
#ifdef LUNA_VULKAN_ENABLED

#include <LunaEngine/LunaPCH.h>
#include <LunaEngine/Renderer/HAL/Public/IRenderBackend.h>

namespace Luna
{
class VulkanDevice;

class VulkanBackend : public IRenderBackend
{
  public:
    VulkanBackend();
    virtual ~VulkanBackend() override;

    bool Init(void* windowHandler, uint32_t width, uint32_t height) override;
    void Shutdown() override;

    void BeginFrame() override;
    void DrawFrame() override;
    void EndFrame() override;

    void InitImGui(void* windowHandler) override;
    void StartImGui() override;
    void RenderImGui() override;
    void ShutdownImGui() override;
    void Resize(uint32_t width, uint32_t height) override;

    void Draw(uint32_t vertexCount) override;
    void SetVertexBuffer(class IBuffer* buffer) override;
    void BindPipeline(class IPipeline* pipeline) override;

    const char* GetBackendName() const override { return "Vulkan"; }

  private:
    // ---------------------------------------------------------------------------
    // Init helpers
    // ---------------------------------------------------------------------------
    bool CreateInstance();
    bool SetupDebugMessenger();
    bool CreateSurface(void* windowHandle);
    bool CreateImGuiDescriptorPool();

    // Swapchain lifecycle
    bool CreateSwapchain(uint32_t width, uint32_t height);
    void DestroySwapchain();
    void RecreateSwapchain();

    bool CreateRenderPass();
    bool CreateFramebuffers();
    bool CreateFrameResources();

    // ---------------------------------------------------------------------------
    // Core Vulkan objects
    // ---------------------------------------------------------------------------
    VkInstance   _instance = VK_NULL_HANDLE;
    VkSurfaceKHR _surface  = VK_NULL_HANDLE;

    std::unique_ptr<VulkanDevice> _device;

    VkDebugUtilsMessengerEXT _debugMessenger = VK_NULL_HANDLE;

    // ---------------------------------------------------------------------------
    // Swapchain
    // ---------------------------------------------------------------------------
    VkSwapchainKHR            _swapchain       = VK_NULL_HANDLE;
    VkFormat                  _swapchainFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D                _swapchainExtent = {};
    std::vector<VkImage>      _swapchainImages;
    std::vector<VkImageView>  _swapchainImageViews;

    // ---------------------------------------------------------------------------
    // Render pass + framebuffers
    // ---------------------------------------------------------------------------
    VkRenderPass               _renderPass = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> _framebuffers;

    // ---------------------------------------------------------------------------
    // Per-frame resources (ring buffer, N = FRAMES_IN_FLIGHT)
    // ---------------------------------------------------------------------------
    static constexpr uint32_t FRAMES_IN_FLIGHT = 2;

    struct VkFrameResource
    {
        VkCommandPool   cmdPool    = VK_NULL_HANDLE;
        VkCommandBuffer cmdBuffer  = VK_NULL_HANDLE;
        VkFence         fence      = VK_NULL_HANDLE;  // CPU stalls here if GPU is behind
        VkSemaphore     imageReady = VK_NULL_HANDLE;  // signalled by vkAcquireNextImageKHR
        VkSemaphore     renderDone = VK_NULL_HANDLE;  // signalled after vkQueueSubmit
    };
    VkFrameResource _frames[FRAMES_IN_FLIGHT];

    uint32_t _frameIndex  = 0;     // which FrameResource slot we're currently using
    uint32_t _imageIndex  = 0;     // swapchain image acquired this frame
    bool     _frameActive = false; // false when acquire returns OUT_OF_DATE — skip submit

    // ---------------------------------------------------------------------------
    // ImGui descriptor pool
    // ---------------------------------------------------------------------------
    VkDescriptorPool _imguiDescriptorPool = VK_NULL_HANDLE;

    // ---------------------------------------------------------------------------
    // Window size (needed for swapchain recreation)
    // ---------------------------------------------------------------------------
    uint32_t _width  = 0;
    uint32_t _height = 0;
};

} // namespace Luna

#endif // LUNA_VULKAN_ENABLED
