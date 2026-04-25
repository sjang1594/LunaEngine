# Bug #009: Hi-Z Pyramid Build Leaves Depth in Wrong Layout — Device Lost

**Date:** 2026-04-25  
**Status:** Fixed  
**Severity:** Critical  
**Component:** Phase 23 Hi-Z occlusion culling, VulkanBackend depth image layout management

---

## Symptoms

```
Exception / VK_ERROR_DEVICE_LOST after LoadMeshes
```

Crash occurred on the first rendered frame after `LoadMeshes` completed successfully. The Hi-Z resources were created without error, but the first `CompositeFrame()` call triggered a device lost.

---

## Root Cause Analysis

### The Depth Layout Contract

The G-buffer render pass declares `finalLayout = DEPTH_STENCIL_READ_ONLY_OPTIMAL` for the depth attachment. After `vkCmdEndRenderPass`, every downstream consumer — the VulkanRenderGraph, SSAO, deferred lighting, SSR — expects depth in this layout.

The render graph explicitly imports `_depthImage` with this assumption:

```cpp
auto hDepth = rg.ImportImage(_depthImage,
    VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,  // ← assumed initial layout
    VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
    VK_IMAGE_ASPECT_DEPTH_BIT);
```

### The Hi-Z Build Broke the Contract

Phase 23 added a `BuildHiZPyramid(cmd)` call in `CompositeFrame()` between the render pass end and the render graph, to build the Hi-Z pyramid from the current frame's depth (for next frame's occlusion culling). The code transitioned depth to `ATTACHMENT_OPTIMAL` before calling `BuildHiZPyramid`, which internally transitions:

```
ATTACHMENT_OPTIMAL → TRANSFER_SRC_OPTIMAL (blit depth → Hi-Z mip 0)
TRANSFER_SRC_OPTIMAL → ATTACHMENT_OPTIMAL (restore for mip generation)
```

**After `BuildHiZPyramid` returned, depth was left in `ATTACHMENT_OPTIMAL`.** The render graph then emitted barriers based on its assumed `READ_ONLY_OPTIMAL` initial layout — a layout mismatch that is **undefined behavior per Vulkan spec §8.1**, causing validation errors and device lost.

```
Layout flow (BROKEN):
  G-buffer pass ends    → READ_ONLY_OPTIMAL
  Pre-HiZ transition    → ATTACHMENT_OPTIMAL
  BuildHiZPyramid       → TRANSFER_SRC → ATTACHMENT_OPTIMAL  ← stuck here
  Render graph expects  → READ_ONLY_OPTIMAL                  ← MISMATCH → crash
```

### Why Not Fix Inside BuildHiZPyramid?

`BuildHiZPyramid` is called from two sites:

1. **`FlushDraws()` (pre-cull)** — After this call, the G-buffer render pass is re-opened with `_gbRenderPassLoad`, which requires depth in `ATTACHMENT_OPTIMAL`. Changing the postcondition would break this path.

2. **`CompositeFrame()` (post-draw)** — After this call, the render graph and all downstream passes need `READ_ONLY_OPTIMAL`.

The two call sites have **different postcondition requirements**, so the fix must be at the call site, not inside `BuildHiZPyramid`.

---

## Additional Build Errors Fixed

Three build errors were introduced alongside the Hi-Z Phase 23 implementation:

### 1. `hiz_generate.comp.hlsl` Compiled as C++ (MSVC Error C1010)

The HLSL shader was listed under `<ClCompile>` in the vcxproj instead of `<None>`. MSVC tried to compile it as a C++ translation unit and failed looking for the precompiled header.

**Fix:** Regenerated vcxproj via `premake5 vs2022`. The premake script already had `filter { "files:**.hlsl" } buildaction "None"` — the vcxproj was simply out of sync.

### 2. `DX12Pipeline::Init` Does Not Exist (Error C2039)

`CreateHiZResources()` called `_hizGeneratePipeline->Init(...)` but the actual method is `Initialize(...)`.

**Fix:** `Init` → `Initialize`

### 3. `InsertEndTimestamp` / `WriteEndTimestamp` Wrong Argument Count (Error C2660)

Both DX12 and Vulkan backends called the end-timestamp profiler functions with 2 arguments (`cmd, "Hi-Z Build"`), but the functions only take 1 argument (`cmd`). The begin-timestamp functions take 2 (with pass name), but end-timestamp does not.

