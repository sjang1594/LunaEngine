# VulkanPostProcess — Implementation Document

**Date:** 2026-04-25  
**Status:** ✅ Complete (standalone)  
**Lines:** Header ~260, Impl ~920  

---

## Overview

VulkanPostProcess owns the full post-processing stack: HDR intermediate, TAA with ping-pong history, Bloom (bright extraction + separable blur), SSR compute, Motion Blur, and final tonemap. Provides discrete pass methods; frame loop orchestration stays in VulkanBackend.

---

## Responsibilities

| Responsibility | API |
|----------------|-----|
| Resource lifecycle | `Create()`, `Destroy()`, `Resize()` |
| Per-frame descriptor update | `UpdateDescriptors()` |
| SSR compute | `DrawSSR()` |
| Motion blur graphics | `DrawMotionBlur()` |
| TAA resolve | `DrawTAA()` |
| Bloom bright extraction | `DrawBloomBright()` |
| Bloom separable blur | `DrawBloomBlur()` |
| Final tonemap | `DrawTonemap()` |
| Jitter/VP tracking | `SetJitter()`, `SetUnjitteredVP()`, `GetPrevUnjitteredVP()` |
| State query | `IsReady()`, `IsTAAReady()`, `IsMotionBlurReady()`, `IsSSRReady()` |
| Image accessors | `GetHDRView()`, `GetSSRView()`, `GetHDRImage()`, `GetSSRImage()` |
| Render pass accessors | `GetPPRenderPass()`, `GetTonemapRenderPass()` |
| Framebuffer accessors | `GetDeferredHDRFramebuffer()`, `SetTonemapFramebuffers()` |

---

## Design Decisions

### 1. Building Blocks, Not Frame Loop

VulkanPostProcess provides discrete pass methods (DrawTAA, DrawBloom*, etc.) rather than a monolithic Composite() method. This keeps frame loop orchestration in VulkanBackend where cross-subsystem coordination (render graph, profiler timestamps, barriers) happens.

### 2. Decoupled from VulkanBackend State

Instead of reaching into VulkanBackend members, VulkanPostProcess receives all required inputs:
- **CreateInfo**: VulkanCore, extent, samplers, G-buffer views
- **Pass methods**: frameIndex, view/proj matrices, TAA constants

### 3. Two Render Passes

- **`_ppRenderPass`**: R16G16B16A16_SFLOAT → SHADER_READ_ONLY (used for TAA, Bloom, Motion Blur)
- **`_tonemapRenderPass`**: swapchain format → PRESENT_SRC (final output)

### 4. TAA Ping-Pong History

Two `_taaHistoryImage` buffers alternate each frame. `UpdateDescriptors()` writes the correct history texture to the TAA descriptor set. The write index is `_taaHistoryIndex = frameCount & 1`.

### 5. External Tonemap Framebuffers

Swapchain framebuffers are created elsewhere (VulkanSwapchain or VulkanBackend). `SetTonemapFramebuffers()` receives the vector; DrawTonemap uses `_tonemapFramebuffers[imageIndex]`.

### 6. Graceful Degradation

Shader compile failures are non-fatal warnings. The `Is*Ready()` accessors let VulkanBackend skip unavailable passes:
- `IsSSRReady()` → `_ssrPipeline != VK_NULL_HANDLE`
- `IsMotionBlurReady()` → `_mbPipeline != VK_NULL_HANDLE`
- `IsTAAReady()` → `_taaPipeline != VK_NULL_HANDLE`

---

## Data Structures

### TAAConstants (160 bytes)
```cpp
struct TAAConstants {
    float invViewProj[16];   // 64 B - jittered inverse VP
    float prevViewProj[16];  // 64 B - previous unjittered VP
    float jitter[2];         //  8 B
    float prevJitter[2];     //  8 B
    float alpha;             //  4 B
    float _pad[3];           // 12 B
};
```

### MotionBlurConstants (152 bytes)
```cpp
struct MotionBlurConstants {
    float invViewProj[16];   // 64 B
    float prevViewProj[16];  // 64 B
    float screenSizeX;       //  4 B
    float screenSizeY;       //  4 B
    float shutterScale;      //  4 B
    int   numSamples;        //  4 B
    float _pad[2];           //  8 B
};
```

