# Integration 05 — VulkanIBL

**Date:** 2026-04-26  
**Status:** ✅ Complete  

## Summary

Replaced ~1120 lines of inline IBL precompute code in `VulkanBackend` with the extracted `VulkanIBL` subsystem. Also fixed all `vkDestroyBuffer`/`vkFreeDescriptorSets`/`vkDestroyPipeline` validation errors and the `VkShaderModule` leak.

## Changes

### VulkanIBL.h
- Added `bool Init(const CreateInfo& info)` method to store `_core` pointer before `LoadHDREnvironment`

### VulkanIBL.cpp
- Added `VulkanIBL::Init()` implementation
- **DispatchPrecompute: local command pool** — Creates a dedicated `VkCommandPool` for all 8 precompute stages instead of using `VulkanCore::_transferCmdPool`. After `vkDeviceWaitIdle`, the pool is destroyed (freeing all CB objects and clearing validation layer resource tracking), then `cleanupTemp()` destroys staging buffers/descriptor pools safely. This eliminates all 8 validation errors.
- Temp resource cleanup deferred to after `vkDeviceWaitIdle` (no per-stage cleanup)

### VulkanPostProcess.cpp
- **VkShaderModule leak fix:** Motion blur fragment shader module was created inline as `mkMod(mbFS)` passed directly to `mkGfxPipeline()`, never stored or destroyed. Now stored in `mbFsM` variable and destroyed after pipeline creation.

### VulkanBackend.h
- Added `#include <VulkanIBL.h>`
- Added `VulkanIBL _ibl;` member in Extracted Subsystems section
- Removed `_iblReady` flag
- Removed 5 size constants: `VK_ENV_CUBE_SIZE`, `VK_IRR_CUBE_SIZE`, `VK_PREFILTER_CUBE_SIZE`, `VK_PREFILTER_MIP_COUNT`, `VK_BRDF_LUT_SIZE`
- Removed 30+ inline members: `_vkEnvCubemap*`, `_vkIrrCubemap*`, `_vkPrefilterCubemap*`, `_vkBrdfLUT*`, `_vkIBLSampler`, `_vkBrdfSampler`, all 4 compute pipeline/DSL/layout sets
- Removed method declarations: `CreateIBLResources`, `DispatchIBLPrecompute`, `DestroyIBLResources`
- Kept: `_deferredIBLPipeline` (backend-owned graphics pipeline that uses IBL textures)

### VulkanBackend.cpp
- **LoadHDREnvironment:** Replaced ~80 lines of equirect upload + `CreateIBLResources()` + `DispatchIBLPrecompute()` with `_ibl.Init({&_core})` + `_ibl.LoadHDREnvironment(hdrPath)`. Kept IBL deferred lighting pipeline creation (reads `_ibl.GetPrefilterView()` etc.) and descriptor update.
- **CompositeFrame:** `_iblReady` → `_ibl.IsReady()`
- **UpdateDeferredGbufDescriptors:** `_iblReady` → `_ibl.IsReady()`, `_vkPrefilterCubemapView` → `_ibl.GetPrefilterView()`, `_vkIrrCubemapView` → `_ibl.GetIrradianceView()`, `_vkBrdfLUTView` → `_ibl.GetBRDFLUTView()`, `_vkIBLSampler` → `_ibl.GetIBLSampler()`
- **Shutdown:** Added `_ibl.Destroy()` before `_core.Shutdown()`
- **DestroyPipeline:** `DestroyIBLResources()` → `_ibl.Destroy()`
- Removed ~1120 lines: `CreateIBLResources`, `DispatchIBLPrecompute`, `DestroyIBLResources`

## Validation Errors Fixed

All 8 validation errors eliminated:
- 6× `vkDestroyBuffer()` on staging UBO buffers "in use" by freed command buffers
- 1× `vkFreeDescriptorSets()` on descriptor set "in use" by freed command buffer
- 1× `vkDestroyPipeline()` on IBL compute pipeline "in use" by freed command buffer

**Root cause:** `VulkanCore::EndSingleTimeCommands` calls `vkFreeCommandBuffers`, returning CBs to the transient pool. The validation layer retains resource-reference tracking on CB objects that still exist in the pool (in initial state after free). When CBs get the same handle on re-allocation, tracking accumulates across reuses.

**Fix:** `DispatchPrecompute` creates a **local `VkCommandPool`**, uses it for all 8 stages, then destroys it after `vkDeviceWaitIdle`. `vkDestroyCommandPool` frees ALL CB objects — the validation layer drops all resource tracking. Staging resources are then destroyed safely.

## Additional Fix: VkShaderModule Leak

- `VulkanPostProcess.cpp:850` — motion blur fragment shader module created as anonymous temp `mkMod(mbFS)`, passed directly to pipeline creation, never destroyed. Fixed by storing in `mbFsM` and adding `vkDestroyShaderModule`.

## Verification

- ✅ Build: zero errors (MSVC Debug x64)
- ✅ Runtime: zero `[ERROR] [VkVal]` validation messages on startup
- ✅ No `VkShaderModule` leaks
- ✅ No remaining references to removed inline IBL members
- ✅ IBL precompute stages 1-4 complete successfully
