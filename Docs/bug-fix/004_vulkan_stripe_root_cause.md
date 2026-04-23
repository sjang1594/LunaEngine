# 004 — Vulkan Triangle Edge Stripe Root Cause Analysis

## Root Cause: Missing TAA Jitter + UV Seam Artifacts

The "diagonal metallic stripes" are **UV seam artifacts** at triangle edges — an inherent property
of deferred rendering without MSAA on meshes with dense UV seams (DamagedHelmet).

**Why DX12 doesn't show them**: DX12 has working TAA with Halton(2,3) sub-pixel jitter applied
to the projection matrix. This jitters the G-buffer sampling position each frame, and TAA's
90% history blending smooths out the 1-pixel seam discontinuities over multiple frames.

**Why Vulkan showed them**: `_vkCurJitter` was initialized to `{0, 0}` and **never assigned
a jitter value**. The Vulkan TAA was running (blending history), but without jitter, every
frame sampled identical sub-pixel positions — the TAA effectively did nothing for anti-aliasing.

## Fixes Applied

### 1. TAA Halton Jitter (Root Cause Fix)
**File**: `VulkanBackend.cpp` — `UpdateMVP()`
- Added Halton(2,3) jitter sequence matching DX12's implementation
- Jitter applied to both `_lastProj` (legacy DrawMesh) and `_deferredProj` (deferred lighting)
- Unjittered VP stored separately (`_vkUnjitteredVP`) for TAA reprojection
- TAA constants use jittered invVP for depth reconstruction, unjittered prevVP for reprojection
- Indirect path (`FlushDraws`) uses jittered `_lastProj`

**File**: `VulkanBackend.h`
- Added `_vkUnjitteredVP[16]` and `_vkPrevUnjitteredVP[16]` member variables

### 2. Anisotropic Filtering (Parity Fix)  
**File**: `VulkanBackend.cpp` — `_linearSampler` creation
- Changed from `maxAnisotropy = 1.0` to `anisotropyEnable = VK_TRUE, maxAnisotropy = 8.0`
- Changed `maxLod` from `1.0` to `0.0` (only 1 mip level, prevent sampling nonexistent mip)
- Matches DX12's `D3D12_FILTER_ANISOTROPIC` with `MaxAnisotropy = 8`

## Remaining Work
- **TAA may need further tuning** — jitter is now applied but the TAA resolve pass
  (neighbourhood clamping, history rejection) may need debugging for full effectiveness
- **Multiple `_linearSampler` creation points** (lines ~850, ~1156, ~3435) should all be
  updated consistently with anisotropic settings
- Clean up debug toggles in GLSL shaders (`DEBUG_SURFACE`, `DEBUG_MR_TO_ALBEDO`, etc.)

## Verification
- **Box.glb** (simple geometry, no UV seams): clean in Vulkan ✅
- **DamagedHelmet.glb** (dense UV seams): stripes reduced with jitter, need TAA tuning

## Key Diagnostic Findings (Session 4)
| # | Test | Result |
|---|------|--------|
| 1 | Hardcoded constant → G-buffer | Solid — pipeline works ✅ |
| 2 | UV visualization | Smooth gradients — UVs correct ✅ |
| 3 | Texture sample → any G-buffer channel | Stripes at triangle edges ❌ |
| 4 | DX12 rendering | Clean — has working TAA with jitter ✅ |
| 5 | Box.glb (no UV seams) | Clean in Vulkan ✅ |
| 6 | `_vkCurJitter` inspection | Always {0,0} — never assigned! 🔥 |
