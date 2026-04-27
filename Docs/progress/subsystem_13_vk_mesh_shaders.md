# Phase 27 — Vulkan Mesh Shaders (VK_EXT_mesh_shader)

**Date:** 2026-04-27  
**Backend:** Vulkan  
**Status:** ✅ Complete (build verified)

---

## Overview

Ports the DX12 mesh shader pipeline (Phase 25) to Vulkan using `VK_EXT_mesh_shader`. Task shader performs per-meshlet frustum culling, mesh shader outputs G-buffer geometry from SSBO vertex data, fragment shader reuses the existing bindless material pipeline. Graceful fallback to `vkCmdDrawIndexedIndirectCount` when the extension is unavailable.

---

## Architecture

```
VulkanDevice::CreateLogicalDevice()
  └─ Probe VkPhysicalDeviceMeshShaderFeaturesEXT (taskShader + meshShader)
  └─ Enable VK_EXT_mesh_shader extension if supported

VulkanBackend::BuildMergedGeometry()
  └─ Call BuildMeshlets() per mesh (reuses Meshlet.h/cpp from Phase 25)
  └─ Upload to device-local SSBO buffers: meshlets, bounds, meshletVerts, meshletTris

VulkanBackend::CreateMeshShaderResources()
  ├─ Load vkCmdDrawMeshTasksEXT function pointer
  ├─ Descriptor set layout (set=0): 6 SSBO bindings
  ├─ Pipeline layout: set=0 (mesh SSBOs) + set=1 (bindless materials) + push constants (240B)
  ├─ Compile task + mesh + fragment shaders (GLSL → SPIR-V via glslc)
  └─ Create VkGraphicsPipeline (3 stages: TASK + MESH + FRAGMENT, no vertex input/IA)

VulkanBackend::FlushDraws()
  └─ if _meshShaderReady:
       ├─ Upload object data + build frustum planes
       ├─ Bind mesh shader pipeline + descriptors
       ├─ Per-object: push constants + vkCmdDrawMeshTasksEXT(ceil(meshletCount/32), 1, 1)
       └─ return (skip indirect draw path)
  └─ else: existing compute cull + vkCmdDrawIndexedIndirectCount path
```

---

## Shaders

### Task Shader (`meshlet_cull_vk.task.glsl`)
- GLSL 4.60 + `GL_EXT_mesh_shader`
- 32 threads per workgroup (one per meshlet)
- Reads `MeshletBounds` from SSBO, transforms to world space via object model matrix
- 6-plane frustum test per meshlet bounding sphere
- Shared-memory atomic compaction → `taskPayloadSharedEXT` with visible meshlet indices
- `EmitMeshTasksEXT(visibleCount, 1, 1)`

### Mesh Shader (`gbuffer_mesh_vk.mesh.glsl`)
- GLSL 4.60 + `GL_EXT_mesh_shader`
- 128 threads, `max_vertices=64, max_primitives=124`
- Reads `PBRVertex` from SSBO (48B stride: pos, normal, uv, tangent)
- Transforms vertices to clip space via push constant view/proj matrices
- Outputs: `gl_MeshVerticesEXT[].gl_Position`, per-vertex varyings (posWS, normalWS, uv, tangentWS, bitanWS, materialIndex)
- Unpacks meshlet triangle indices from packed uint32 format
- `SetMeshOutputsEXT(vertexCount, triangleCount)`

### Fragment Shader
- Reuses `gbuffer_indirect_vk.frag.glsl` (Phase 15B bindless material shader)
- Same inputs at locations 0-5, same 3 G-buffer MRT outputs
- No changes needed — mesh shader output matches indirect VS output exactly

---

## Push Constant Layout (240 bytes)

| Offset | Size | Field |
|--------|------|-------|
| 0 | 64 B | viewMatrix (mat4) |
| 64 | 64 B | projMatrix (mat4) |
| 128 | 96 B | frustumPlanes[6] (vec4×6) |
| 224 | 4 B | objectIndex |
| 228 | 4 B | meshletOffset |
| 232 | 4 B | meshletCount |
| 236 | 4 B | _pad |

---

## Descriptor Set Layout

| Set | Binding | Type | Content |
|-----|---------|------|---------|
| 0 | 0 | STORAGE_BUFFER | GPUObjectData[] |
| 0 | 1 | STORAGE_BUFFER | Meshlet[] |
| 0 | 2 | STORAGE_BUFFER | MeshletBounds[] |
| 0 | 3 | STORAGE_BUFFER | PBRVertex[] (merged VB as SSBO) |
| 0 | 4 | STORAGE_BUFFER | meshletVertices[] (uint32) |
| 0 | 5 | STORAGE_BUFFER | meshletTriangles[] (packed uint32) |
| 1 | * | (reused) | Bindless material set from Phase 15B |

---

## Files

| File | Type | Description |
|------|------|-------------|
| `Shaders/meshlet_cull_vk.task.glsl` | New | GLSL task shader — per-meshlet frustum cull |
| `Shaders/gbuffer_mesh_vk.mesh.glsl` | New | GLSL mesh shader — SSBO vertex fetch + G-buffer output |
| `Vulkan/Public/VulkanDevice.h` | Modified | Added `_meshShaderSupported` + `IsMeshShaderSupported()` |
| `Vulkan/Private/VulkanDevice.cpp` | Modified | Probe `VkPhysicalDeviceMeshShaderFeaturesEXT`, enable `VK_EXT_mesh_shader` |
| `Vulkan/Public/VulkanBackend.h` | Modified | Added mesh shader members: buffers, pipeline, descriptor set, function pointer |
| `Vulkan/Private/VulkanBackend.cpp` | Modified | `CompileGLSLtoSPIRV` task/mesh stage detection; `BuildMergedGeometry` meshlet generation; `CreateMeshShaderResources` pipeline; `FlushDraws` mesh shader dispatch path |

---

## Differences from DX12 Phase 25

| Aspect | DX12 | Vulkan |
|--------|------|--------|
| Extension | SM 6.5 (built-in) | `VK_EXT_mesh_shader` |
| Shader language | HLSL (.as.hlsl, .ms.hlsl) | GLSL (.task.glsl, .mesh.glsl) |
| Dispatch | `ID3D12GraphicsCommandList6::DispatchMesh` | `vkCmdDrawMeshTasksEXT` |
| Payload | `groupshared` struct + `DispatchMesh()` | `taskPayloadSharedEXT` + `EmitMeshTasksEXT()` |
| Output | `out vertices`/`out indices` arrays | `gl_MeshVerticesEXT`/`gl_PrimitiveTriangleIndicesEXT` |
| Compaction | Wave intrinsics (`WavePrefixCountBits`) | Shared-memory atomic (`atomicAdd`) |
| Constants | Per-frame CBV (256B) | Push constants (240B) |
| PSO creation | Pipeline state stream (`CD3DX12_PIPELINE_STATE_STREAM_AS/MS/PS`) | Standard `VkGraphicsPipelineCreateInfo` with null vertex input/IA |

