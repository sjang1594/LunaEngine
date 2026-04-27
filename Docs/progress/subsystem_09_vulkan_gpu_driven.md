# VulkanGPUDriven — Implementation Document

**Date:** 2026-04-25  
**Status:** ✅ Complete (standalone)  
**Lines:** Header 253, Impl 1113  

---

## Overview

VulkanGPUDriven owns GPU-driven indirect rendering resources: merged geometry, compute culling pipeline, indirect draw resources, and async compute sync primitives. Provides building blocks for GPU culling + indirect draw; frame loop orchestration stays in VulkanBackend.

---

## Responsibilities

| Responsibility | API |
|----------------|-----|
| Merged geometry lifecycle | `BuildMergedGeometry()`, internal `DestroyMergedGeometry()` |
| Instance recording | `RecordInstance()`, `ClearInstances()`, `GetInstanceCount()` |
| ViewProj UBO update | `UpdateViewProj()` |
| Cull dispatch (sync) | `DispatchCullSync()` |
| Cull dispatch (async) | `DispatchCullAsync()`, `WaitForComputeFence()`, `RecordAcquireBarriers()` |
| Indirect draw | `DrawIndirect()` |
| State query | `IsReady()`, `IsAsyncComputeReady()` |
| Accessors | `GetMergedVB()`, `GetMergedIB()`, `GetCullDescSet()`, `GetComputeDoneSemaphore()` |

---

## Design Decisions

### 1. Building Blocks, Not Frame Loop

VulkanGPUDriven provides discrete operations (cull dispatch, indirect draw) rather than owning `FlushDraws()`. This keeps frame loop orchestration in VulkanBackend where cross-subsystem coordination (G-buffer transitions, Hi-Z build, profiler timestamps) happens.

### 2. CullConstants as Parameter

`DispatchCullSync/Async` takes pre-filled `CullConstants` rather than view/proj matrices. The caller (VulkanBackend) builds frustum planes and fills Hi-Z params because:
- Hi-Z ready state is owned by VulkanHiZ (or still in VulkanBackend)
- BuildFrustumPlanes() is provided as static helper but not called internally

### 3. MaterialData Struct for Decoupling

Instead of depending on `VulkanBackend::VkMaterial`, VulkanGPUDriven defines its own `MaterialData` struct with only the needed fields (factors + image views). Materials are passed via `CreateInfo.materials` pointer.

### 4. Geometry and Resources Separate

`BuildMergedGeometry()` only builds VB/IB/MeshInfo. Indirect resources (SSBOs, pipelines, descriptors) are created in `Create()` after materials are available. This allows flexibility in initialization order.

### 5. Hi-Z Descriptor Updates External

Cull descriptor set bindings 4-5 (Hi-Z UBO + texture) are NOT written by VulkanGPUDriven. VulkanBackend reads `GetCullDescSet(frameIndex)` and writes Hi-Z bindings after Hi-Z creation. This avoids circular dependency.

### 6. Async Compute Optional

`CreateAsyncComputeResources()` may fail if no dedicated compute queue. `IsAsyncComputeReady()` lets caller choose sync vs async path. `GetComputeDoneSemaphore()` returns `VK_NULL_HANDLE` if async not available.

---

## Data Structures

### MaterialData (user-provided)
```cpp
struct MaterialData {
    float albedoFactor[4];
    float metallicFactor;
    float roughnessFactor;
    VkImageView albedoView;
    VkImageView normalView;
    VkImageView metalRoughView;
    VkImageView emissiveView;
};
```

### GPUObjectData (96 bytes)
```cpp
struct GPUObjectData {
    XMFLOAT4X4 model;           // 64 B
    XMFLOAT4   boundingSphere;  // 16 B
    uint32_t   meshIndex;       //  4 B
    uint32_t   materialIndex;   //  4 B
    uint64_t   _unused;         //  8 B
};
```

### CullConstants (128 bytes, push constant)
```cpp
struct CullConstants {
    float    frustumPlanes[6][4];  // 96 B
    uint32_t objectCount;
    uint32_t enableHiZ;
    uint32_t hizMipCount;
    uint32_t _pad0;
    float    projParams[4];        // m11, m22, m33, m43
};
```

---

## Files

| File | Path |
|------|------|
| Header | `LunaEngine/src/LunaEngine/Renderer/Vulkan/Public/VulkanGPUDriven.h` |
| Implementation | `LunaEngine/src/LunaEngine/Renderer/Vulkan/Private/VulkanGPUDriven.cpp` |
| This doc | `Docs/progress/subsystem_09_vulkan_gpu_driven.md` |

---

## Usage Example

```cpp
// Build materials array from scene
std::vector<VulkanGPUDriven::MaterialData> matData;
for (auto* mat : uniqueMats) {
    VulkanGPUDriven::MaterialData m;
    m.albedoFactor[0] = mat->albedoFactor[0]; // ...
    m.albedoView = mat->albedo.view;
    // ...
    matData.push_back(m);
}

// Build geometry first
_gpuDriven.BuildMergedGeometry(allVerts, allIdxs, _rtSupported);

// Then create with materials
VulkanGPUDriven::CreateInfo gpuInfo{};
gpuInfo.core = _core.get();
gpuInfo.gbRenderPassLoad = _gbuffer.GetRenderPassLoad();
gpuInfo.linearSampler = _linearSampler;
gpuInfo.materials = &matData;
_gpuDriven.Create(gpuInfo);

// After Hi-Z creation, write cull desc bindings 4-5:
for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
    VkDescriptorSet cullSet = _gpuDriven.GetCullDescSet(i);
    // ... vkUpdateDescriptorSets for Hi-Z UBO + texture
}

// In FlushDraws():
_gpuDriven.UpdateViewProj(_deferredView, _lastProj);

VulkanGPUDriven::CullConstants cc{};
VulkanGPUDriven::BuildFrustumPlanes(VP, cc);
cc.objectCount = _gpuDriven.GetInstanceCount();
cc.enableHiZ   = _hiZ.IsReady() ? 1 : 0;
cc.hizMipCount = _hiZ.GetMipCount();
// ... fill projParams

if (_gpuDriven.IsAsyncComputeReady()) {
    _gpuDriven.DispatchCullAsync(frameIndex, cc);
    _gpuDriven.RecordAcquireBarriers(cmd, frameIndex);
} else {
    _gpuDriven.DispatchCullSync(cmd, frameIndex, cc);
}

// Re-open G-buffer render pass, then:
_gpuDriven.DrawIndirect(cmd, frameIndex);
_gpuDriven.ClearInstances();
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

Standalone extraction complete. Not yet integrated into VulkanBackend. Backend still contains GPU-driven members at lines 344-421 and methods at lines 4857-6261.

---

## Next Steps

1. **VulkanPostProcess** extraction (subsystem 10)

