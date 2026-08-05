# Phase 30 — Global Illumination (SSGI + Irradiance Probes)

**Date:** 2026-04-30
**Backends:** DX12 + Vulkan
**Dependencies:** Phase 23 (Hi-Z pyramid), Phase 15C (IBL), Phase 24 (Clustered lighting)

---

## Overview

Phase 30 adds two-tier indirect lighting on top of the existing IBL ambient term:

1. **SSGI** — screen-space rays for near-field diffuse bounce (geometry visible in the current frame contributing light back onto itself)
2. **Irradiance probes** — sparse world-space probe grid for low-frequency off-screen indirect (areas outside the frustum that SSGI cannot reach)

Both tiers feed into a new deferred lighting pipeline variant that replaces the IBL-only pass when GI is active.

---

## Why Two Tiers

IBL gives plausible ambient from the environment map but is view-independent and spatially uniform — every point at the same roughness/normal receives identical irradiance regardless of local geometry. SSGI fixes the near-field (short bounce paths within the view), but breaks down at the frustum boundary. Probes fill the gap for long-range indirect without the cost of full path tracing.

```
IBL alone:       uniform sky-derived ambient, no local bounce
IBL + SSGI:      local bounce within screen, seams at edges
IBL + SSGI + GI: smooth blend — SSGI for near, probes for far
```

---

## Architecture

### Compute Pass (per-frame)

```
Hi-Z pyramid (Phase 23)
        ↓
SSGI dispatch  (half-res, 8×4×8×1 threads)
  ├─ Hammersley sequence → cosine hemisphere sample directions
  ├─ Ray march 16 steps against Hi-Z
  ├─ On hit: sample G-buffer albedo × NdL as radiance estimate
  └─ Temporal blend: new × α + history × (1−α)  →  ssgiImage[pingPong]
        ↓
Probe update dispatch  (8×4×8 grid, one pass)
  ├─ For each probe: sample sky + reflected radiance at probe position
  ├─ Octahedral encode into 16×16 texel patch in atlas
  └─ Write → probeAtlas (128×64×8, R16G16B16A16_FLOAT)
        ↓
Deferred GI lighting pass  (fullscreen)
  ├─ Sample ssgiImage[1−pingPong]  (read the previous write slot)
  ├─ Trilinear probe lookup: find 2–3 nearest probes, blend by distance
  └─ ambient = (iblAmbient + ssgiRadiance + probeRadiance × 0.3) × ao
```

### Ping-Pong Temporal Accumulation

SSGI uses two half-res RGBA16F images (`_ssgiImage[0]`, `_ssgiImage[1]`). Each frame:

- **Write** to `_ssgiImage[pingPong]`
- **Read history** from `_ssgiImage[1 − pingPong]`
- Flip: `pingPong ^= 1` after dispatch

The deferred pass always reads `GetSSGIReadView()` = `_ssgiView[1 − pingPong]`, which is the slot written last frame — a deliberate 1-frame lag that avoids any intra-frame write/read hazard on the same image.

---

## DX12 Implementation

### Resources

| Resource | Format | Size | Usage |
|----------|--------|------|-------|
| `_ssgiImage[2]` | R16G16B16A16_FLOAT | half-res × 2 | UAV (write) + SRV (read) |
| `_probeAtlas` | R16G16B16A16_FLOAT | 128×64×8 | UAV (update) + SRV (lighting) |
| `_probeGridCB` | — | 256 B | ProbeGridConstants CBV |

### Shaders

**`ssgi.comp.hlsl`** — SSGI compute
- Reconstructs world position from depth + invViewProj
- Generates 8 cosine-weighted directions via Hammersley sequence
- Marches each ray against the Hi-Z mip pyramid (binary search refinement, 16 steps max)
- On hit: reads G-buffer albedo, computes `radiance = albedo × max(dot(N, L), 0)`
- Temporal blend with history via `prevViewProj` reprojection

