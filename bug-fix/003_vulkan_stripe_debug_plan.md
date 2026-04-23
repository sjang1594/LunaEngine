# 003 — Vulkan Diagonal Metallic Stripe Debug Plan

## Current State (Session 3 — Updated)

### Confirmed Facts
1. **Diagonal metallic stripes** persist across ALL rendering configurations
2. **Texture data is correct** — stb_image loads correct pixel values (verified via logging): albedo 2048×2048, metalRough 2048×2048, normal 2048×2048
3. **Stripes are NOT from TAA** — present with TAA alpha=1.0 AND Phase 16C (no TAA at all)
4. **Stripes are NOT from bloom** — present with Phase 16C (no bloom)
5. **Stripes are NOT from tonemapping** — present with both ACES hue-preserving and Phase 16C simple tonemap
6. **Stripes are in BOTH legacy and indirect paths** — legacy DrawMesh path also shows wrong rendering
7. **Flat red test (1,0,0) from deferred lighting shows RED** — post-process pipeline doesn't swap channels
8. **Flat green from G-buffer frag shows SOLID GREEN** — G-buffer write/read pipeline is correct
9. **Fixed UV(0.5,0.5) texture sample shows SOLID COLOR, no stripes** — texture binding is correct
10. **UV debug (DEBUG_SURFACE=1) shows stripes at triangle edges** — but these may be normal UV seams
11. **Material factors**: albedo=(1,1,1,1), metallic=1.0, roughness=1.0 — matches DamagedHelmet glTF
12. **SPIR-V locations verified**: DXC assigns POSITION=0, NORMAL=1, TEXCOORD0=2, TANGENT=3, BITANGENT=4 — matches GLSL fragment shader expectations
13. **Stripes NOT from specular** — present even with diffuse-only rendering
14. **Stripes NOT from shadows** — present with `shadow = 1.0` forced
15. **Stripes NOT from SSAO** — present with `ao = 1.0` forced
16. **Stripes NOT from normal mapping** — present with vertex normals only (TBN bypassed)
17. **Color is NOT channel-swapped** — user confirmed
18. **`[[vk::location()]]` annotations added to pbr_forward.vert.hlsl** — no effect (locations were already correct)

### What We Know Works
- G-buffer pipeline: write constant → read back → display ✅
- Texture binding: fixed UV samples correct color ✅
- Post-process chain: flat color passes through correctly ✅
- Vertex shader output locations: SPIR-V verified ✅
- Texture data: pixel values verified ✅

### What Still Fails
- **Normal texture-mapped rendering shows diagonal metallic stripes** in both legacy and indirect paths
- Stripes visible even with diffuse-only (no specular, no shadows, no SSAO, no normal map)
- UV debug shows discontinuities at triangle edges (may be normal UV seams or actual corruption)

### Remaining Suspects

#### 1. UV Seam Artifacts Amplified by Deferred Rendering
The DamagedHelmet has many UV seams. At seam edges, adjacent triangles have different UVs at shared vertices. In a deferred renderer with MSAA off (as is the case), these seams create 1-pixel wide discontinuities in the G-buffer. The deferred lighting then reads these edge pixels which have interpolated/wrong values, creating visible "stripe" patterns. DX12 might handle this differently (e.g., different subpixel precision, different rasterization rules).

**Test**: Compare DX12 rendering side-by-side. If DX12 also shows slight seam artifacts but they're less visible, this is a deferred rendering + no-MSAA expected artifact.

#### 2. Depth Precision / Z-Fighting at Triangle Edges
If the depth buffer has precision issues, adjacent triangles could Z-fight at their shared edges, causing flickering/stripes where the wrong triangle "wins" and shows its own UV/albedo.

**Test**: Set `dss.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL` instead of `VK_COMPARE_OP_LESS` in the indirect G-buffer pipeline.

#### 3. Legacy Path Rendering Over Indirect Path
Both render passes write to the same G-buffer. The first pass (LOAD_OP_CLEAR) runs the legacy pipeline, the second (LOAD_OP_LOAD) runs the indirect pipeline. If the legacy path accidentally draws something, it could interfere.

**Test**: Add `return;` at the top of `DrawMesh` to ensure zero legacy draws.

#### 4. Vulkan vs DX12 Rasterization Rule Differences
Vulkan and DX12 have slightly different rasterization rules (top-left rule vs spec differences). At triangle edges, different pixels may be covered, creating different artifacts.

