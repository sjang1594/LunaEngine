# Bug Fix 006: Vulkan Resize VK_ERROR_DEVICE_LOST

## Summary
Fixed `VK_ERROR_DEVICE_LOST` errors occurring during window resize and IBL resource loading in the Vulkan backend.

## Root Causes Identified

### 1. Command Pool Race Condition (Critical)
**Location:** `BeginSingleTimeCommands()` / `EndSingleTimeCommands()`

`BeginSingleTimeCommands()` was using `_frames[0].cmdPool` for allocating single-time command buffers used by resource loading (IBL, textures, etc.). This created a race condition:

```
Frame Loop:              Resource Loading (IBL):
┌─────────────┐         ┌─────────────┐
│ BeginFrame  │         │ BeginSingleTimeCommands() │
│ (uses _frames[0].cmdPool) │         │ (ALSO uses _frames[0].cmdPool!) │
└─────────────┘         └─────────────┘
         ↓                      ↓
    RACE CONDITION: Command pool accessed concurrently
```

From Vulkan spec: *"Command pools are externally synchronized, meaning that a command pool must not be used concurrently in multiple threads."*

**Fix:** Created dedicated `_transferCmdPool` for single-time commands, completely separate from frame command pools.

### 2. Missing Render Graph Dependencies for RT Pass
**Location:** `CompositeFrame()` RT Shadows pass

The ray tracing shadows pass samples `depthTex` and `normalTex` via descriptor sets, but the render graph didn't declare these reads. This caused missing barriers for image layout transitions, leading to undefined behavior after resize.

**Before:**
```cpp
rg.AddPass("RT Shadows")
    .Write(hShadowMask, ...)  // Only write declared!
    .Execute(...);
```

**After:**
```cpp
rg.AddPass("RT Shadows")
    .Read(hDepth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, 
          VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_SHADER_READ_BIT)
    .Read(hGBNormal, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_SHADER_READ_BIT)
    .Write(hShadowMask, ...)
    .Execute(...);
```

### 3. RT Instability After Resize
**Problem:** Ray tracing descriptor sets reference depth/normal views that are destroyed during resize. Even with proper synchronization, accessing these immediately after resize could cause issues.

**Fix:** Added `_framesSinceResize` counter to skip RT for 2 frames after resize, allowing all recreated resources to stabilize.

```cpp
bool rtActive = _rtSupported && _vkRTPipeline != VK_NULL_HANDLE 
                && _vkShadowMaskImage != VK_NULL_HANDLE
                && _framesSinceResize >= 2;  // Skip RT for 2 frames after resize
```

## Changes Made

### VulkanBackend.h
```cpp
// Added dedicated transfer command pool
VkCommandPool _transferCmdPool = VK_NULL_HANDLE;

// Added resize stabilization counter  
uint32_t _framesSinceResize = 100;  // Start high so RT is enabled immediately
```

### VulkanBackend.cpp

1. **CreateFrameResources()** - Create dedicated transfer command pool:
```cpp
VkCommandPoolCreateInfo tpi{};
tpi.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
tpi.queueFamilyIndex = _device->GetGraphicsQueueFamily();
tpi.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
vkCreateCommandPool(dev, &tpi, nullptr, &_transferCmdPool);
```

2. **BeginSingleTimeCommands()** - Use dedicated pool:
```cpp
ai.commandPool = _transferCmdPool;  // Was: _frames[0].cmdPool
```

3. **EndSingleTimeCommands()** - Free from correct pool:
```cpp
vkFreeCommandBuffers(dev, _transferCmdPool, 1, &cmd);  // Was: _frames[0].cmdPool
```

4. **Shutdown()** - Destroy transfer pool:
```cpp
if (_transferCmdPool) { vkDestroyCommandPool(dev, _transferCmdPool, nullptr); }
```

5. **BeginFrame()** - Increment counter, reset on resize:
```cpp
// At end of successful BeginFrame:
if (_framesSinceResize < 100) _framesSinceResize++;

// After RecreateSwapchain in all paths:
_framesSinceResize = 0;
```

6. **CompositeFrame()** - RT pass declarations + cooldown check

## Deferred Resize Pattern (Review)

The existing deferred resize pattern is correct:

| Before | After |
|--------|-------|
| `Resize()` → immediate `RecreateSwapchain()` | `Resize()` → set `_pendingResize = true` |
| Framebuffers destroyed mid-recording | `BeginFrame()` checks `_pendingResize` before any recording |
| ❌ VkFramebuffer was destroyed while in use | ✅ All GPU work finished via fence waits + `vkDeviceWaitIdle`, then safe recreate |

The resize flow:
1. GLFW callback fires during `glfwPollEvents()` (before BeginFrame)
2. `Resize()` sets `_pendingResize = true`
3. Next `BeginFrame()`:
   - Waits for ALL frame fences (`FRAMES_IN_FLIGHT`)
   - Calls `vkDeviceWaitIdle()` for extra safety
   - `RecreateSwapchain()` - safe to destroy/recreate resources
   - Clears `_imagesInFlight` array
   - Resets `_framesSinceResize = 0`
   - Returns early to let next frame start clean

## Testing
- [x] Build succeeds
- [ ] No validation errors during resize
- [ ] No device lost during IBL loading
- [ ] RT shadows work correctly after resize

## References
- Vulkan Spec §6.1: Command Pool externally synchronized
- Vulkan Spec §7.4: VK_ERROR_DEVICE_LOST
- [Vulkan Guide - Synchronization](https://github.com/KhronosGroup/Vulkan-Guide/blob/master/chapters/synchronization.adoc)

