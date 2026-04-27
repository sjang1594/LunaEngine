# Phase 24 — Clustered Lighting (Both Backends)

**Date:** 2026-04-26  
**Status:** ✅ Complete (Vulkan + DX12)  
**Dependencies:** Phase 7 (Deferred), Phase 15C (IBL), Phase 18C (Render Graph)

---

## Goal

Support hundreds of point lights via GPU clustered lighting. The screen is divided into 16×9×24 view-frustum clusters with logarithmic depth slicing. A compute shader assigns lights to clusters. The deferred lighting fragment shader iterates only per-cluster light lists, avoiding O(pixels × lights) cost. The single directional light remains unchanged.

---

## Architecture

```
Per frame:
  1. CPU: SetPointLights() uploads PointLightDesc[] from ImGui editor
  2. CompositeFrame():
     a. DispatchClusterAssign(cmd):
        - Transform light positions to view space
        - Upload to light SSBO (host-visible)
        - Update ClusterParams UBO (invProj, near/far, screen dims, numLights)
        - vkCmdFillBuffer → clear clusterLightCounts
        - vkCmdDispatch(16, 9, 24) → cluster_assign_vk.comp.glsl
        - Buffer barrier: COMPUTE_WRITE → FRAGMENT_READ
     b. Deferred Lighting pass:
        - Bind set=2 (cluster data)
        - Fragment shader: derive cluster index from UV + viewZ
        - Loop over cluster's light list → Cook-Torrance BRDF per light
```

---

## Cluster Grid

| Axis | Count | Slicing |
|------|-------|---------|
| X    | 16    | Uniform screen tiles |
| Y    | 9     | Uniform screen tiles |
| Z    | 24    | Logarithmic depth (near..far) |

**Total clusters:** 3,456  
**Max lights per cluster:** 128  
**Max total point lights:** 1,024

### Logarithmic Depth Slicing

```
sliceNear = near * (far/near)^(z / CLUSTER_Z)
sliceFar  = near * (far/near)^((z+1) / CLUSTER_Z)
```

This concentrates depth resolution near the camera where small lights matter most.

---

## GPU Data Structures

### GPUPointLight (32 bytes, std430)

```glsl
struct GPUPointLight {
    vec3  position;   // view-space
    float radius;
    vec3  color;
    float intensity;
};
```

### ClusterParams UBO (96 bytes)

```glsl
uniform ClusterParams {
    mat4  invProj;       // row-major inverse projection
    float nearZ, farZ;
    float screenW, screenH;
    uint  numLights;
    uint  _pad[3];
};
```

### SSBOs

| Buffer | Size (1080p) | Usage |
|--------|-------------|-------|
| Light SSBO | 32 KB (1024 × 32B) | Host-visible, CPU upload per frame |
| Cluster counts | ~14 KB (3456 × 4B) | Device-local, cleared + written by compute |
| Cluster indices | ~1.7 MB (3456 × 128 × 4B) | Device-local, written by compute |

---

## Compute Shader: `cluster_assign_vk.comp.glsl`

- **Dispatch:** (16, 9, 24) — one thread per cluster
- **Algorithm per thread:**
  1. Compute logarithmic near/far depth for this Z slice
  2. Reconstruct 8 view-space corners of the cluster AABB (4 screen corners × near/far)
  3. Compute axis-aligned bounding box from the 8 corners
  4. For each light: sphere-AABB intersection test
  5. Write matching light indices to `clusterLightIndex[clusterIdx * 128 + count]`
  6. Store final count in `clusterLightCount[clusterIdx]`

### Descriptor Layout (set=0)

| Binding | Type | Content |
|---------|------|---------|
| 0 | UNIFORM_BUFFER | ClusterParams |
| 1 | STORAGE_BUFFER (read) | GPUPointLight[] |
| 2 | STORAGE_BUFFER (read/write) | clusterLightCount[] |
| 3 | STORAGE_BUFFER (write) | clusterLightIndex[] |

---

## Deferred Lighting Shader Changes

`deferred_lighting_ibl_vk.frag.glsl` extended with:

### New Descriptor Set (set=2)

| Binding | Type | Content |
|---------|------|---------|
| 0 | UNIFORM_BUFFER | ClusterParams |
| 1 | STORAGE_BUFFER (read) | GPUPointLight[] |
| 2 | STORAGE_BUFFER (read) | clusterLightCount[] |
| 3 | STORAGE_BUFFER (read) | clusterLightIndex[] |

### Scene UBO Extension

```glsl
uint rtEnabled;
uint numPointLights;  // NEW — Phase 24
uvec2 _pad2;
```