### CreateInfo
```cpp
struct CreateInfo {
    VulkanCore*   core              = nullptr;
    VkExtent2D    extent            = {};
    VkFormat      swapchainFormat   = VK_FORMAT_B8G8R8A8_UNORM;
    VkImageView   depthView         = VK_NULL_HANDLE;
    VkImageView   normalView        = VK_NULL_HANDLE;
    VkImageView   metalRoughView    = VK_NULL_HANDLE;
    VkSampler     linearSampler     = VK_NULL_HANDLE;
    VkSampler     pointClampSampler = VK_NULL_HANDLE;
};
```

---

## Pipeline Flow

```
1. Deferred Lighting → HDR image
2. SSR Compute       → SSR image
3. Motion Blur       → MB image (reads HDR + depth)
4. TAA               → TAA history[write] (reads MB/HDR + history[read] + depth)
5. Bloom Bright      → Half-res bright (reads TAA output)
6. Bloom Blur H      → Half-res blur (reads bright)
7. Bloom Blur V      → Half-res bright (reads blur)
8. Tonemap           → Swapchain (reads TAA + bloom + SSR)
```

---

## Files

| File | Path |
|------|------|
| Header | `LunaEngine/src/LunaEngine/Renderer/Vulkan/Public/VulkanPostProcess.h` |
| Implementation | `LunaEngine/src/LunaEngine/Renderer/Vulkan/Private/VulkanPostProcess.cpp` |
| This doc | `Docs/progress/subsystem_10_vulkan_postprocess.md` |

---

## Usage Example

```cpp
// Create
VulkanPostProcess::CreateInfo ppInfo{};
ppInfo.core = _core.get();
ppInfo.extent = _swapchainExtent;
ppInfo.swapchainFormat = _swapchainFormat;
ppInfo.depthView = _gbuffer.GetDepthView();
ppInfo.normalView = _gbuffer.GetNormalView();
ppInfo.metalRoughView = _gbuffer.GetMetalRoughView();
ppInfo.linearSampler = _linearSampler;
ppInfo.pointClampSampler = _pointClampSampler;
_postProcess.Create(ppInfo);

// Set tonemap framebuffers (after swapchain creation)
_postProcess.SetTonemapFramebuffers(_swapchain.GetFramebuffers());

// In BeginFrame (after fence wait)
_postProcess.UpdateDescriptors(_frameIndex);

// In CompositeFrame
// ... deferred lighting writes to _postProcess.GetDeferredHDRFramebuffer() ...

// SSR
_postProcess.DrawSSR(cmd, _frameIndex, _deferredView, _deferredProj);

// Motion blur
_postProcess.DrawMotionBlur(cmd, _frameIndex, _deferredView, _deferredProj, _prevVP);

// TAA
VulkanPostProcess::TAAConstants taaConst{};
// ... fill from jittered VP, prev unjittered VP, jitter values ...
taaConst.alpha = (_frameCount < 8) ? 1.0f : 0.1f;
_postProcess.DrawTAA(cmd, _frameIndex, taaConst);

// Bloom
_postProcess.DrawBloomBright(cmd);
_postProcess.DrawBloomBlur(cmd, true);   // H
_postProcess.DrawBloomBlur(cmd, false);  // V

// Tonemap
_postProcess.DrawTonemap(cmd, _imageIndex);
```

---

## Build Verification

```
Build succeeded.
0 Error(s)
5 Warning(s) (codepage, linker duplicate symbols - pre-existing)
```

---

## Integration Status

Standalone extraction complete. Not yet integrated into VulkanBackend. Backend still contains post-process members at lines 593-748 and methods at lines 3915-4768.

---

## Tech Debt

**Shader compiler duplication**: `CompileHLSLtoSPIRV` and `CompileGLSLtoSPIRV` are now duplicated in 4 files (VulkanBackend, VulkanSSAO, VulkanIBL, VulkanPostProcess). Future work: extract to `VulkanShaderCompiler` utility.

---

## Next Steps

1. **Integration** into VulkanBackend's `CompositeFrame()` — replace inline PP code with `_postProcess.*` calls
2. **VulkanShaderCompiler** extraction — consolidate duplicated compilation helpers