**`probe_update.comp.hlsl`** — probe atlas update
- Each thread group handles one probe
- Samples hemisphere directions, looks up sky LUT + scene depth
- Accumulates irradiance, encodes into octahedral patch in the atlas

**`deferred_lighting_gi.frag.hlsl`** — GI-extended deferred lighting
```hlsl
// Bindings added on top of existing IBL set:
Texture2D    ssgiTex      : register(t12);
Texture2DArray probeIrrArray : register(t13);

// In lighting loop:
float3 ssgiRadiance  = ssgiTex.SampleLevel(linearClamp, uv, 0).rgb;
float3 probeRadiance = SampleProbeIrradiance(posWS, N, probeIrrArray);
float3 ambient = (iblAmbient + ssgiRadiance + probeRadiance * 0.3) * ao;
```

**Root signature**: `DeferredLightingGI` — 12 parameters (extends IBL root sig with t12/t13 + ProbeGridConstants CBV).

### Pipeline Selection (`CompositeFrame`)

```
_ssgiReady && _iblReady && _lightingPipelineGI
    → _lightingPipelineGI   (GI path)
_iblReady
    → _lightingPipelineIBL  (IBL-only path)
else
    → _lightingPipeline     (flat ambient fallback)
```

---

## Vulkan Implementation

### VulkanGI Subsystem

`VulkanGI` owns all GI compute resources and is created inside `LoadHDREnvironment()` (same point IBL is ready, ensures cluster set is also available).

**Image layout policy**: All SSGI and probe atlas images stay in `VK_IMAGE_LAYOUT_GENERAL` throughout their lifetime. This simplifies the barrier logic — the only transitions are access-mask changes (SHADER_WRITE → SHADER_READ) with layout remaining GENERAL. Descriptor writes must use `VK_IMAGE_LAYOUT_GENERAL`, not `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`.

### Descriptor Set Layout (set = 3)

```
binding 0  SAMPLED_IMAGE      ssgiTex          (read SSGI ping-pong slot)
binding 1  SAMPLED_IMAGE      probeIrrTex      (probe atlas array)
binding 2  SAMPLER            giSampler        (bilinear, clamp-to-edge)
binding 3  UNIFORM_BUFFER     ProbeGridData    (origin, spacing, dims)
```

Full 4-set pipeline layout: `{scene UBO, G-buffer+shadow+IBL, cluster lights, GI}`.
Vulkan requires all sets 0..N−1 to be valid when binding up to set N, so the GI pipeline is only activated when `_clusterLightDescSet != VK_NULL_HANDLE` (set=2).

### ProbeGridData UBO

```cpp
struct ProbeGridUBO {
    float    origin[4];    // xyz + pad  (16 B)
    float    spacing[4];   // xyz + pad  (16 B)
    uint32_t dims[4];      // xyz + pad  (16 B)
};                         // total: 48 B, allocated 256 B for alignment
```

Mapped persistently (HOST_VISIBLE | HOST_COHERENT). Updated in `UpdateGIDescriptorSet()` each frame from `_gi.probeOrigin`, `_gi.probeSpacing`, and the compile-time `PROBE_GRID_X/Y/Z` constants.

### CreateGIDeferredResources()

Called once after `_gi.Create()` succeeds:

1. Create `_giSampler` (VK_FILTER_LINEAR, CLAMP_TO_EDGE, no anisotropy)
2. Allocate `_giProbeGridUBO[i]` / `_giProbeGridMem[i]` per frame, map permanently
3. Create `_giDescLayout` with 4 bindings above
4. Create `_giDescPool` (SAMPLED_IMAGE×2 + SAMPLER×1 + UNIFORM_BUFFER×1, `maxSets = FRAMES_IN_FLIGHT`)
5. Allocate `_giDescSet[i]` and call `UpdateGIDescriptorSet(i)` for initial write
6. Create `_deferredGIPipeLayout` with 4 set layouts
7. Compile `deferred_lighting_gi_vk.frag.glsl` + `fullscreen.vert.hlsl` → `_deferredGIPipeline`
   (same fixed-function state as `_deferredIBLPipeline`, targeting `_ppRenderPass`)

