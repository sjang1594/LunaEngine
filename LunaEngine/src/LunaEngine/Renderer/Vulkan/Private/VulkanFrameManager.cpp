#include "LunaPCH.h"
#include "LunaEngine/Renderer/Vulkan/Public/VulkanFrameManager.h"
#include "LunaEngine/Renderer/Vulkan/Public/VulkanCore.h"
#include "Logger/Logger.h"

namespace Luna
{

// ===========================================================================
// Constants
// ===========================================================================
static constexpr VkDeviceSize MVP_UBO_SIZE   = VulkanFrameManager::MAX_DRAWS_PER_FRAME * 256;  // 16KB
static constexpr VkDeviceSize SCENE_UBO_SIZE = 256;

// ===========================================================================
// Destructor
// ===========================================================================
VulkanFrameManager::~VulkanFrameManager()
{
    Destroy();
}

// ===========================================================================
// Create
// ===========================================================================
bool VulkanFrameManager::Create(const CreateInfo& info)
{
    if (!info.core)
    {
        LUNA_LOG_ERROR("VulkanFrameManager: null core");
        return false;
    }

    _core = info.core;

    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; ++i)
    {
        if (!CreateCommandResources(_frames[i]))
        {
            LUNA_LOG_ERROR("VulkanFrameManager: failed to create command resources for frame %u", i);
            Destroy();
            return false;
        }

        if (!CreateSyncObjects(_frames[i]))
        {
            LUNA_LOG_ERROR("VulkanFrameManager: failed to create sync objects for frame %u", i);
            Destroy();
            return false;
        }

        if (!CreateUBOs(_frames[i]))
        {
            LUNA_LOG_ERROR("VulkanFrameManager: failed to create UBOs for frame %u", i);
            Destroy();
            return false;
        }
    }

    LUNA_LOG_INFO("VulkanFrameManager: created %u frames, MVP UBO %llu KB each",
                  FRAMES_IN_FLIGHT, MVP_UBO_SIZE / 1024);
    return true;
}

// ===========================================================================
// Destroy
// ===========================================================================
void VulkanFrameManager::Destroy()
{
    if (!_core) return;

    VkDevice dev = _core->GetDevice();
    if (dev)
    {
        vkDeviceWaitIdle(dev);

        for (auto& frame : _frames)
        {
            DestroyFrame(frame);
        }
    }

    _core = nullptr;
    _frameIndex = 0;
    _frameActive = false;

    LUNA_LOG_INFO("VulkanFrameManager: destroyed");
}

// ===========================================================================
// BeginFrame
// ===========================================================================
bool VulkanFrameManager::BeginFrame()
{
    if (_core->IsDeviceLost())
    {
        _frameActive = false;
        return false;
    }

    VkDevice dev = _core->GetDevice();
    FrameResource& frame = _frames[_frameIndex];

    // 1. Wait for this frame slot's previous GPU work to complete
    VkResult waitResult = vkWaitForFences(dev, 1, &frame.fence, VK_TRUE, UINT64_MAX);
    if (waitResult == VK_ERROR_DEVICE_LOST)
    {
        LUNA_LOG_ERROR("VulkanFrameManager: device lost during fence wait");
        _core->SetDeviceLost();
        _frameActive = false;
        return false;
    }

    // 2. Reset fence for this frame
    vkResetFences(dev, 1, &frame.fence);

    // 3. Reset command pool (implicitly resets all command buffers from this pool)
    vkResetCommandPool(dev, frame.cmdPool, 0);

    // 4. Begin command buffer recording
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    VkResult beginResult = vkBeginCommandBuffer(frame.cmdBuffer, &beginInfo);
    if (beginResult != VK_SUCCESS)
    {
        LUNA_LOG_ERROR("VulkanFrameManager: failed to begin command buffer: %d", static_cast<int>(beginResult));
        _frameActive = false;
        return false;
    }

    _frameActive = true;
    return true;
}