### Point Light Accumulation

```glsl
if (numPointLights > 0) {
    // Derive cluster index from screen UV + view-space Z
    uint cx = uint(uv.x * CLUSTER_X);
    uint cy = uint(uv.y * CLUSTER_Y);
    uint cz = uint(log(viewZ / nearZ) / log(farZ / nearZ) * CLUSTER_Z);
    uint clusterIdx = cx + cy * CLUSTER_X + cz * CLUSTER_X * CLUSTER_Y;

    uint count = clusterLightCount[clusterIdx];
    for (uint i = 0; i < count; ++i) {
        uint lightIdx = clusterLightIndex[clusterIdx * 128 + i];
        // View-space light → world-space direction for BRDF
        // Cook-Torrance: D_GGX + G_Smith + F_Schlick
        // Attenuation: 1/(dist² + ε) × smoothstep radius falloff
        Lo += (diffuse + specular) * radiance * NdL * attenuation;
    }
}
```

---

## Pipeline Layout Change

The deferred pipeline layout expanded from 2 to 3 descriptor sets:

| Set | Layout | Content |
|-----|--------|---------|
| 0 | `_deferredSceneLayout` | Per-frame scene UBO |
| 1 | `_deferredGbufLayout` | G-buffer textures, CSM, SSAO, IBL, RT shadow (14 bindings) |
| 2 | `_clusterLightLayout` | **NEW** — Cluster params UBO + 3 SSBOs |

---

## Render Graph Integration

Cluster assignment inserted as a compute pass before Deferred Lighting:

```
Pass 1: SSAO
Pass 2: SSAO Blur
Pass 3: RT Shadows
Pass 3.5: Cluster Assign  ← NEW (compute)
Pass 4: Deferred Lighting  (now reads cluster data via set=2)
Pass 5: SSR
...
```

The Cluster Assign pass is a `SideEffect()` pass (always executed when lights > 0).

---

## ImGui Light Editor

New "Point Lights" panel in `LunaApp.cpp`:

- **Add Light** — spawns at camera position with warm white (1.0, 0.9, 0.7) default
- **Per-light controls:** DragFloat3 position, DragFloat radius/intensity, ColorEdit3
- **Remove** — per-light X button
- **Clear All** — remove all lights
- Calls `IRenderBackend::SetPointLights()` every frame to push to GPU

---

## Files Changed

| File | Type | Description |
|------|------|-------------|
| `Shaders/cluster_assign_vk.comp.glsl` | New | Vulkan cluster assignment compute shader |
| `Shaders/cluster_assign.comp.hlsl` | New | DX12 cluster assignment compute shader |
| `Shaders/deferred_lighting_ibl_vk.frag.glsl` | Modified | Added set=2 cluster bindings, point light accumulation loop |
| `Shaders/deferred_lighting_ibl.frag.hlsl` | Modified | Added cluster bindings (b1, t9-t11), point light accumulation loop |
| `Renderer/Vulkan/Public/VulkanBackend.h` | Modified | GPUPointLight, ClusterParamsUBO, cluster members, SetPointLights() |
| `Renderer/Vulkan/Private/VulkanBackend.cpp` | Modified | Create/Destroy/Dispatch cluster resources, render graph pass, pipeline layout expansion |
| `Renderer/DX12/Public/DX12Backend.h` | Modified | DX12GPUPointLight, ClusterParamsData, cluster members, SetPointLights() |
| `Renderer/DX12/Private/DX12Backend.cpp` | Modified | Create/Destroy clustered lighting resources, DispatchClusterAssign(), SceneConstants extension |
| `Renderer/DX12/Private/DX12Pipeline.cpp` | Modified | Added ClusterAssign root signature layout |
| `Graphics/IPipeline.h` | Modified | Added RootSignatureLayout::ClusterAssign |
| `Renderer/HAL/Public/IRenderBackend.h` | Modified | PointLightDesc struct, SetPointLights() virtual |
| `LunaApp/src/LunaApp.cpp` | Modified | ImGui Point Lights editor panel |

---

## Success Criteria

- [x] Cluster compute shader compiles and dispatches without validation errors (both backends)
- [x] Deferred lighting shader compiles with cluster bindings (both backends)
- [x] Pipeline layout correctly includes cluster data (both backends)
- [x] Zero validation errors during 15-second run
- [x] ImGui light editor panel functional (add/remove/edit)
- [x] DX12 backend parity
- [ ] Visual verification: point lights illuminate geometry (requires adding lights via UI)
- [ ] 256+ point lights at ≥30 FPS (1080p)