**Fix:** Removed the extra string argument from both `_gpuProfiler.InsertEndTimestamp(cmd)` (DX12) and `_gpuProfiler.WriteEndTimestamp(cmd)` (Vulkan).

---

## Fixes Applied

### 1. Restore Depth Layout After Hi-Z Build in CompositeFrame

**File:** `VulkanBackend.cpp` — `CompositeFrame()`

Added a `VkImageMemoryBarrier` immediately after `BuildHiZPyramid(cmd)` to transition depth from `ATTACHMENT_OPTIMAL` back to `READ_ONLY_OPTIMAL`:

```cpp
if (_hizImage && _hizMipCount >= 2)
{
    // READ_ONLY → ATTACHMENT (BuildHiZPyramid expects ATTACHMENT_OPTIMAL)
    VkImageMemoryBarrier depthBar{ ... };
    depthBar.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    depthBar.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(cmd, ...);

    BuildHiZPyramid(cmd);

    // ATTACHMENT → READ_ONLY (restore for render graph + downstream passes)
    VkImageMemoryBarrier restoreBar{ ... };
    restoreBar.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                             | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    restoreBar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    restoreBar.oldLayout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    restoreBar.newLayout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &restoreBar);
}
```

```
Layout flow (FIXED):
  G-buffer pass ends    → READ_ONLY_OPTIMAL
  Pre-HiZ transition    → ATTACHMENT_OPTIMAL
  BuildHiZPyramid       → TRANSFER_SRC → ATTACHMENT_OPTIMAL
  Restore barrier (NEW) → READ_ONLY_OPTIMAL  ← matches render graph ✓
```

### 2. HLSL Shader Build Action

**File:** `LunaEngine.vcxproj` (regenerated via premake5)

`hiz_generate.comp.hlsl` moved from `<ClCompile>` to `<None>`.

### 3. DX12 API Name Fixes

**File:** `DX12Backend.cpp`

| Line | Before | After |
|------|--------|-------|
| 3495 | `_hizGeneratePipeline->Init(...)` | `_hizGeneratePipeline->Initialize(...)` |
| 3607 | `_gpuProfiler.InsertEndTimestamp(cmd, "Hi-Z Build")` | `_gpuProfiler.InsertEndTimestamp(cmd)` |

### 4. Vulkan Profiler Call Fix

**File:** `VulkanBackend.cpp` — `BuildHiZPyramid()`

| Line | Before | After |
|------|--------|-------|
| 5751 | `_gpuProfiler.WriteEndTimestamp(cmd, "Hi-Z Build")` | `_gpuProfiler.WriteEndTimestamp(cmd)` |

---

## Files Modified

| File | Changes |
|------|---------|
| `VulkanBackend.cpp` | Added depth layout restore barrier after Hi-Z build in `CompositeFrame()`; fixed `WriteEndTimestamp` arg count in `BuildHiZPyramid()` |
| `DX12Backend.cpp` | Fixed `Init` → `Initialize`; fixed `InsertEndTimestamp` arg count |
| `LunaEngine.vcxproj` | Regenerated — `hiz_generate.comp.hlsl` now `<None>` instead of `<ClCompile>` |

---

## Key Insight

**Every image layout transition must maintain the contract expected by downstream consumers.** When inserting a new pass between two existing stages, the new pass must restore all image layouts to the state the next stage expects. `BuildHiZPyramid` was designed for the `FlushDraws` context (depth stays in `ATTACHMENT_OPTIMAL`), but reusing it in `CompositeFrame` (where depth must return to `READ_ONLY_OPTIMAL`) required an explicit restore barrier at the call site.

This is a general pattern in Vulkan engines: **helper functions that touch image layouts should document their postconditions**, and callers must add fixup barriers when the postcondition doesn't match their context.

---

## References

- [Vulkan Spec §8.1: Image Layout Transitions](https://registry.khronos.org/vulkan/specs/1.3-extensions/html/vkspec.html#resources-image-layouts)
- [Vulkan Spec §7.4.2: Render Pass Layout Transitions](https://registry.khronos.org/vulkan/specs/1.3-extensions/html/vkspec.html#renderpass-layout-transitions)

