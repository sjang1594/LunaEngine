# VulkanHiZ — Implementation Document

**Date:** 2026-04-25  
**Status:** ✅ Complete (standalone)  
**Lines:** Header 103, Impl 390  

---

## Overview

VulkanHiZ manages the Hi-Z (Hierarchical-Z) depth pyramid used for GPU occlusion culling. Creates a full-resolution R32_SFLOAT mip chain from the depth buffer via compute dispatches.

---

## Responsibilities

| Responsibility | API |
|----------------|-----|
| Hi-Z image lifecycle | `Create()`, `Destroy()`, `Resize()` |
| Mip-chain generation | `BuildPyramid()` |
| Cull shader access | `GetFullView()`, `GetSampler()`, `GetParamsBuffer()` |
| State query | `IsReady()`, `GetMipCount()` |

---

## Design Decisions

### 1. Depth View Passed Externally

`Create()` and `Resize()` take `VkImageView depthView` as parameter. VulkanHiZ doesn't own or know about the swapchain depth buffer. Mip-0 descriptor set reads from this external depth view.

### 2. Depth Layout Transitions Inside BuildPyramid

`BuildPyramid()` transitions the depth image: `ATTACHMENT_OPTIMAL → READ_ONLY → ATTACHMENT_OPTIMAL`. These transitions are tightly coupled to the mip-0 dispatch and belong here rather than in the caller.

### 3. GPU Profiler Timestamps External

`BuildPyramid()` doesn't call profiler timestamps. VulkanBackend wraps the call:
```cpp
_gpuProfiler.WriteBeginTimestamp(cmd, "Hi-Z Build");
_hiZ.BuildPyramid(cmd, depthImage, extent);
_gpuProfiler.WriteEndTimestamp(cmd);
```

### 4. Cull Descriptor Updates External

The cull shader descriptor set updates (bindings 4,5 for Hi-Z UBO + texture) remain in VulkanBackend since they belong to the GPU-driven subsystem. After `_hiZ.Create()`, VulkanBackend reads `GetParamsBuffer()`, `GetSampler()`, `GetFullView()` to write cull descriptors.

### 5. CompileGLSLtoSPIRV Duplicated

The GLSL→SPIR-V compilation helper is duplicated as a file-local static function. Tech debt note: extract to shared `VulkanShaderUtils` when more subsystems need it.

---

## Memory Layout

```
VulkanHiZ (~280 bytes)
├── VulkanCore*     _core           (8B)
├── VkImage         _image          (8B)
├── VkDeviceMemory  _memory         (8B)
├── VkImageView     _mipView[13]    (104B)
├── VkImageView     _fullView       (8B)
├── uint32_t        _mipCount       (4B)
├── bool            _ready          (1B)
├── VkSampler       _sampler        (8B)
├── VkDescriptorSetLayout _descLayout (8B)
├── VkDescriptorPool _descPool      (8B)
├── VkDescriptorSet  _descSet[13]   (104B)
├── VkPipelineLayout _pipeLayout    (8B)
├── VkPipeline       _pipeline      (8B)
├── VkBuffer         _paramsBuffer  (8B)
├── VkDeviceMemory   _paramsMem     (8B)
└── void*            _paramsMapped  (8B)
```

---

## Files

| File | Path |
|------|------|
| Header | `LunaEngine/src/LunaEngine/Renderer/Vulkan/Public/VulkanHiZ.h` |
| Implementation | `LunaEngine/src/LunaEngine/Renderer/Vulkan/Private/VulkanHiZ.cpp` |
| This doc | `Docs/progress/subsystem_04_vulkan_hiz.md` |

---

## Build Verification

```
Build succeeded.
0 Warning(s)
0 Error(s)
```

---

## Integration Status

Not yet integrated into VulkanBackend. Backend still contains Hi-Z members at lines 449-469 and methods at lines 5679-6015.

---

## Next Steps

1. **VulkanSSAO** extraction (subsystem 5)