### CompositeFrame() Pipeline Selection

```cpp
bool useGI  = _gi.IsReady() && _deferredGIPipeline && _clusterLightDescSet;
bool useIBL = !useGI && _ibl.IsReady() && _deferredIBLPipeline;

VkPipeline       pipe   = useGI  ? _deferredGIPipeline
                        : useIBL ? _deferredIBLPipeline
                                 : _deferredPipeline;
VkPipelineLayout layout = useGI  ? _deferredGIPipeLayout : _deferredPipeLayout;

if (useGI) {
    UpdateGIDescriptorSet(_frameIndex);  // CPU memcpy + vkUpdateDescriptorSets
    VkDescriptorSet sets[] = { sceneUBO, gbuffer, clusterLights, gi };
    vkCmdBindDescriptorSets(..., 0, 4, sets, 0, nullptr);
}
```

`UpdateGIDescriptorSet` is called inside the Execute lambda — a CPU-side `vkUpdateDescriptorSets` call, safe because the per-frame fence guarantees the GPU is done with this frame's set before the CPU writes to it again.

---

## Visual Impact

| Condition | Before (IBL only) | After (IBL + GI) |
|-----------|-------------------|------------------|
| Open scene, direct sunlight | Uniform dark ambient on shadowed surfaces | Slight warm bounce from ground/walls |
| Concave geometry (corners, alcoves) | Uniform ambient — same as open sky | Reduced indirect in tight spaces (nearby occluders reduce probe visibility) |
| Coloured walls | No colour bleed | Adjacent-surface colour bleeds into bounce term |
| Off-screen occluders | No effect | Probe grid carries their irradiance contribution |
| First frame | SSGI history empty → full `α` blend → only direct sample | Smooth temporal buildup over ~4 frames |

The effect is most visible in enclosed scenes (Sponza interior, any room geometry) and on rough surfaces. Smooth/metallic surfaces are dominated by the specular IBL term and show less change.

---

## Limitations

- **SSGI is screen-space only** — fast-moving objects leave temporal ghosting until the `temporalAlpha` hysteresis decays the history
- **Probe grid is static** — probe positions are uniform and don't adapt to scene geometry; probes inside walls receive incorrect irradiance
- **No probe visibility** — neighbouring probe weights are distance-only, no ray visibility test between probe and shaded point (DDGI uses a depth atlas for this)
- **Half-res SSGI** — bilinearly upsampled to full-res in the fragment shader; thin geometry (wires, railings) can show slight halo artifacts at discontinuities

---

## Files Modified

| File | Change |
|------|--------|
| `Shaders/ssgi.comp.hlsl` | SSGI compute shader (new) |
| `Shaders/probe_update.comp.hlsl` | Probe atlas update compute (new) |
| `Shaders/deferred_lighting_gi.frag.hlsl` | DX12 GI deferred lighting (new) |
| `Shaders/deferred_lighting_gi_vk.frag.glsl` | Vulkan GI deferred lighting (new) |
| `Renderer/Vulkan/Public/VulkanGI.h` | VulkanGI subsystem header (new) |
| `Renderer/Vulkan/Private/VulkanGI.cpp` | VulkanGI subsystem implementation (new) |
| `Renderer/Vulkan/Public/VulkanBackend.h` | Added GI pipeline/descriptor/UBO members |
| `Renderer/Vulkan/Private/VulkanBackend.cpp` | `CreateGIDeferredResources`, `DestroyGIDeferredResources`, `UpdateGIDescriptorSet`, `CompositeFrame` GI path |
| `Renderer/DX12/private/DX12Backend.cpp` | `CreateSSGIResources`, `DispatchSSGI`, `DispatchProbeUpdate`, GI pipeline in `CompositeFrame` |
