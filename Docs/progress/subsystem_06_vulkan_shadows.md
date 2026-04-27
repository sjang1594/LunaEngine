# VulkanShadows — Implementation Document

**Date:** 2026-04-25  
**Status:** ✅ Complete (standalone)  
**Lines:** Header 100, Impl 370  

---

## Overview

VulkanShadows implements Cascaded Shadow Maps (CSM) — 4 cascades at 2048×2048, D32_SFLOAT, with practical split scheme and orthographic light VP computation.

---

## Responsibilities

| Responsibility | API |
|----------------|-----|
| CSM lifecycle | `Create()`, `Destroy()` |
| Cascade matrices | `UpdateMatrices()` — splits + light VP per cascade |
| Shadow rendering | `DrawPass()` — depth-only render into 4 cascades |
| Deferred access | `GetArrayView()`, `GetSampler()`, `GetLightVPs()`, `GetCascadeSplits()` |

---

## Design Decisions

### 1. Mesh Data Passed via ShadowDraw Struct

`DrawPass()` takes `std::vector<ShadowDraw>` instead of accessing internal scene mesh arrays. Each `ShadowDraw` contains:
- `VkBuffer vertexBuffer, indexBuffer`
- `uint32_t indexCount`
- `XMFLOAT4X4 model`

This decouples VulkanShadows from scene management.

### 2. Push Constants for Light MVP

Each draw uses a 64-byte push constant (float4x4 lightMVP = model × lightVP). No descriptor sets needed — minimal binding overhead.

### 3. Front-Face Culling + Depth Bias

Reduces shadow acne via:
- `cullMode = VK_CULL_MODE_FRONT_BIT`
- `depthBiasConstantFactor = 1.25f, depthBiasSlopeFactor = 1.75f`

### 4. Initial Transition to READ_ONLY

CSM image transitions to `DEPTH_STENCIL_READ_ONLY_OPTIMAL` at creation time so the first frame's deferred lighting read is valid (even before any shadows are rendered).

### 5. Hardcoded Split Parameters

Near/far planes (0.1/100.0) and lambda (0.5) match the original VulkanBackend values. Light direction is fixed at (1, 2, 1). These can be parameterized in a future pass if needed.

---

## Files

| File | Path |
|------|------|
| Header | `LunaEngine/src/LunaEngine/Renderer/Vulkan/Public/VulkanShadows.h` |
| Implementation | `LunaEngine/src/LunaEngine/Renderer/Vulkan/Private/VulkanShadows.cpp` |
| This doc | `Docs/progress/subsystem_06_vulkan_shadows.md` |

---

## Build Verification

```
Build succeeded.
0 Warning(s)
0 Error(s)
```

---

## Integration Status

Not yet integrated into VulkanBackend. Backend still contains CSM members at lines 266-281 and methods at lines 3223-3600.

---

## Next Steps

1. **VulkanIBL** extraction (subsystem 7)

