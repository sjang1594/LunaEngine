# VulkanIBL — Implementation Document

**Date:** 2026-04-25  
**Status:** ✅ Complete (standalone)  
**Lines:** Header 108, Impl 500  

---

## Overview

VulkanIBL handles Image-Based Lighting: equirectangular HDR loading, cubemap conversion, irradiance/prefilter convolution, and BRDF LUT generation. All precompute runs via compute shaders with TDR-safe separate GPU submissions per stage.

---

## Responsibilities

| Responsibility | API |
|----------------|-----|
| HDR loading + precompute | `LoadHDREnvironment()` |
| Resource cleanup | `Destroy()` |
| Deferred lighting access | `GetIrradianceView()`, `GetPrefilterView()`, `GetBRDFLUTView()`, `GetIBLSampler()` |
| State query | `IsReady()` |

---

## Design Decisions

### 1. TDR-Safe Separate Submissions

Each precompute stage uses its own `BeginSingleTimeCommands()`/`EndSingleTimeCommands()`:
- Stage 1: Equirect → EnvCube
- Stage 2: Irradiance convolution
- Stage 3: Prefilter (one mip per submission)
- Stage 4: BRDF LUT

Heavy convolution kernels can trigger GPU TDR if batched.

### 2. Deferred IBL Pipeline External

The `_deferredIBLPipeline` stays in VulkanBackend since it depends on `_deferredPipeLayout` and `_ppRenderPass`. VulkanIBL only creates cubemaps/pipelines and exposes views for descriptor binding.

### 3. Temp Resources Cleaned Per Stage

Each stage's descriptor pools, UBOs, and staging buffers are cleaned immediately after submission. No accumulation across stages.

---

## Files

| File | Path |
|------|------|
| Header | `LunaEngine/src/LunaEngine/Renderer/Vulkan/Public/VulkanIBL.h` |
| Implementation | `LunaEngine/src/LunaEngine/Renderer/Vulkan/Private/VulkanIBL.cpp` |
| This doc | `Docs/progress/subsystem_07_vulkan_ibl.md` |

---

## Build Verification

```
Build succeeded.
0 Warning(s)
0 Error(s)
```

---

## Next Steps

1. **VulkanGBuffer** extraction (subsystem 8)

