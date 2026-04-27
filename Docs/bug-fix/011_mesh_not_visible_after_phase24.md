# Bug #011 — Mesh Not Visible After Phase 24 Integration

**Date:** 2026-04-26  
**Status:** In Progress  
**Severity:** High  
**Affects:** Vulkan backend only

## Symptoms

After integrating Phase 24 (Clustered Lighting), the DamagedHelmet mesh is not visible in the viewport. ImGui overlay and background gradient are visible.

## Changes That May Have Caused It

1. **Swapchain render pass `LOAD_OP_CLEAR` → `LOAD_OP_LOAD`** — The original code had `clearValueCount=0` with `LOAD_OP_CLEAR`, which is technically UB per Vulkan spec but NVIDIA treated it as LOAD (preserving content). Fixing the validation error by providing clear values caused the swapchain to actually clear (wipe tonemapped content). Fixed by changing to `LOAD_OP_LOAD`.

2. **Deferred pipeline layout expansion** — `_deferredPipeLayout` changed from 2 to 3 descriptor sets (added set=2 for clustered lighting). Both `_deferredPipeline` and `_deferredIBLPipeline` are created with this layout.

3. **`DeferredSceneUBO` change** — `uint32_t rtEnabled; uint32_t _p2[3]` changed to `uint32_t rtEnabled; uint32_t numPointLights; uint32_t _p2[2]`. Same total size (16 bytes for last row).

4. **`deferred_lighting_ibl_vk.frag.glsl`** — Added set=2 descriptor declarations and point light accumulation loop (guarded by `if (numPointLights > 0u)`, so skipped when no lights).

## Verified Working (via diagnostic logging)

- Zero validation errors (0 bytes stderr)
- All shaders compile successfully
- Phase 24 clustered lighting resources initialized (`_clusterLightDescSet` is non-null)
- IBL loaded and active
- `FlushDraws()` executes with `cpuInstances=1` (mesh is queued)
- `_deferredIBLPipeline` is active and bound correctly
- 3 descriptor sets are bound (`_clusterLightDescSet` is valid pointer)
- `DrawTonemap()` executes each frame with valid image indices
- Tonemap render pass writes to swapchain (finalLayout = PRESENT_SRC_KHR)
- ImGui overlay render pass preserves content (LOAD_OP_LOAD)

## Investigation Steps Completed

1. ✅ Added logging to FlushDraws — confirms `gpuDrivenReady=1, cpuInstances=1`
2. ✅ Added logging to deferred pass — confirms `_clusterLightDescSet` non-null, IBL pipeline bound
3. ✅ Added logging to DrawTonemap — confirms execution with valid descriptors
4. ✅ Checked for validation errors — none in stderr
5. ✅ Verified shader compilation — all shaders compile successfully
6. ⏳ RenderDoc capture needed to verify G-buffer content

## Investigation Steps Remaining

1. **Visual verification** — Run with `DEBUG_GBUFFER 1` or `DEBUG_SURFACE 99` to see if mesh renders
2. **DX12 comparison** — Run with `--dx12` to confirm mesh visible on D3D12 backend
3. **RenderDoc capture** — Capture a frame to verify:
   - G-buffer has content (depth < 1.0 where mesh should be)
   - GPU cull pass produces draw count > 0
   - Indirect draw arguments are valid

## Debug Modes Available

- **G-buffer frag shader**: `gbuffer_indirect_vk.frag.glsl` line 20: `#define DEBUG_SURFACE 99` → flat green output
- **Deferred shader**: `deferred_lighting_ibl_vk.frag.glsl` line 96: `#define DEBUG_GBUFFER 1` → show albedo directly

If green appears with both debug modes enabled, the indirect draw is working. If only gradient appears, the issue is in GPU culling or indirect draw execution.

## Likely Root Cause Theories

- **Theory A (UNLIKELY):** Shader compiler difference with set=2 SSBO declarations — but shaders compile and no errors
- **Theory B (UNLIKELY):** Stale `_tonemapDescSet` — but logging shows valid descriptors bound
- **Theory C:** imgui.ini window layout covering viewport — check if viewport is 0-sized
- **Theory D:** Hi-Z culling with depth=1.0 pyramid on first frame — but depth 1.0 should NOT cull (ndc.z < 1.0 passes)
- **Theory E:** Frustum plane extraction row-major/column-major mismatch — investigate matrix conventions

## Recommended Next Steps

1. Run app and visually confirm if background gradient or solid color appears
2. Use RenderDoc to capture a frame and inspect:
   - G-buffer color/depth content
   - Draw count buffer after GPU cull
   - Indirect argument buffer contents
3. If G-buffer is empty, focus on GPU cull → indirect draw path
4. If G-buffer has content but not visible, focus on deferred → tonemap path
