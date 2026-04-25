# Bug #010 — DX12 GPU-Driven Rendering Flickering

**Date:** 2026-04-25  
**Severity:** HIGH — persistent flickering on DX12 backend  
**Root Cause:** Multiple issues: normalization safety + Hi-Z race condition

## Symptom

DX12 backend exhibits severe flickering with objects appearing/disappearing randomly.

## Root Cause Analysis

### 1. Frustum Plane Normalization Issue

The `ExtractFrustumPlanes()` function's normalization could produce bad values (NaN/inf) if plane length was near zero. Fixed by inlining the extraction with a safety check:

```cpp
float len = XMVectorGetX(XMVector3Length(p));
if (len > 0.0001f)
{
    p = XMVectorScale(p, 1.0f / len);
    XMStoreFloat4(&cullCB.planes[i], p);
}
```

### 2. Hi-Z Texture Race Condition

The `_hizTexture` is a **single shared resource** across all frames. With `FRAMES_IN_FLIGHT=2`:
- Frame N: Reads Hi-Z, draws, builds new Hi-Z
- Frame N+1: May start while Frame N is still building Hi-Z → reads partially written data

**Solution**: Hi-Z occlusion culling **disabled** until `_hizTexture` is double-buffered.

### 3. Secondary Issues Fixed

- **`_objectDataBuffer`** — Changed from single buffer to per-frame array
- **Indirect buffers** — Created in UAV state instead of COMMON  
- **OMSetRenderTargets** — Added explicit call before ExecuteIndirect

## Current State

| Feature | Status |
|---------|--------|
| GPU Frustum Culling | ✅ Working |
| Hi-Z Occlusion Culling | ❌ Disabled (race condition) |
| Async Compute | ❌ Disabled (queue-level wait issues) |

## Files Changed

| File | Change |
|------|--------|
| `DX12Backend.cpp` | Inlined frustum plane extraction with safety check; disabled Hi-Z |
| `DX12Backend.h` | Added `_objectDataMapped[]` for per-frame buffer pointers |

## TODO

1. Double-buffer `_hizTexture` to enable Hi-Z occlusion culling
2. Redesign async compute to use separate command lists for true GPU overlap
