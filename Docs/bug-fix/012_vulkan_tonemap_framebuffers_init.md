# Bug #012 — Vulkan Tonemap Framebuffers Not Created During Init

**Date:** 2026-04-26  
**Severity:** CRITICAL — black screen, no rendering output  
**Root Cause:** `CreateFramebuffers()` was never called during `Init()`, only during `RecreateSwapchain()`

## Symptom

Vulkan renderer output was completely black. Draw calls were executing (validated via debug logs showing 46356 indices submitted), render graph was running, PostProcess was ready, but nothing appeared on screen.

## Investigation Steps

1. **Verified shader debug flags disabled** — All `DEBUG_*` macros set to 0
2. **Confirmed draw calls happening** — Added debug log showing `vkCmdDrawIndexed` with correct index count
3. **RED clear test** — Bypassed render graph, cleared swapchain to RED → **Visible at 4 FPS**
   - Conclusion: Swapchain presentation path works
4. **Debug shader output** — Set `DEBUG_GBUFFER 99` in deferred lighting → **Still black**
   - Conclusion: Tonemap pass not executing properly
5. **Traced `_tonemapFramebuffers`** — Found it was empty during frame rendering

## Root Cause Analysis

`_tonemapFramebuffers` is populated by `CreateFramebuffers()`, which wraps swapchain images in framebuffers compatible with `_tonemapRenderPass`.

**The bug:** `CreateFramebuffers()` was only called inside `RecreateSwapchain()` (line 2240), but **never during initial `Init()`**.

During Init:
```
CreateRenderPass()     ✓ Creates _tonemapRenderPass
CreateFramebuffers()   ✗ NEVER CALLED
_postProcess.Create()  ✓ 
SetTonemapFramebuffers(_tonemapFramebuffers) → passes EMPTY vector!
```

When actual rendering runs, `VulkanPostProcess::Tonemap()` receives an empty framebuffer vector and either skips rendering or writes to invalid handles.

## Fix

Added `CreateFramebuffers()` call in `VulkanBackend::Init()` after render pass creation, and moved `SetTonemapFramebuffers()` to after the framebuffers are created:

```cpp
// Create tonemap framebuffers (must be after CreateRenderPass which creates _tonemapRenderPass)
if (!CreateFramebuffers()) {
    LUNA_LOG_ERROR("VK: CreateFramebuffers failed");
    return false;
}
// Now that _tonemapFramebuffers is populated, pass it to PostProcess
_postProcess.SetTonemapFramebuffers(_tonemapFramebuffers);
```

## Files Changed

| File | Change |
|------|--------|
| `VulkanBackend.cpp` | Added `CreateFramebuffers()` call in `Init()` after `CreateRenderPass()` |
| `VulkanBackend.cpp` | Moved `SetTonemapFramebuffers()` to after `CreateFramebuffers()` |

## Verification

- Render graph executes with valid framebuffers
- Tonemap pass writes to swapchain images correctly
- DamagedHelmet mesh visible

## Lessons Learned

1. **Init vs Recreate divergence** — When adding new subsystems, ensure both code paths create all required resources
2. **Empty container silent failure** — Passing empty vectors to subsystems can cause silent failures if not validated
3. **Swapchain bypass test** — Simple RED clear test immediately isolates swapchain vs. render graph issues

