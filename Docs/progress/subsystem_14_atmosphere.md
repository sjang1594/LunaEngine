# Phase 28 — Atmosphere / Sky Rendering (Hillaire 2020)

**Date:** 2026-04-27
**Backend:** Vulkan
**Status:** ✅ Complete (build verified, Bug #015 fixed 2026-04-27)

---

## Overview

Physically-based atmosphere rendering based on Sébastien Hillaire's "A Scalable and Production Ready Sky and Atmosphere Rendering Technique" (EGSR 2020). Three LUTs (transmittance, multi-scattering, sky-view) computed via compute shaders, composited into the HDR buffer via a fullscreen pass. Renders dynamic sky, sun disk with limb darkening, and ground-bounce coloring based on sun position.

---

## Architecture

```
VulkanAtmosphere::Create()
  ├─ CreateLUTImages()         ← 3× RGBA16F images (transmittance 256×64, multiScatter 32×32, skyView 192×108)
  ├─ CreateSampler()           ← bilinear clamp
  ├─ CreateUBO()               ← per-frame AtmosphereUBO (176B, triple-buffered)
  ├─ CreateDescriptors()       ← desc sets for 3 compute + 1 composite pass
  ├─ CreateComputePipelines()  ← transmittance + multiScatter + skyView compute
  ├─ CreateCompositePipeline() ← fullscreen sky composite (VS+FS)
  └─ Precompute(cmd)           ← one-time: dispatch transmittance + multiScatter

Per-frame (in CompositeFrame render graph):
  Atmo SkyView LUT  → _atmosphere.Update(cmd, frameIndex, view, proj)
                       ← dispatch skyView compute with current sun dir + camera
  Sky Composite      → _atmosphere.DrawComposite(cmd, frameIndex)
                       ← fullscreen pass: sky for depth==1.0, passthrough for geometry
```

---

## Atmosphere Physics (Earth Defaults)

| Parameter | Value | Unit |
|-----------|-------|------|
| Rayleigh scattering | (5.802, 13.558, 33.1) × 10⁻⁶ | /m |
| Rayleigh scale height | 8000 | m |
| Mie scattering | 3.996 × 10⁻⁶ | /m |
| Mie absorption | 4.4 × 10⁻⁶ | /m |
| Mie scale height | 1200 | m |
| Mie phase asymmetry (g) | 0.8 | — |
| Ozone absorption | (0.650, 1.881, 0.085) × 10⁻⁶ | /m |
| Ozone center height | 25000 | m |
| Ozone width | 15000 | m |
| Ground radius | 6,360,000 | m |
| Atmosphere radius | 6,460,000 | m |
| Sun angular radius | 0.00935 | rad |
| Ground albedo | (0.3, 0.3, 0.3) | — |

---

## LUTs

| LUT | Size | Compute | Frequency |
|-----|------|---------|-----------|
| Transmittance | 256×64 RGBA16F | 64-step ray march, optical depth integration | Once at startup |
| Multi-scattering | 32×32 RGBA16F | 64 hemisphere dirs × 20-step ray march, infinite-order approx | Once at startup |
| Sky-view | 192×108 RGBA16F | 32-step ray march with Rayleigh+Mie phase, single+multi scatter | Per-frame |

---

## Shaders

| File | Type | Description |
|------|------|-------------|
| `atmosphere_common.glsl` | Include | Shared physics: phase functions, density profiles, ray-sphere intersect, LUT parameterization |
| `atmosphere_transmittance_vk.comp.glsl` | Compute | Transmittance LUT generation (64 steps) |
| `atmosphere_multiscatter_vk.comp.glsl` | Compute | Multi-scattering LUT (64 dirs × 20 steps + ground bounce) |
| `atmosphere_skyview_vk.comp.glsl` | Compute | Per-frame sky-view LUT (32 steps, single + multi scatter) |
| `atmosphere_composite_vk.frag.glsl` | Fragment | Sky composite: sky-view sampling for background, sun disk with limb darkening |
| `fullscreen_vk.vert.glsl` | Vertex | GLSL fullscreen triangle (new, equivalent to HLSL version) |

---

## Files

| File | Type | Description |
|------|------|-------------|
| `Vulkan/Public/VulkanAtmosphere.h` | New | Class declaration, `AtmosphereUBO` struct, lifecycle API |
| `Vulkan/Private/VulkanAtmosphere.cpp` | New | Full implementation: LUT creation, compute dispatch, composite pass |
| `Vulkan/Public/VulkanBackend.h` | Modified | Added `VulkanAtmosphere _atmosphere` member, include |
| `Vulkan/Private/VulkanBackend.cpp` | Modified | Init atmosphere after IBL, destroy in DestroyPipeline, 2 render graph passes in CompositeFrame |
| 6 shader files | New | See Shaders table above |

---

## Render Graph Integration

```
Existing passes:
  SSAO → SSAO Blur → RT Shadows → Deferred Lighting
  ↓
  NEW: Atmo SkyView LUT (compute, .SideEffect())
  NEW: Sky Composite (reads hDepth, writes hHDR, .SideEffect())
  ↓
  SSR → Motion Blur → TAA → Bloom → Tonemap
```

---

## Bug Fix Applied (Bug #015 — 2026-04-27)

Three bugs found and fixed after initial implementation:

### 1. Feedback Loop + Layout Mismatch (crash)
`DrawComposite()` was using `_ppRenderPass` (`DONT_CARE` loadOp, `initialLayout=UNDEFINED`), which discarded deferred lighting output and transitioned HDR to `COLOR_ATTACHMENT_OPTIMAL`. The `sceneTex` descriptor (binding 3) bound the same `_hdrView` as `SHADER_READ_ONLY_OPTIMAL` — invalid feedback loop → validation errors + access violation crash.

**Fix**: New `_atmosphereRenderPass` (owned) with `LOAD_OP_LOAD` + `initialLayout/finalLayout = SHADER_READ_ONLY_OPTIMAL`. Removed `sceneTex` binding entirely. Fragment shader now `discard`s scene pixels — `LOAD_OP_LOAD` preserves deferred lighting content without reading it as a sampler.

### 2. First-Frame skyView Barrier Bug
`if (_precomputed)` fired on frame 0, emitting a `SHADER_READ_ONLY_OPTIMAL → GENERAL` barrier for `_skyViewImage`. The image was still in `GENERAL` from `CreateLUTImages()` — wrong `oldLayout` confused the validation layer.

**Fix**: Replaced `_precomputed` check with `_skyViewReady` flag, set to `true` at the end of the first `Update()` call.

### 3. Render Graph Incorrect hHDR Declaration
Sky Composite declared `.Read(hHDR, SHADER_READ_ONLY_OPTIMAL)` + `.Write(hHDR, SHADER_READ_ONLY_OPTIMAL, COLOR_ATTACHMENT_WRITE)` — contradictory layout vs access. The new `_atmosphereRenderPass` handles the `SHADER_READ_ONLY → COLOR_ATTACHMENT → SHADER_READ_ONLY` transition internally via the render pass attachment spec.

**Fix**: Removed `.Read(hHDR, ...)` from Sky Composite render graph pass. `_atmosphereRenderPass` subpass dependency (`COLOR_ATTACHMENT_OUTPUT → COLOR_ATTACHMENT_OUTPUT`) ensures correct synchronization with deferred lighting.

---

## Next Steps

- **ImGui panel** for sun elevation/azimuth/intensity control
- **Aerial perspective LUT** (32×32×32 3D texture) for distant geometry fog
- **CSM sun direction coupling** — feed `_atmosphere.GetSunDirection()` to shadow cascade system
- **DX12 backend port**

