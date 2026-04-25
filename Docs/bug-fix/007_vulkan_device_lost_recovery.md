# Bug #007: Vulkan Device Lost Recovery and Resize Synchronization

**Date:** 2024-04-24  
**Updated:** 2026-04-24  
**Status:** Fixed  
**Severity:** Critical  
**Component:** VulkanBackend resize/synchronization, IBL precompute

---

## Symptoms

```
[ERROR] VK: Device lost during image-in-flight fence wait
[ERROR] VK: Device lost during resize fence wait
[ERROR] VK: Device lost during queue submit
[ERROR] ImGui Vk: -4
```

The error code `-4` maps to `VK_ERROR_DEVICE_LOST`, indicating GPU TDR (Timeout Detection and Recovery) occurred.

---

## Root Cause Analysis

### Primary Issue: IBL Precompute GPU TDR

The IBL precompute dispatched ALL 4 stages (equirect→cube, irradiance convolution, prefilter 5 mips, BRDF LUT) in a **single command buffer submission**. Heavy convolution kernels (irradiance samples entire cubemap per pixel, prefilter does importance sampling) could exceed Windows' default TDR timeout (~2 seconds).

**Solution:** Split IBL precompute into **separate submissions per stage** with `vkQueueWaitIdle()` sync points between them:
- Stage 1: Equirect → EnvCube (1 submission)
- Stage 2: Irradiance convolution (1 submission)
- Stage 3: Prefilter mips (1 submission per mip = 5 submissions)
- Stage 4: BRDF LUT (1 submission)
- Total: 8 separate GPU submissions with sync points

### Secondary Issue: GLFW Resize Callback Race Condition

GLFW's `framebuffer_size_callback` fires during `glfwPollEvents()`, which executes inside the frame loop. Before the fix:

| Before | After |
|--------|-------|
| `Resize()` → immediate `RecreateSwapchain()` | `Resize()` → set `_pendingResize = true` |
| Framebuffers destroyed mid-recording | `BeginFrame()` checks flag before any recording |
| ❌ VkFramebuffer destroyed while in use | ✅ All GPU work finished, then safe recreate |

### Tertiary Issue: RT Descriptor Stale References

After `RecreateSwapchain()`, RT descriptor sets (`_vkRTDescSet[]`) still referenced destroyed image views:
- `_depthView` (depth buffer, recreated on resize)
- `_gbNormalView` (G-buffer normal, recreated on resize)
- `_vkShadowMaskView` (RT shadow output, recreated on resize)

### Quaternary Issue: Command Pool Contention

`BeginSingleTimeCommands()` was using `_frames[0].cmdPool`, which could race with frame rendering during async resource loading (e.g., IBL precompute).

### Quinary Issue: Render Graph Missing Dependencies

RT Shadows pass sampled depth and normal textures, but the render graph didn't declare these reads, causing missing barriers.

---

## Fixes Applied

### 1. Split IBL Precompute into Separate Submissions (TDR Prevention)
```cpp
// BEFORE: All stages in one command buffer
VkCommandBuffer cmd = BeginSingleTimeCommands();
// ... record equirect, irradiance, prefilter×5, brdfLUT ...
EndSingleTimeCommands(cmd);  // One massive GPU submission

// AFTER: Each stage is a separate submission
{
    VkCommandBuffer cmd = BeginSingleTimeCommands();
    // Stage 1: equirect → cube
    vkCmdDispatch(cmd, g, g, 6);
    EndSingleTimeCommands(cmd);  // Submit + vkQueueWaitIdle
}
{
    VkCommandBuffer cmd = BeginSingleTimeCommands();
    // Stage 2: irradiance convolution
    vkCmdDispatch(cmd, g, g, 6);
    EndSingleTimeCommands(cmd);  // Submit + sync
}
// Stage 3: One submission per prefilter mip level
for (mip = 0; mip < 5; mip++) {
    VkCommandBuffer cmd = BeginSingleTimeCommands();
    vkCmdDispatch(...);
    EndSingleTimeCommands(cmd);  // Submit + sync
}
// Stage 4: BRDF LUT
{
    VkCommandBuffer cmd = BeginSingleTimeCommands();
    vkCmdDispatch(cmd, g, g, 1);
    EndSingleTimeCommands(cmd);
}
```

**Benefits:**
- GPU completes work between stages, avoiding TDR timeout
- Proper resource cleanup after each stage (temp buffers, descriptor pools)
- Per-stage error reporting if device lost occurs
- Progress logging shows which stage completed

