# Phase 25 — DX12 Mesh Shaders

**Date:** 2026-04-27  
**Backend:** DX12 only (SM 6.5)  
**Status:** ✅ Complete (build verified, runtime pending visual confirmation)

---

## Overview

Replace the Phase 12 vertex/index `ExecuteIndirect` pipeline with DX12 mesh shaders. An amplification shader (AS) performs per-meshlet frustum culling; a mesh shader (MS) reads vertex data from structured buffers and outputs triangles directly to the G-buffer. The input assembler is completely bypassed.

---

## Why Mesh Shaders

### Phase 12 (Before)

```
CPU uploads GPUObjectData[]
  → GPU compute: per-OBJECT frustum cull
    → ExecuteIndirect: IA fetches from merged VB/IB
      → VS transforms per-vertex
        → Rasterizer → PS → G-buffer
```

### Phase 25 (After)

```
CPU uploads GPUObjectData[]
  → Per object: DispatchMesh(ceil(meshletCount/32))
    → AS: per-MESHLET frustum cull (wave compact)
      → MS: cooperative vertex fetch from StructuredBuffer
        → Rasterizer → PS → G-buffer
```

### Key Differences

| Aspect | Phase 12 (ExecuteIndirect) | Phase 25 (Mesh Shaders) |
|--------|---------------------------|-------------------------|
| **Cull granularity** | Per-object (whole mesh) | Per-meshlet (~124 tris) |
| **Vertex fetch** | Fixed-function IA | Programmable `StructuredBuffer<PBRVertex>` |
| **Index format** | `uint32` per index (4 B) | Packed `uint8×3` per triangle (3 B) |
| **Draw call** | `ExecuteIndirect` | `DispatchMesh` (from AS) |
| **Pipeline stages** | VS → Rasterizer → PS | AS → MS → Rasterizer → PS |
| **Culling overhead** | Separate compute dispatch + UAV barrier + state transition | Inline in AS — zero barrier |
| **Thread model** | 1 VS per vertex (IA-driven) | 128 cooperative threads per meshlet |

The main advantage is **meshlet-level culling**: a 15K-triangle mesh splits into ~125 meshlets. Individual clusters behind walls or outside the frustum are rejected, instead of the all-or-nothing per-object test from Phase 12.

---

## Implementation

### 1. Meshlet Generation (`Meshlet.h`, `Meshlet.cpp`)

Greedy builder at load time:
- Walk triangles in index buffer order
- Add to current meshlet until 64-vertex or 124-triangle limit
- Per-meshlet bounding sphere from AABB center + max vertex distance
- Triangle indices packed: `idx0 | (idx1 << 8) | (idx2 << 16)` into `uint32`

```
DamagedHelmet: 15,452 tris → ~125 meshlets
Sponza:       ~262K tris  → ~2,100 meshlets
```

### 2. GPU Buffers (uploaded in `BuildMergedGeometry`)

| Buffer | Type | Content |
|--------|------|---------|
| `_meshletBuffer` | `StructuredBuffer<Meshlet>` | vertexOffset, triangleOffset, vertexCount, triangleCount |
| `_meshletBoundsBuffer` | `StructuredBuffer<MeshletBounds>` | center.xyz + radius per meshlet |
| `_meshletVertexBuffer` | `StructuredBuffer<uint>` | Global vertex indices into merged VB |
| `_meshletTriBuffer` | `StructuredBuffer<uint>` | Packed local triangle indices |
| `_meshletMeshInfoBuffer` | `StructuredBuffer<MeshletMeshInfo>` | Per-mesh meshlet offset + count |

### 3. Amplification Shader (`meshlet_cull.as.hlsl`)

- `[numthreads(32, 1, 1)]`
- Each thread tests one meshlet: transform bounding sphere to world → 6-plane frustum test
- `WavePrefixCountBits(visible)` compacts surviving meshlet indices into `groupshared` payload
- `DispatchMesh(WaveActiveCountBits(visible), 1, 1)` — zero-overhead dispatch of only visible meshlets

### 4. Mesh Shader (`gbuffer_mesh.ms.hlsl`)

- `[numthreads(128, 1, 1)]`, `[outputtopology("triangle")]`
- `SetMeshOutputCounts(vertCount, triCount)` from meshlet descriptor
- Vertex loop: `for (v = gtid; v < vertCount; v += 128)` — reads `gVertices[meshletVertices[offset + v]]`
- Triangle loop: `for (t = gtid; t < triCount; t += 128)` — unpacks `uint32 → uint3(i0, i1, i2)`
- Output attributes match `PSInput` exactly — no pixel shader changes needed

### 5. Root Signature (`MeshShaderGBuffer`)

```
params[0] b0  = MeshShaderConstants CBV (view/proj + frustum planes + object/meshlet info)
params[1] b1  = MaterialConstants CBV
params[2] b2  = materialIndex root constant (1 DWORD)
params[3] t0  = StructuredBuffer<GPUObjectData>
params[4] t1  = StructuredBuffer<Meshlet>
params[5] t2  = StructuredBuffer<MeshletBounds>
params[6] t3  = StructuredBuffer<PBRVertex>       (merged VB as SRV)
params[7] t4  = StructuredBuffer<uint>             (meshlet vertex indices)
params[8] t5  = StructuredBuffer<uint>             (meshlet triangle indices)
params[9]     = Unbounded SRV table t0+ space1     (bindless texture heap)
s0            = Static anisotropic sampler
```

### 6. PSO Creation

Uses pipeline state stream API (`D3D12_PIPELINE_STATE_STREAM_DESC`) because mesh shader PSOs cannot use `D3D12_GRAPHICS_PIPELINE_STATE_DESC`. Requires `ID3D12Device2::CreatePipelineState`.

### 7. Feature Detection

```cpp
D3D12_FEATURE_DATA_D3D12_OPTIONS7 opts7 = {};
device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &opts7, sizeof(opts7));
return opts7.MeshShaderTier >= D3D12_MESH_SHADER_TIER_1;
```

Fallback: `_meshShaderReady = false` → Phase 12 `ExecuteIndirect` path used unchanged.

---

## Files

| File | Type | Description |
|------|------|-------------|
| `Renderer/Meshlet.h` | New | Data structures + `BuildMeshlets()` declaration |
| `Renderer/Meshlet.cpp` | New | Greedy meshlet builder with bounding sphere |
| `Shaders/meshlet_cull.as.hlsl` | New | AS: per-meshlet frustum cull + wave compact |
| `Shaders/gbuffer_mesh.ms.hlsl` | New | MS: structured buffer vertex fetch → G-buffer |
| `Shaders/gbuffer_mesh.frag.hlsl` | New | PS: same G-buffer output, mesh shader root sig |
| `Graphics/IPipeline.h` | Modified | `MeshShaderGBuffer` enum + `meshShaderPipeline` flag |
| `DX12/Public/DX12Pipeline.h` | Modified | `InitializeMeshShader()`, AS/MS blobs |
| `DX12/Private/DX12Pipeline.cpp` | Modified | Root sig + PSO via stream API |
| `DX12/Public/DX12Device.h` | Modified | `SupportsMeshShaders()` |
| `DX12/Private/DX12Device.cpp` | Modified | Options7 mesh shader tier check |
| `DX12/Public/DX12Backend.h` | Modified | Meshlet buffers, pipeline, per-frame CB |
| `DX12/Private/DX12Backend.cpp` | Modified | Meshlet gen, resource create/destroy, DispatchMesh path |

