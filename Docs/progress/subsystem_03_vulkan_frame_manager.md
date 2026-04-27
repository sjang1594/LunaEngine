# VulkanFrameManager — Implementation Document

**Date:** 2026-04-25  
**Status:** ✅ Complete (standalone)  
**Lines:** Header 148, Impl 436  

---

## Overview

VulkanFrameManager manages per-frame GPU resources: command pools/buffers, fences, semaphores, and per-frame UBOs (MVP + Scene). Implements the frames-in-flight pattern for CPU-GPU overlap.

---

## Responsibilities

| Responsibility | API |
|----------------|-----|
| Frame lifecycle | `BeginFrame()`, `EndFrame()`, `AdvanceFrame()` |
| Command recording | `GetCurrentCommandBuffer()` |
| Sync objects | `GetCurrentFence()`, `GetCurrentImageReadySemaphore()`, `GetCurrentRenderDoneSemaphore()` |
| Per-frame UBOs | `GetMVPMapped()`, `GetSceneMapped()`, `GetMVPBuffer()`, `GetSceneBuffer()` |
| Device lost | Propagates to `VulkanCore::SetDeviceLost()` |

---

## Design Decisions

### 1. Per-Frame Command Pool (not shared)

Each frame slot owns its own `VkCommandPool` with `VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT`. Reset entire pool at `BeginFrame()` instead of individual command buffers.

**Trade-off**: More pools, but simpler and avoids cross-frame state leakage.

### 2. Fence Created Signaled

```cpp
fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
```

First frame doesn't wait forever. `vkWaitForFences` immediately returns on first use.

### 3. UBO Persistent Mapping

MVP and Scene UBOs use `HOST_VISIBLE | HOST_COHERENT` with persistent `vkMapMemory`. No per-frame map/unmap overhead.

- MVP: `MAX_DRAWS_PER_FRAME × 256 = 16KB` (dynamic offset per draw)
- Scene: `256 bytes` (eye position, light params)

### 4. Submit Wait/Signal Semantics

```
EndFrame():
  wait:   waitSemaphore (imageReady from swapchain acquire)
  signal: renderDone (for Present)
  fence:  per-frame fence (for next BeginFrame CPU wait)
```

### 5. Descriptor Sets Deferred

Per-frame descriptor sets are NOT in VulkanFrameManager. They remain in VulkanBackend because:
- Layout depends on pipeline configuration
- Multiple descriptor set layouts exist (MVP, deferred scene, SSAO, etc.)
- VulkanFrameManager only owns the raw buffers; binding is caller's responsibility

---

## Memory Layout

```
VulkanFrameManager (~400 bytes + 3 × FrameResource)
├── VulkanCore*     _core          (8B pointer)
├── FrameResource   _frames[3]     (3 × ~96B = 288B)
│   ├── VkCommandPool               (8B)
│   ├── VkCommandBuffer              (8B)
│   ├── VkFence                      (8B)
│   ├── VkSemaphore imageReady       (8B)
│   ├── VkSemaphore renderDone       (8B)
│   ├── VkBuffer mvpBuffer           (8B)
│   ├── VkDeviceMemory mvpMemory     (8B)
│   ├── void* mvpMapped              (8B)
│   ├── VkBuffer sceneBuffer         (8B)
│   ├── VkDeviceMemory sceneMemory   (8B)
│   └── void* sceneMapped            (8B)
├── uint32_t _frameIndex            (4B)
└── bool     _frameActive           (1B)
```

---

## Files

| File | Path |
|------|------|
| Header | `LunaEngine/src/LunaEngine/Renderer/Vulkan/Public/VulkanFrameManager.h` |
| Implementation | `LunaEngine/src/LunaEngine/Renderer/Vulkan/Private/VulkanFrameManager.cpp` |
| Plan | `Docs/progress/plan_03_vulkan_frame_manager.md` |
| This doc | `Docs/progress/subsystem_03_vulkan_frame_manager.md` |

---

## Build Verification

```
Build succeeded.
0 Warning(s)
0 Error(s)
```

---

## Integration Status

**Not yet integrated into VulkanBackend.** VulkanBackend still contains its own `VkFrameResource` struct and `_frames[]` array (lines 166-195). Integration will replace these with `_frameManager->` calls.

Note: VulkanCore and VulkanSwapchain are also not yet integrated. All three subsystems exist standalone. Backend integration will happen as a separate pass after all standalone subsystems are created.

---

## Next Steps

1. **VulkanHiZ** extraction (subsystem 4)
2. Backend integration pass (after all standalone subsystems exist)

