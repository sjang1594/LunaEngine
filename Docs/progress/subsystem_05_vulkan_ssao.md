# VulkanSSAO — Implementation Document

**Date:** 2026-04-25  
**Status:** ✅ Complete (standalone)  
**Lines:** Header 137, Impl 440  

---

## Overview

VulkanSSAO implements half-resolution Screen-Space Ambient Occlusion with a separate blur pass. Both are fullscreen triangle draws into R8_UNORM framebuffers.

---

## Responsibilities

| Responsibility | API |
|----------------|-----|
| SSAO lifecycle | `Create()`, `Destroy()`, `Resize()` |
| SSAO rendering | `Draw()` (raw SSAO), `DrawBlur()` (blur pass) |
| Result access | `GetBlurredView()`, `GetRawView()` |

---

## Design Decisions

### 1. External Sampler + Views

The shared `pointClampSampler`, depth view, and normal view are passed via `CreateInfo`. VulkanSSAO doesn't own them — they come from the deferred pipeline and swapchain subsystems.

### 2. Fullscreen Pipeline Helper

Extracted the fullscreen pipeline creation lambda from VulkanBackend into a file-local static `CreateFullscreenPipeline()` function. Reusable across subsystems but currently duplicated.

### 3. Shader Compilation Duplication

Both `CompileHLSLtoSPIRV` and `CompileGLSLtoSPIRV` are duplicated as file-local statics. See Tech Debt section.

### 4. Per-Frame UBO with framesInFlight Parameter

Supports configurable frames-in-flight count (capped at MAX_FRAMES=3) rather than hardcoding FRAMES_IN_FLIGHT.

---

## Tech Debt

### Shader Compilation Duplication (Priority: Medium)

**Problem**: `CompileHLSLtoSPIRV` and `CompileGLSLtoSPIRV` are now duplicated in 3 files:
- `VulkanBackend.cpp` (original)
- `VulkanHiZ.cpp` (GLSL only)
- `VulkanSSAO.cpp` (both HLSL + GLSL)
- `VulkanShadows.cpp` (GLSL only)

**Solution**: Extract to `VulkanShaderUtils.h/.cpp`:
```cpp
namespace Luna::VulkanShaderUtils {
    bool CompileHLSLtoSPIRV(const std::wstring& path, const std::wstring& target, std::vector<uint32_t>& out);
    bool CompileGLSLtoSPIRV(const std::wstring& path, std::vector<uint32_t>& out);
    VkPipeline CreateFullscreenPipeline(VkDevice dev, const wchar_t* fsPath, VkPipelineLayout layout, VkRenderPass rp);
}
```

**When**: Before VulkanIBL extraction (uses 4+ compute pipelines needing both compilers).

### Fullscreen Pipeline Duplication (Priority: Low)

`CreateFullscreenPipeline` static helper in VulkanSSAO.cpp could be shared. Same function needed by future VulkanPostProcess extraction.

---

## Files

| File | Path |
|------|------|
| Header | `LunaEngine/src/LunaEngine/Renderer/Vulkan/Public/VulkanSSAO.h` |
| Implementation | `LunaEngine/src/LunaEngine/Renderer/Vulkan/Private/VulkanSSAO.cpp` |
| This doc | `Docs/progress/subsystem_05_vulkan_ssao.md` |

---

## Build Verification

```
Build succeeded.
0 Warning(s)
0 Error(s)
```

---

## Integration Status

Not yet integrated into VulkanBackend. Backend still contains SSAO members at lines 284-342 and methods at lines 3619-3910.

---

## Next Steps

1. **VulkanShadows** extraction (subsystem 6) ✅
2. **VulkanShaderUtils** extraction (tech debt, before VulkanIBL)