// ===========================================================================
// EndFrame
// ===========================================================================
void VulkanFrameManager::EndFrame(VkQueue queue, VkSemaphore waitSemaphore, VkPipelineStageFlags waitStage)
{
    if (!_frameActive || _core->IsDeviceLost())
    {
        return;
    }

    FrameResource& frame = _frames[_frameIndex];

    // End command buffer recording
    VkResult endResult = vkEndCommandBuffer(frame.cmdBuffer);
    if (endResult != VK_SUCCESS)
    {
        LUNA_LOG_ERROR("VulkanFrameManager: failed to end command buffer: %d", static_cast<int>(endResult));
        _frameActive = false;
        return;
    }

    // Submit
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

    // Wait for image acquisition before color attachment output
    VkSemaphore waitSemaphores[] = { waitSemaphore };
    VkPipelineStageFlags waitStages[] = { waitStage };
    submitInfo.waitSemaphoreCount = (waitSemaphore != VK_NULL_HANDLE) ? 1 : 0;
    submitInfo.pWaitSemaphores    = waitSemaphores;
    submitInfo.pWaitDstStageMask  = waitStages;

    // Command buffer
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers    = &frame.cmdBuffer;

    // Signal render done semaphore
    VkSemaphore signalSemaphores[] = { frame.renderDone };
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores    = signalSemaphores;

    // Submit with fence
    VkResult submitResult = vkQueueSubmit(queue, 1, &submitInfo, frame.fence);
    if (submitResult == VK_ERROR_DEVICE_LOST)
    {
        LUNA_LOG_ERROR("VulkanFrameManager: device lost during queue submit");
        _core->SetDeviceLost();
    }
    else if (submitResult != VK_SUCCESS)
    {
        LUNA_LOG_ERROR("VulkanFrameManager: queue submit failed: %d", static_cast<int>(submitResult));
    }

    _frameActive = false;
}

// ===========================================================================
// AdvanceFrame
// ===========================================================================
void VulkanFrameManager::AdvanceFrame()
{
    _frameIndex = (_frameIndex + 1) % FRAMES_IN_FLIGHT;
}

// ===========================================================================
// Current Frame Accessors
// ===========================================================================
VkCommandBuffer VulkanFrameManager::GetCurrentCommandBuffer() const
{
    return _frames[_frameIndex].cmdBuffer;
}

VkFence VulkanFrameManager::GetCurrentFence() const
{
    return _frames[_frameIndex].fence;
}

VkSemaphore VulkanFrameManager::GetCurrentImageReadySemaphore() const
{
    return _frames[_frameIndex].imageReady;
}

VkSemaphore VulkanFrameManager::GetCurrentRenderDoneSemaphore() const
{
    return _frames[_frameIndex].renderDone;
}

void* VulkanFrameManager::GetMVPMapped() const
{
    return _frames[_frameIndex].mvpMapped;
}

void* VulkanFrameManager::GetSceneMapped() const
{
    return _frames[_frameIndex].sceneMapped;
}

VkBuffer VulkanFrameManager::GetMVPBuffer() const
{
    return _frames[_frameIndex].mvpBuffer;
}

VkBuffer VulkanFrameManager::GetSceneBuffer() const
{
    return _frames[_frameIndex].sceneBuffer;
}

VkFence VulkanFrameManager::GetFenceAt(uint32_t index) const
{
    if (index >= FRAMES_IN_FLIGHT) return VK_NULL_HANDLE;
    return _frames[index].fence;
}

// ===========================================================================
// CreateCommandResources
// ===========================================================================
bool VulkanFrameManager::CreateCommandResources(FrameResource& frame)
{
    VkDevice dev = _core->GetDevice();

    // Command pool (resettable)
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = _core->GetGraphicsQueueFamily();
    poolInfo.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    if (vkCreateCommandPool(dev, &poolInfo, nullptr, &frame.cmdPool) != VK_SUCCESS)
    {
        LUNA_LOG_ERROR("VulkanFrameManager: failed to create command pool");
        return false;
    }

    // Command buffer
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool        = frame.cmdPool;
    allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    if (vkAllocateCommandBuffers(dev, &allocInfo, &frame.cmdBuffer) != VK_SUCCESS)
    {
        LUNA_LOG_ERROR("VulkanFrameManager: failed to allocate command buffer");
        return false;
    }

    return true;
}

