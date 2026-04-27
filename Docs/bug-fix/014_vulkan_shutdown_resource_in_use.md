# Bug #014: Vulkan Shutdown Validation Errors — Resources Destroyed While In Use

**Date**: 2026-04-26  
**Severity**: Medium (validation errors, no crash)  
**Backend**: Vulkan  
**Status**: Fixed

---

## Symptoms

Vulkan validation layer errors during shutdown:

```
[ERROR] [VkVal] vkDestroyBuffer(): can't be called on VkBuffer 0x228... that is currently in use by VkCommandBuffer 0x245...
[ERROR] [VkVal] vkFreeDescriptorSets(): pDescriptorSets[0] can't be called on VkDescriptorSet 0x1f5... that is currently in use by VkCommandBuffer ...
[ERROR] [VkVal] vkDestroyPipeline(): can't be called on VkPipeline 0x108... that is currently in use by VkCommandBuffer ...
```

6 buffer errors (2 per frame × 3 frames), 1 descriptor set error, 1 pipeline error.

---

## Root Cause

`Application::Shutdown()` calls `ShutdownImGui()` **before** `Backend::Shutdown()`:

```cpp
void Application::Shutdown() {
    IRenderContext::ShutdownImGui();  // ← destroys ImGui resources FIRST
    IRenderContext::Shutdown();       // ← destroys command pools LATER
}
```

`ImGui_ImplVulkan_RenderDrawData()` records into our frame command buffers, binding ImGui's internal vertex/index buffers, pipeline, and descriptor sets. When `ImGui_ImplVulkan_Shutdown()` destroys those resources, the validation layer reports them as "in use" because the frame command buffers still hold implicit references.

The original fix (moving command pool destruction to the top of `Shutdown()`) was insufficient because `ShutdownImGui()` runs in a separate call **before** `Shutdown()` is ever entered.

### Additional Issues (from original investigation)

1. **Async compute command pools** (`_computeFrames[i].cmdPool`) were only destroyed inside `DestroyIndirectResources()` which ran late in the sequence.

2. **VulkanCore::Shutdown()** in non-owning mode nulled `_deviceRaw`, breaking subsequent cleanup code that needed the device handle.

---

## Fix

### File: `VulkanBackend.cpp` — Reset command pools in ShutdownImGui()

The key insight: `vkResetCommandPool` clears all recorded resource references from command buffers **without destroying the pools**. This must happen before ImGui destroys its resources.

```cpp
void VulkanBackend::ShutdownImGui() {
    if (ImGui::GetCurrentContext() == nullptr) return;

    // Reset all command pools BEFORE ImGui destroys its resources.
    // Frame command buffers hold references to ImGui's internal
    // vertex/index buffers and pipeline from ImGui_ImplVulkan_RenderDrawData().
    if (_device && _device->GetDevice() != VK_NULL_HANDLE) {
        VkDevice dev = _device->GetDevice();
        vkDeviceWaitIdle(dev);
        for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
            if (_frames[i].cmdPool)
                vkResetCommandPool(dev, _frames[i].cmdPool, 0);
            if (_computeFrames[i].cmdPool)
                vkResetCommandPool(dev, _computeFrames[i].cmdPool, 0);
        }
        if (_transferCmdPool)
            vkResetCommandPool(dev, _transferCmdPool, 0);
    }

    ImGuiIO& io = ImGui::GetIO();
    if (io.BackendRendererUserData != nullptr)
        ImGui_ImplVulkan_Shutdown();
    if (io.BackendPlatformUserData != nullptr)
        ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}
```

### File: `VulkanBackend.cpp` — Destroy command pools FIRST in Shutdown()

Command pools are still destroyed at the top of `Shutdown()` for resources bound during the rendering frame (deferred pipeline, scene CBs, etc.):

```cpp
void VulkanBackend::Shutdown() {
    if (!_device || _device->GetDevice() == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(_device->GetDevice());
    VkDevice dev = _device->GetDevice();

    // Reset + destroy ALL command pools to release resource references
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
        if (_frames[i].cmdPool) {
            vkResetCommandPool(dev, _frames[i].cmdPool, 0);
            vkDestroyCommandPool(dev, _frames[i].cmdPool, nullptr);
        }
        if (_computeFrames[i].cmdPool) {
            vkResetCommandPool(dev, _computeFrames[i].cmdPool, 0);
            vkDestroyCommandPool(dev, _computeFrames[i].cmdPool, nullptr);
        }
    }
    // ...then destroy subsystems and resources safely
```

### File: `VulkanCore.cpp` — Keep device pointer valid in non-owning mode

```cpp
if (!_ownsDevice) {
    // Don't null _deviceRaw — other cleanup code needs it.
    LUNA_LOG_INFO("VulkanCore: shutdown complete (non-owning mode)");
    return;
}
```

---

## Correct Vulkan Shutdown Order

```
1. vkDeviceWaitIdle()          — GPU finishes all work
2. Reset command pools         — clears implicit resource references from command buffers
3. Destroy ImGui resources     — safe now that no CB references ImGui buffers/pipeline
4. Destroy command pools       — frees command buffers
5. Destroy subsystems          — SSAO, shadows, Hi-Z, IBL, post-process
6. Destroy descriptor pools    — frees descriptor sets
7. Destroy pipelines           — graphics + compute + RT
8. Destroy buffers/images      — scene data, materials, render targets
9. Destroy samplers/render passes/framebuffers
10. Destroy swapchain
11. Destroy device
12. Destroy surface/instance
```

---

## Files Modified

| File | Change |
|------|--------|
| `Vulkan/Private/VulkanBackend.cpp` | Added command pool resets in `ShutdownImGui()` before `ImGui_ImplVulkan_Shutdown()` |
| `Vulkan/Private/VulkanBackend.cpp` | Command pool reset+destroy at top of `Shutdown()` |
| `Vulkan/Private/VulkanCore.cpp` | Removed `_deviceRaw = nullptr` in non-owning `Shutdown()` |
