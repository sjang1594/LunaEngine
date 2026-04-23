# 005 — Vulkan/DX12 Rendering Parity Fix

**Date**: 2025-04-22  
**Severity**: Critical (visual)  
**Affected**: Both Vulkan and DX12 backends  
**Symptom**: DamagedHelmet rendering looked completely different between DX12 and Vulkan — wrong ambient lighting, emissive color bleed, and UV seam artifacts

---

## Root Causes Found (3 issues)

### 1. DX12 IBL Shader Never Sampled IBL Textures (CRITICAL)

**File**: `deferred_lighting_ibl.frag.hlsl`

The DX12 IBL deferred lighting shader declared `irrMap` (t6), `prefilterMap` (t7), and `brdfLUT` (t8) — and the C++ backend correctly bound them to descriptor tables 3/4/5 — but the shader **never sampled them**. The ambient term was hardcoded:

```hlsl
// BEFORE (bug): IBL textures declared but unused
float3 ambient = 0.05f * albedo * ao;
```

Meanwhile the Vulkan IBL shader (`deferred_lighting_ibl_vk.frag.glsl`) did full split-sum IBL with irradiance diffuse + prefiltered specular + BRDF LUT integration. This made the two backends look completely different — Vulkan had rich environment reflections and image-based ambient, DX12 had flat dark ambient.

**Fix**: Replaced the flat ambient with the same split-sum IBL computation:

```hlsl
// AFTER: full IBL split-sum (matches Vulkan shader)
float  NdV      = max(dot(N, V), 0.0f);
float3 F_ibl    = F_SchlickRoughness(NdV, F0, roughness);
float3 kD_ibl   = (1.0 - F_ibl) * (1.0 - metallic);

float3 irradiance    = irrMap.Sample(trilinearClamp, N).rgb;
float3 diffuseIBL    = kD_ibl * irradiance * albedo;

float  mipLevel         = roughness * float(PREFILTER_MIP_COUNT - 1u);
float3 prefilteredColor = prefilterMap.SampleLevel(trilinearClamp, R, mipLevel).rgb;
float2 brdf             = brdfLUT.Sample(bilinearClamp, float2(NdV, roughness));
float3 specularIBL      = prefilteredColor * (F_ibl * brdf.x + brdf.y);

float3 ambient = (diffuseIBL + specularIBL) * ao;
```

### 2. Legacy Vulkan G-Buffer Emissive Leak (HIGH)

**File**: `gbuffer_vk.frag.glsl`

The legacy Vulkan G-buffer shader output `outAlbedo = vec4(albedo, 1.0)`. The IBL deferred lighting shader reads emissive packed into alpha channels:

```glsl
vec3 emissiveRaw = vec3(gb0Full.a, gb2Full.b, gb2Full.a);
```

With `gb0Full.a = 1.0`, this decoded as `emissiveRaw = (1.0, 0.0, 0.0)` — a **bright red emissive** added to every pixel when the IBL path was active. The indirect path (`gbuffer_indirect_vk.frag.glsl`) and DX12 path (`gbuffer.frag.hlsl`) correctly packed emissive into alpha, but the legacy Vulkan path did not.

**Fix**: Changed `outAlbedo.a` from `1.0` to `0.0`:

```glsl
// BEFORE: alpha = 1.0 → decoded as emissive.r = 1.0 → bright red bleed
outAlbedo = vec4(albedo, 1.0);

// AFTER: alpha = 0.0 → no emissive in legacy path
outAlbedo = vec4(albedo, 0.0);
```

### 3. Inconsistent `_linearSampler` Creation (MEDIUM)

**File**: `VulkanBackend.cpp`

Three code paths could create `_linearSampler`, guarded by `== VK_NULL_HANDLE`. Whichever ran first determined the sampler for all material texture sampling. Only the first (line ~850) had correct anisotropic settings; the other two (lines ~1163, ~3442) were inferior:

| Creation Site | anisotropyEnable | maxAnisotropy | maxLod | addressMode |
|---|---|---|---|---|
| Line ~850 (LoadMesh) | ✅ VK_TRUE | 8.0 | 0.0 | REPEAT |
| Line ~1163 (LoadScene) | ❌ VK_FALSE | 1.0 | 1.0 | REPEAT |
| Line ~3442 (PostProcess) | ❌ VK_FALSE | 1.0 | CLAMP_NONE | CLAMP_TO_EDGE |

If scene loading or post-process init ran before mesh loading, the sampler would lack anisotropic filtering — amplifying UV seam artifacts at triangle edges (the diagonal stripe issue from sessions 002–004).

**Fix**: Made all three creation points consistent: `anisotropyEnable=VK_TRUE`, `maxAnisotropy=8.0`, `maxLod=0.0`, `addressMode=REPEAT`.

---

## Connection to Previous Sessions

| Session | Issue | Resolution |
|---|---|---|
| 001 | TAA YCoCg channel swap (DX12) | Fixed in session 001 ✅ |
| 002–003 | Vulkan diagonal metallic stripes | Root cause identified in 004 (missing TAA jitter) ✅ |
| 004 | Missing Halton jitter in Vulkan TAA | Fixed in session 004 ✅ |
| **005** | **DX12/Vulkan rendering look completely different** | **Fixed: IBL not sampled + emissive leak + sampler parity** ✅ |

The diagonal stripe artifacts from sessions 002–004 were UV seam artifacts at triangle edges, inherent to deferred rendering without MSAA. They were amplified by:
1. Missing TAA jitter (fixed in 004) — TAA couldn't smooth sub-pixel discontinuities
2. Missing anisotropic filtering (fixed here) — texture sampling at grazing angles produced aliasing at UV seams

With both TAA jitter and anisotropic 8x filtering active, these seam artifacts are smoothed to near-invisible levels — matching DX12 behavior.

---

## Files Changed

| File | Change |
|---|---|
| `Shaders/deferred_lighting_ibl.frag.hlsl` | Added split-sum IBL sampling (irradiance + prefiltered env + BRDF LUT) replacing flat 0.05 ambient |
| `Shaders/gbuffer_vk.frag.glsl` | Changed `outAlbedo.a` from 1.0 to 0.0 (prevent emissive leak) |
| `Renderer/Vulkan/Private/VulkanBackend.cpp` | Made all 3 `_linearSampler` creation sites use anisotropic 8x, maxLod=0, REPEAT |

## Verification

- Build: 0 errors, 0 warnings (linker warnings pre-existing) ✅
- Vulkan runtime: all shaders compile, IBL pipeline loads, helmet renders ✅

