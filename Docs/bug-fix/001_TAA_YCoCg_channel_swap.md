# Bug Fix: TAA YCoCg Inverse Transform Channel Swap

**Date**: 2025-04-21  
**Severity**: Critical (visual)  
**Affected**: DX12 Backend — all rendered output  
**Symptom**: Gold/warm colors render as green, green renders as gold; overall purple/magenta tint accumulates over time

---

## Root Cause

`taa.frag.hlsl` — the `YCoCgToRGB()` inverse transform had **Co (Chroma Orange) and Cg (Chroma Green) swapped**.

The forward transform `RGBToYCoCg()` encodes as:

| Slot | Value |
|------|-------|
| `c.x` | Y (luma) |
| `c.y` | **Cg** + 0.5 |
| `c.z` | **Co** + 0.5 |

The buggy inverse decoded `c.y` as Co and `c.z` as Cg — the opposite of what was encoded.

### Impact

TAA blends 90% history + 10% current frame. The neighbourhood clamping step converts history to YCoCg, clamps, then converts back via the buggy inverse. Every frame, colors are slightly scrambled. Over ~10 frames the error saturates, producing a stable but **wrong** color palette.

**Concrete example** — pure red (1, 0, 0) round-trips through the buggy transform:

```
Encode: Y=0.25, Cg+0.5=0.25, Co+0.5=1.0
Buggy decode: R=0, G=0.75, B=0  →  red becomes green
```

---

## Fix

**File**: `LunaEngine/src/LunaEngine/Shaders/taa.frag.hlsl`

### Before (buggy)

```hlsl
float3 YCoCgToRGB(float3 c)
{
    float t = c.x - c.z + 0.5;
    return saturate(float3(t + c.y - 0.5, c.x + c.z - 0.5, t - c.y + 0.5));
}
```

### After (fixed)

```hlsl
float3 YCoCgToRGB(float3 c)
{
    // c.x = Y, c.y = Cg + 0.5, c.z = Co + 0.5
    float Co = c.z - 0.5;
    float Cg = c.y - 0.5;
    return saturate(float3(
        c.x + Co - Cg,   // R = Y + Co - Cg
        c.x + Cg,        // G = Y + Cg
        c.x - Co - Cg    // B = Y - Co - Cg
    ));
}
```

---

## Additional Improvement

**File**: `LunaEngine/src/LunaEngine/Shaders/tonemapping.frag.hlsl`

Replaced per-channel ACES filmic tone mapping with a **luminance-based hue-preserving variant**. Per-channel ACES is known to shift hues on saturated colors (e.g. saturated yellows shift toward green). The new approach tone-maps luminance only and scales the color proportionally:

```hlsl
float3 ACESFilmHuePreserving(float3 x)
{
    float lum = dot(x, float3(0.2126, 0.7152, 0.0722));
    if (lum <= 0.0) return float3(0, 0, 0);
    float toneMappedLum = ACESFilmLum(lum);
    return x * (toneMappedLum / lum);
}
```

---

## Diagnosis Process

| Step | Test | Result | Conclusion |
|------|------|--------|------------|
| 1 | Force `return float4(1,0,0,1)` in tonemapping | Red ✓ | Back buffer/swapchain OK |
| 2 | Output raw G-buffer albedo (no lighting) | Correct colors | Texture loading OK |
| 3 | Full PBR lighting, SSR disabled | Purple | Not SSR |
| 4 | Diffuse only (no specular) | Purple | Not specular |
| 5 | **Bypass TAA** (pass-through current frame) | **No purple** ✓ | **TAA is the culprit** |
| 6 | Fix `YCoCgToRGB()`, re-enable everything | **Correct colors** ✓ | Root cause confirmed |

---

## Files Changed

- `LunaEngine/src/LunaEngine/Shaders/taa.frag.hlsl` — fixed `YCoCgToRGB()`
- `LunaEngine/src/LunaEngine/Shaders/tonemapping.frag.hlsl` — hue-preserving ACES