### 2. Deferred Resize Pattern
```cpp
// In Resize() callback:
_pendingResize = true;
_pendingResizeW = width;
_pendingResizeH = height;

// In BeginFrame() — before any command buffer recording:
if (_pendingResize)
{
    _pendingResize = false;
    // Wait for ALL in-flight frames
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++)
    {
        VkResult wr = vkWaitForFences(dev, 1, &_frames[i].fence, VK_TRUE, UINT64_MAX);
        if (wr == VK_ERROR_DEVICE_LOST) { _deviceLost = true; return; }
    }
    vkDeviceWaitIdle(dev);
    RecreateSwapchain();
    for (auto& f : _imagesInFlight) f = VK_NULL_HANDLE;
    _framesSinceResize = 0;
    return; // Critical: return to let next BeginFrame start clean
}
```

### 3. RT Descriptor Updates After Resize
```cpp
// In RecreateSwapchain(), after G-buffer recreation:
if (_rtSupported && _vkRTDescPool)
{
    // Recreate shadow mask at new resolution
    CreateImage(W, H, VK_FORMAT_R8_UNORM, ...);
    _vkShadowMaskView = CreateImageView(...);

    // Update all per-frame RT descriptor sets
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++)
    {
        VkDescriptorImageInfo shadowII{ VK_NULL_HANDLE, _vkShadowMaskView, VK_IMAGE_LAYOUT_GENERAL };
        VkDescriptorImageInfo depthII { _pointClampSampler, _depthView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo normalII{ _pointClampSampler, _gbNormalView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        // ... vkUpdateDescriptorSets
    }
}
```

### 4. Dedicated Transfer Command Pool
```cpp
// In CreateFrameResources():
VkCommandPoolCreateInfo tpi{};
tpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
tpi.queueFamilyIndex = _device->GetGraphicsQueueFamily();
tpi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
vkCreateCommandPool(dev, &tpi, nullptr, &_transferCmdPool);

// In BeginSingleTimeCommands():
ai.commandPool = _transferCmdPool;  // Not _frames[0].cmdPool
```

### 5. Render Graph RT Dependencies
```cpp
rg.AddPass("RT Shadows")
    .Read(hDepth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
          VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_SHADER_READ_BIT)
    .Read(hGBNormal, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_SHADER_READ_BIT)
    .Write(hShadowMask, VK_IMAGE_LAYOUT_GENERAL,
           VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_SHADER_WRITE_BIT)
    // ...
```

### 6. Resize Cooldown for RT
```cpp
// Skip RT for 2 frames after resize to allow GPU state to stabilize
bool rtActive = _rtSupported && _vkRTPipeline != VK_NULL_HANDLE 
                && _vkShadowMaskImage != VK_NULL_HANDLE
                && _framesSinceResize >= 2;
```

### 7. Device Lost State Tracking
```cpp
bool _deviceLost = false;

// Checked at entry of BeginFrame, CompositeFrame, EndFrame:
if (_deviceLost) return;

// Set on any VK_ERROR_DEVICE_LOST return from Vulkan calls
```

---

## Recovery Procedure

`VK_ERROR_DEVICE_LOST` is a "sticky" GPU state that persists until the driver resets.

**To recover:**
1. **Quick method:** Press `Win + Ctrl + Shift + B` to reset the GPU driver
2. **Alternative:** Reboot the system
3. **Verify:** Run the application again

---

## Files Modified

| File | Changes |
|------|---------|
| `VulkanBackend.h` | Added `_transferCmdPool`, `_deviceLost`, `_framesSinceResize` |
| `VulkanBackend.cpp` | Deferred resize, RT descriptor updates, dedicated transfer pool, render graph dependencies |

---

## Validation

After applying fixes:
1. Run `--vulkan` mode
2. Resize window multiple times rapidly
3. Verify no validation errors in console
4. Verify no `VK_ERROR_DEVICE_LOST` occurs
5. RT shadows should render correctly at all resolutions

---

## References

- [Vulkan Spec: VK_ERROR_DEVICE_LOST](https://registry.khronos.org/vulkan/specs/1.3-extensions/man/html/VK_ERROR_DEVICE_LOST.html)
- [NVIDIA GPU TDR](https://developer.nvidia.com/timeout-detection-recovery-nvidia-gpus)
- [Vulkan Synchronization Wiki](https://github.com/KhronosGroup/Vulkan-Docs/wiki/Synchronization-Examples)