// ===========================================================================
// CreateSyncObjects
// ===========================================================================
bool VulkanFrameManager::CreateSyncObjects(FrameResource& frame)
{
    VkDevice dev = _core->GetDevice();

    // Fence (start signaled so first frame doesn't wait forever)
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    if (vkCreateFence(dev, &fenceInfo, nullptr, &frame.fence) != VK_SUCCESS)
    {
        LUNA_LOG_ERROR("VulkanFrameManager: failed to create fence");
        return false;
    }

    // Semaphores
    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    if (vkCreateSemaphore(dev, &semInfo, nullptr, &frame.imageReady) != VK_SUCCESS)
    {
        LUNA_LOG_ERROR("VulkanFrameManager: failed to create imageReady semaphore");
        return false;
    }

    if (vkCreateSemaphore(dev, &semInfo, nullptr, &frame.renderDone) != VK_SUCCESS)
    {
        LUNA_LOG_ERROR("VulkanFrameManager: failed to create renderDone semaphore");
        return false;
    }

    return true;
}

// ===========================================================================
// CreateUBOs
// ===========================================================================
bool VulkanFrameManager::CreateUBOs(FrameResource& frame)
{
    VkDevice dev = _core->GetDevice();

    // MVP UBO (HOST_VISIBLE | HOST_COHERENT for persistent mapping)
    if (!_core->CreateBuffer(
            MVP_UBO_SIZE,
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            frame.mvpBuffer, frame.mvpMemory))
    {
        LUNA_LOG_ERROR("VulkanFrameManager: failed to create MVP UBO");
        return false;
    }

    // Map MVP buffer persistently
    if (vkMapMemory(dev, frame.mvpMemory, 0, MVP_UBO_SIZE, 0, &frame.mvpMapped) != VK_SUCCESS)
    {
        LUNA_LOG_ERROR("VulkanFrameManager: failed to map MVP UBO");
        return false;
    }

    // Scene UBO
    if (!_core->CreateBuffer(
            SCENE_UBO_SIZE,
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            frame.sceneBuffer, frame.sceneMemory))
    {
        LUNA_LOG_ERROR("VulkanFrameManager: failed to create Scene UBO");
        return false;
    }

    // Map Scene buffer persistently
    if (vkMapMemory(dev, frame.sceneMemory, 0, SCENE_UBO_SIZE, 0, &frame.sceneMapped) != VK_SUCCESS)
    {
        LUNA_LOG_ERROR("VulkanFrameManager: failed to map Scene UBO");
        return false;
    }

    return true;
}

// ===========================================================================
// DestroyFrame
// ===========================================================================
void VulkanFrameManager::DestroyFrame(FrameResource& frame)
{
    VkDevice dev = _core->GetDevice();

    // Unmap and destroy UBOs
    if (frame.mvpMapped)
    {
        vkUnmapMemory(dev, frame.mvpMemory);
        frame.mvpMapped = nullptr;
    }
    if (frame.mvpBuffer)
    {
        vkDestroyBuffer(dev, frame.mvpBuffer, nullptr);
        frame.mvpBuffer = VK_NULL_HANDLE;
    }
    if (frame.mvpMemory)
    {
        vkFreeMemory(dev, frame.mvpMemory, nullptr);
        frame.mvpMemory = VK_NULL_HANDLE;
    }

    if (frame.sceneMapped)
    {
        vkUnmapMemory(dev, frame.sceneMemory);
        frame.sceneMapped = nullptr;
    }
    if (frame.sceneBuffer)
    {
        vkDestroyBuffer(dev, frame.sceneBuffer, nullptr);
        frame.sceneBuffer = VK_NULL_HANDLE;
    }
    if (frame.sceneMemory)
    {
        vkFreeMemory(dev, frame.sceneMemory, nullptr);
        frame.sceneMemory = VK_NULL_HANDLE;
    }

    // Destroy sync objects
    if (frame.fence)
    {
        vkDestroyFence(dev, frame.fence, nullptr);
        frame.fence = VK_NULL_HANDLE;
    }
    if (frame.imageReady)
    {
        vkDestroySemaphore(dev, frame.imageReady, nullptr);
        frame.imageReady = VK_NULL_HANDLE;
    }
    if (frame.renderDone)
    {
        vkDestroySemaphore(dev, frame.renderDone, nullptr);
        frame.renderDone = VK_NULL_HANDLE;
    }

    // Destroy command pool (implicitly frees command buffers)
    if (frame.cmdPool)
    {
        vkDestroyCommandPool(dev, frame.cmdPool, nullptr);
        frame.cmdPool = VK_NULL_HANDLE;
    }
    frame.cmdBuffer = VK_NULL_HANDLE;
}

} // namespace Luna

