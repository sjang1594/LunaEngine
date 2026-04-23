# 002 — Vulkan Diagonal Stripe & Texture Artifacts Investigation

## Summary

Systematic investigation of the Vulkan indirect G-buffer rendering path for diagonal stripe and texture artifacts. All primary suspects were verified programmatically; no code-level bug was found.

## Verification Results

| # | Suspect | Method | Result |
|---|---------|--------|--------|
| 1 | RowMajor SPIR-V decorations missing | `spirv-dis` on compiled `.spv` | ✅ `RowMajor` present on `GPUObjectData.model`, `ViewProjCB.viewMatrix`, `ViewProjCB.projectionMatrix` |
| 2 | Vertex data / firstInstance mapping | Code review | ✅ `firstInstance = idx` in cull shader matches `gl_InstanceIndex` in vert shader; stride 48 matches `PBRVertex` |
| 3 | Texture VkFormat mismatch (SRGB vs UNORM) | `grep R8G8B8A8` in both backends | ✅ Both DX12 and Vulkan use `R8G8B8A8_UNORM` with manual `pow(2.2)` in lighting |
| 4 | UV/NDC reconstruction error | Mathematical analysis | ✅ `ndc.y = 1.0 - uv.y * 2.0` correctly undoes the vertex shader `gl_Position.y = -gl_Position.y` flip; `invViewProj` uses un-flipped projection, which is correct |
| 5 | ViewProjCB UBO layout mismatch | Layout comparison (std140 row_major vs CPU struct) | ✅ 128 bytes: two `XMFLOAT4X4` memcpy'd → matches std140 row_major mat4×2 |
| 6 | Normal map Y-flip | GLSL vs HLSL comparison | ✅ Neither path flips `tn.y`; glTF uses OpenGL convention (+Y up) — consistent |
| 7 | Front face / back-face culling | Code review | ✅ `VK_FRONT_FACE_CLOCKWISE` matches both DXC `-fvk-invert-y` (non-indirect) and manual Y-flip (indirect) |
| 8 | GPUObjectDataVK struct padding | Layout comparison (CPU 96B vs GLSL std430 96B) | ✅ Identical: mat4(64) + vec4(16) + uint(4) + uint(4) + uvec2(8) = 96B |
| 9 | Frustum plane extraction | Gribb-Hartmann derivation check | ✅ Transpose + row extraction matches row-vector convention |
| 10 | `_gbRenderPassLoad` format for metalRough | Code review (line 1881) | ✅ `atts[2] = atts[0]` inherits `R8G8B8A8_UNORM` — correct for metalRough |

## Diagnostic Toggles Added

### `pbr_indirect_vk.vert.glsl` — `DEBUG_IDENTITY_MODEL`
```glsl
#define DEBUG_IDENTITY_MODEL 0  // Set to 1 to use mat4(1.0) instead of SSBO model
```
- **If stripes disappear**: Issue is in SSBO data upload or row_major interpretation
- **If stripes persist**: Issue is in VP matrices, vertex data, or downstream

### `gbuffer_indirect_vk.frag.glsl` — `DEBUG_SURFACE`
```glsl
#define DEBUG_SURFACE 0  // 1=UV, 2=materialIndex, 3=vertex normal
```
- **Mode 1 (UV)**: Diagonal stripes in UV → vertex data or transform problem
- **Mode 1 (UV) clean**: Issue is in texture binding/sampling
- **Mode 3 (normal)**: Verify vertex normals are correct

## Recommended Next Steps

1. **Run with `DEBUG_SURFACE 1`** — If UVs show smooth gradients per mesh island, vertex data and transforms are correct. If diagonal patterns appear in UVs, the issue is upstream of texturing.

2. **Run with `DEBUG_IDENTITY_MODEL 1`** — If the mesh renders at origin without stripes, the SSBO model matrix data or its interpretation is the problem.

3. **RenderDoc capture** — Inspect:
   - `_mergedVB` vertex data (stride 48: pos@0, normal@12, uv@24, tangent@32)
   - `VkDrawIndexedIndirectCommand` buffer: verify `firstInstance` values
   - SSBO contents: verify model matrices are valid (not garbage/transposed)

4. **Compare non-indirect vs indirect** — If the legacy `DrawMesh` path renders correctly for the same geometry, the issue is isolated to the GPU-driven path.

5. **If all diagnostics pass** — The artifact may be a driver issue. Test on a different GPU vendor (NVIDIA vs AMD vs Intel).

## Runtime Diagnostic Results (Phase 2)

### Phase 15B Initialization — ✅ Confirmed Working
- `BuildMergedGeometry`: 1 vert group, 1 idx group → 14556 verts, 46356 indices, 1 mesh
- `CreateIndirectResources`: succeeded, `_mergedVB`/`_mergedIB`/`_meshInfoBuf` all non-null
- RT: 1 BLAS + TLAS built successfully

### DEBUG_SURFACE 1 (UV Visualization) — ✅ Clean
- UV output shows smooth gradients with B≈0 (99.2% of center pixels have B<10)
- Center average: R=98.0 G=200.5 B=0.5
- **No diagonal stripe artifacts visible in UV data**
- **Conclusion**: Vertex data, transforms (model + VP), and UV interpolation are all correct

### Albedo Passthrough (texture sampling only) — ✅ Clean
- Raw albedo from G-buffer shows smooth texture data, no stripe artifacts
- Values consistent with DamagedHelmet baseColor texture

### Full Render Gradient Analysis — No Obvious Diagonal Stripes
- Gradient StdDev: horizontal=20.9, vertical=46.3, diag_TL-BR=46.9, diag_TR-BL=47.4
- Both diagonal directions have near-equal gradients — no directional stripe bias
- FFT: dominant frequency=1-2 (no high-frequency periodic pattern)

### Shutdown Validation Errors (from stderr)
- `vkDestroyBuffer` on in-use buffers (6 occurrences) — missing `vkDeviceWaitIdle` before cleanup
- `vkFreeDescriptorSets` on in-use descriptor set
- `vkDestroyPipeline` on in-use pipeline
- Leaked `VkFramebuffer` objects (4 instances)
- **Note**: These are cleanup-order bugs, not related to rendering artifacts

## Conclusion

All diagnostic tests pass. The diagonal stripe artifact reported earlier could not be reproduced through pixel-level analysis. Possible explanations:
1. The artifact was intermittent / view-angle-dependent
2. The artifact was caused by a driver issue that has since been resolved
3. The artifact may only be visible in certain post-processing stages (TAA, bloom)

If the artifact reappears, use RenderDoc to capture and inspect the G-buffer attachments directly.

## Files Modified
- `LunaEngine/src/LunaEngine/Shaders/pbr_indirect_vk.vert.glsl` — Added `DEBUG_IDENTITY_MODEL` toggle
- `LunaEngine/src/LunaEngine/Shaders/gbuffer_indirect_vk.frag.glsl` — Added `DEBUG_SURFACE` toggle
- `LunaEngine/src/LunaEngine/Shaders/deferred_lighting_ibl_vk.frag.glsl` — Added `DEBUG_GBUFFER` toggle
- `LunaEngine/src/LunaEngine/Renderer/Vulkan/Private/VulkanBackend.cpp` — Added `CreateIndirectResources` failure log

