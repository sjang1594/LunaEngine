# Integration 03 — VulkanShadows

**Date:** 2026-04-26  
**Status:** ✅ Complete  

## Summary

Replaced ~380 lines of inline CSM (Cascaded Shadow Maps) code in `VulkanBackend` with the extracted `VulkanShadows` subsystem.

## Changes

### VulkanBackend.h
- Added `#include <VulkanShadows.h>`
- Added `VulkanShadows _shadows;` member in Extracted Subsystems section
- Removed method declarations: `CreateCSMResources`, `DestroyCSMResources`, `UpdateCSMMatrices`, `DrawCSMPass`
- Removed inline members: `_csmImage`, `_csmMemory`, `_csmLayerView[4]`, `_csmArrayView`, `_csmRenderPass`, `_csmFramebuffers[4]`, `_csmPipelineLayout`, `_csmPipeline`, `_csmSampler`, `_csmLightVP[4]`, `_csmSplits[4]`
- Removed constants: `CSM_CASCADE_COUNT`, `CSM_SHADOW_SIZE`
- **Retained** `_lastMeshModels` — still needed to build `ShadowDraw` list each frame

### VulkanBackend.cpp
- **Init:** `CreateCSMResources()` → `_shadows.Create({&_core})`
- **Shutdown:** `DestroyCSMResources()` → `_shadows.Destroy()`
- **BeginFrame:** Build `std::vector<VulkanShadows::ShadowDraw>` from `_vkSceneMeshes` + `_lastMeshModels`, call `_shadows.UpdateMatrices()` + `_shadows.DrawPass()`
- **CompositeFrame UBO:** `_csmLightVP[c]` → `_shadows.GetLightVPs()[c]`, `_csmSplits` → `_shadows.GetCascadeSplits()`
- **UpdateDeferredGbufDescriptors:** `_csmArrayView` → `_shadows.GetArrayView()`, `_csmSampler` → `_shadows.GetSampler()`
- Removed ~380 lines: `CreateCSMResources`, `DestroyCSMResources`, `UpdateCSMMatrices`, `DrawCSMPass`

## Verification

- ✅ Build: zero errors (MSVC Debug x64)
- ✅ No remaining references to removed `_csm*` members or `CSM_CASCADE_COUNT`/`CSM_SHADOW_SIZE`

