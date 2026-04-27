# Integration 04 — VulkanHiZ

**Date:** 2026-04-26  
**Status:** ✅ Complete  

## Summary

Replaced ~340 lines of inline Hi-Z occlusion culling code in `VulkanBackend` with the extracted `VulkanHiZ` subsystem.

## Changes

### VulkanBackend.h
- Added `#include <VulkanHiZ.h>`
- Added `VulkanHiZ _hiZ;` member in Extracted Subsystems section
- Removed method declarations: `CreateHiZResources`, `DestroyHiZResources`, `BuildHiZPyramid`
- Removed `HIZ_MAX_MIPS` constant
- Removed inline members: `_hizImage`, `_hizMemory`, `_hizMipView[HIZ_MAX_MIPS]`, `_hizFullView`, `_hizMipCount`, `_hizReady`, `_hizGenDescLayout`, `_hizGenDescPool`, `_hizGenDescSet[HIZ_MAX_MIPS]`, `_hizGenPipeLayout`, `_hizGenPipeline`, `_hizSampler`, `_hizParamsBuffer`, `_hizParamsMem`, `_hizParamsMapped`

### VulkanBackend.cpp
- **LoadMeshes:** `CreateHiZResources()` → `_hiZ.Create({&_core, _swapchainExtent, _depthView})` + cull descriptor set update using `_hiZ.GetParamsBuffer()`, `_hiZ.GetFullView()`, `_hiZ.GetSampler()`
- **CompositeFrame:** `_hizImage && _hizMipCount >= 2` → `_hiZ.GetMipCount() >= 2`, `BuildHiZPyramid(cmd)` → `_hiZ.BuildPyramid(cmd, _depthImage, _swapchainExtent)` + GPU profiler timestamps
- **FlushDraws:** Same Hi-Z build replacement + `_hizParamsMapped` → `_hiZ.GetParamsMapped()`, `_hizMipCount` → `_hiZ.GetMipCount()`
- **Cull push constants (async + fallback):** `_hizReady` → `_hiZ.IsReady()`, `_hizMipCount` → `_hiZ.GetMipCount()`
- **Shutdown:** Added `_hiZ.Destroy()` before `_core.Shutdown()`
- **DestroyPipeline:** Removed `DestroyHiZResources()` call
- **RecreateSwapchain:** `DestroyHiZResources/CreateHiZResources` → `_hiZ.Resize(extent, depthView)` + cull descriptor update
- Removed ~340 lines: `CreateHiZResources`, `DestroyHiZResources`, `BuildHiZPyramid`

### Bug fix (shadows shutdown order)
- Moved `_shadows.Destroy()` from `DestroyPipeline()` to `Shutdown()` before `_core.Shutdown()` — was being called after `_core.Shutdown()` which nullified the device pointer, causing all shadow resources to leak.

## Verification

- ✅ Build: zero errors (MSVC Debug x64)
- ✅ No remaining references to removed `_hiz*` members, `HIZ_MAX_MIPS`, `CreateHiZResources`, `DestroyHiZResources`, or `BuildHiZPyramid`

