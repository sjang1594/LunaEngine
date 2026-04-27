#pragma once

#include <vulkan/vulkan.h>
#include <array>

namespace Luna
{

class VulkanCore;

/**
 * @brief Manages per-frame resources: command pools, fences, semaphores, UBOs.
 * 
 * Implements the frames-in-flight pattern for CPU-GPU overlap:
 * - FRAMES_IN_FLIGHT command buffer slots allow CPU to record ahead
 * - Fences ensure safe command buffer reuse
 * - Semaphores coordinate swapchain image acquisition/presentation
 * 
 * Thread Safety: NOT thread-safe. Call from main render thread only.
 */
class VulkanFrameManager
{
public:
    static constexpr uint32_t FRAMES_IN_FLIGHT = 3;
    static constexpr uint32_t MAX_DRAWS_PER_FRAME = 64;

    struct CreateInfo
    {
        VulkanCore* core = nullptr;
    };

    VulkanFrameManager() = default;
    ~VulkanFrameManager();

    // Non-copyable
    VulkanFrameManager(const VulkanFrameManager&) = delete;
    VulkanFrameManager& operator=(const VulkanFrameManager&) = delete;

    /**
     * @brief Create per-frame resources: command pools, fences, semaphores, UBOs.
     */
    bool Create(const CreateInfo& info);

    /**
     * @brief Destroy all per-frame resources.
     */
    void Destroy();

    /**
     * @brief Begin frame: wait for fence, reset command buffer, begin recording.
     * @return true if frame can proceed, false on device lost or error
     */
    bool BeginFrame();

    /**
     * @brief End frame: end command buffer recording and submit.
     * @param queue Queue to submit on
     * @param waitSemaphore Semaphore to wait before execution (e.g., image ready)
     * @param waitStage Pipeline stage to wait at
     */
    void EndFrame(VkQueue queue, VkSemaphore waitSemaphore, 
                  VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    /**
     * @brief Advance to next frame slot.
     * Call after Present, before next BeginFrame.
     */
    void AdvanceFrame();

    // === Frame State ===
    
    uint32_t GetFrameIndex() const { return _frameIndex; }
    bool     IsFrameActive() const { return _frameActive; }

    // === Current Frame Resources ===
    
    VkCommandBuffer GetCurrentCommandBuffer() const;
    VkFence         GetCurrentFence() const;
    VkSemaphore     GetCurrentImageReadySemaphore() const;
    VkSemaphore     GetCurrentRenderDoneSemaphore() const;

    // === Per-Frame UBO Access ===
    
    /**
     * @brief Get mapped MVP UBO for current frame.
     * Layout: MAX_DRAWS_PER_FRAME × 256 bytes (model/view/proj per draw)
     */
    void* GetMVPMapped() const;

    /**
     * @brief Get mapped Scene UBO for current frame.
     * Layout: 256 bytes (eye position, light params)
     */
    void* GetSceneMapped() const;

    /**
     * @brief Get MVP buffer handle for descriptor binding.
     */
    VkBuffer GetMVPBuffer() const;

    /**
     * @brief Get Scene buffer handle for descriptor binding.
     */
    VkBuffer GetSceneBuffer() const;

    // === Frame Resource at Index ===
    
    VkFence GetFenceAt(uint32_t index) const;

private:
    /**
     * @brief Per-frame resource bundle.
     * Each frame slot has independent command recording and UBO state.
     */
    struct FrameResource
    {
        VkCommandPool   cmdPool   = VK_NULL_HANDLE;
        VkCommandBuffer cmdBuffer = VK_NULL_HANDLE;
        VkFence         fence     = VK_NULL_HANDLE;
        VkSemaphore     imageReady = VK_NULL_HANDLE;  // Signaled by AcquireNextImage
        VkSemaphore     renderDone = VK_NULL_HANDLE;  // Signaled after submit, waited by Present

        // MVP UBO: MAX_DRAWS × 256 bytes (dynamic offset)
        VkBuffer        mvpBuffer = VK_NULL_HANDLE;
        VkDeviceMemory  mvpMemory = VK_NULL_HANDLE;
        void*           mvpMapped = nullptr;

        // Scene UBO: 256 bytes (static)
        VkBuffer        sceneBuffer = VK_NULL_HANDLE;
        VkDeviceMemory  sceneMemory = VK_NULL_HANDLE;
        void*           sceneMapped = nullptr;
    };

    bool CreateCommandResources(FrameResource& frame);
    bool CreateSyncObjects(FrameResource& frame);
    bool CreateUBOs(FrameResource& frame);
    void DestroyFrame(FrameResource& frame);

    VulkanCore* _core = nullptr;
    
    std::array<FrameResource, FRAMES_IN_FLIGHT> _frames;
    uint32_t _frameIndex  = 0;
    bool     _frameActive = false;
};

} // namespace Luna