**Test**: This would be a Vulkan-inherent difference, not a bug.

#### 5. The `_gbRenderPass` (CLEAR) Winding/Cull Mode vs `_gbRenderPassLoad` (LOAD)
The indirect pipeline uses `_gbRenderPassLoad` with `VK_CULL_MODE_BACK_BIT` and `VK_FRONT_FACE_CLOCKWISE`. If the winding doesn't match the vertex shader's Y-flip, some triangles could be culled incorrectly, leaving gaps.

**Test**: Try `VK_CULL_MODE_NONE` to see if stripes disappear.

### Files Currently Modified (DEBUG — need cleanup)
1. **`VulkanBackend.cpp`**:
   - Texture diagnostic LUNA_LOG_INFO for Mat[0] (lines ~913-940)
   - BuildMergedGeometry/CreateIndirectResources diagnostic logging (from earlier session)
2. **`deferred_lighting_vk.frag.glsl`**: Has `DEBUG_DEFERRED` toggle (modes 0-7), set to 0
3. **`tonemapping_vk_full.frag.glsl`**: Has `DEBUG_TONEMAP` toggle, set to 0
4. **`tonemapping_vk.frag.hlsl`**: Updated to hue-preserving ACES
5. **`tonemapping_vk_full.frag.hlsl`**: Updated to hue-preserving ACES
6. **`pbr_forward.vert.hlsl`**: Added `[[vk::location()]]` annotations (harmless, keeps correct behavior)
7. **`pbr_indirect_vk.vert.glsl`**: Has `DEBUG_IDENTITY_MODEL` toggle (from earlier session)
8. **`gbuffer_indirect_vk.frag.glsl`**: Has `DEBUG_SURFACE` toggle + modes 98/99 added, set to 0

### Next Steps — Priority Order

#### Step A: Compare DX12 Side-by-Side
Run DX12 to see if DamagedHelmet shows ANY seam artifacts there too. If DX12 is perfectly clean, the issue is Vulkan-specific.

#### Step B: Try VK_CULL_MODE_NONE
In the indirect G-buffer pipeline creation (~line 4796), change:
```cpp
rs.cullMode = VK_CULL_MODE_NONE;  // was VK_CULL_MODE_BACK_BIT
```
If stripes disappear, the winding/cull mode is wrong for some triangles.

#### Step C: Check if Legacy Path Interferes
Add early return to DrawMesh when `_gpuDrivenReady` is true AND no material:
```cpp
if (_gpuDrivenReady) { _drawCallIndex++; return; }
```

#### Step D: RenderDoc Capture
Capture a frame in RenderDoc and inspect:
- G-buffer albedo attachment pixel-by-pixel at stripe locations
- Verify which draw calls write to the G-buffer
- Check depth buffer at stripe locations for Z-fighting

### Build & Run Commands
```powershell
# Kill, rebuild, run Vulkan
Stop-Process -Name LunaApp -Force -ErrorAction SilentlyContinue; Start-Sleep -Seconds 1
cd C:\Users\Administrator\Documents\project\personal\LunaEngine-source
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" LunaApp.sln /p:Configuration=Debug /p:Platform=x64 /m /t:Rebuild 2>&1 | Select-Object -Last 5
cd LunaApp\bin\Debug-windows-x86_64\LunaApp
Start-Process -FilePath .\LunaApp.exe -ArgumentList "--vulkan"
# For DX12:
Start-Process -FilePath .\LunaApp.exe
```

### Key File Locations
- `LunaEngine/src/LunaEngine/Shaders/deferred_lighting_vk.frag.glsl` — non-IBL deferred lighting (ACTIVE)
- `LunaEngine/src/LunaEngine/Shaders/gbuffer_vk.frag.glsl` — legacy G-buffer frag
- `LunaEngine/src/LunaEngine/Shaders/gbuffer_indirect_vk.frag.glsl` — indirect G-buffer frag
- `LunaEngine/src/LunaEngine/Shaders/pbr_indirect_vk.vert.glsl` — indirect vertex shader
- `LunaEngine/src/LunaEngine/Shaders/pbr_forward.vert.hlsl` — legacy vertex shader (DXC compiled)
- `LunaEngine/src/LunaEngine/Shaders/tonemapping_vk_full.frag.glsl` — Phase 17 full tonemap
- `LunaEngine/src/LunaEngine/Renderer/Vulkan/Private/VulkanBackend.cpp` — main backend (~6214 lines)
