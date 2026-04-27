    # VulkanGBuffer — Implementation Document

**Date:** 2026-04-25  
**Status:** ✅ Complete (standalone)  
**Lines:** Header 93, Impl 240  

---

## Overview

VulkanGBuffer owns the deferred G-buffer images (albedo, normal, metalrough) and both render pass variants (CLEAR + LOAD). Does NOT own the deferred lighting pipeline — that stays in VulkanBackend due to cross-cutting descriptor dependencies.

---

## Responsibilities

| Responsibility | API |
|----------------|-----|
| G-buffer lifecycle | `Create()`, `Destroy()`, `Resize()` |
| Image access | `GetAlbedoView()`, `GetNormalView()`, `GetMetalRoughView()` |
| Render pass access | `GetRenderPass()`, `GetRenderPassLoad()` |
| Framebuffer access | `GetFramebuffer()`, `GetFramebufferLoad()` |

---

## Design Decisions

### 1. External Depth View

Depth buffer is owned by VulkanSwapchain. VulkanGBuffer takes `depthView` as a parameter and uses it in framebuffer creation.

### 2. Two Render Pass Variants

- **CLEAR**: `LOAD_OP_CLEAR`, `initialLayout=UNDEFINED`. Used for standard geometry fill.
- **LOAD**: `LOAD_OP_LOAD`, `initialLayout=COLOR_ATTACHMENT_OPTIMAL`. Used when re-opening G-buffer after GPU cull pass.

### 3. Deferred Pipeline NOT Included

`UpdateDeferredGbufDescriptors()` writes 14 descriptor bindings from CSM, SSAO, IBL, RT shadows — 6+ subsystem dependencies. This cross-cutting concern stays in VulkanBackend as the coordinator.

---

## Files

| File | Path |
|------|------|
| Header | `LunaEngine/src/LunaEngine/Renderer/Vulkan/Public/VulkanGBuffer.h` |
| Implementation | `LunaEngine/src/LunaEngine/Renderer/Vulkan/Private/VulkanGBuffer.cpp` |
| This doc | `Docs/progress/subsystem_08_vulkan_gbuffer.md` |

---

## Build Verification

```
Build succeeded.
0 Warning(s)
0 Error(s)
```

---

## Next Steps

1. **VulkanGPUDriven** extraction (subsystem 9)

