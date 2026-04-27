# LunaEngine — Next Phases Plan

**Last updated:** 2026-04-27
**Current state:** 25 phases complete (DX12 + Vulkan dual-backend, deferred PBR, DXR/VK RT shadows, GPU-driven indirect, render graph, full PP stack, IBL, multi-mesh scene support, GPU profiler, Hi-Z occlusion culling, Vulkan async compute, clustered lighting, DX12 mesh shaders)

---

## Tracks

The project has two independent tracks with separate phase numbering:

| Track | Prefix | Purpose | Status |
|-------|--------|---------|--------|
| **Rendering** | Phase 26, 27, ... | Graphics engine features (rendering pipeline, GPU optimization) | **Active** |
| **Simulation** | S1, S2, S3, ... | Sensor simulation (camera/LiDAR/radar models, data export) | **Deferred** — starts after rendering track |

Simulation phases S2–S3 depend on rendering infrastructure (offscreen render targets, RayQuery compute). These may be naturally provided by rendering phases.

---

## Execution Order (Rendering Track)

```
Phase 21 ✅ → 22 ✅ → 23 ✅ → 20 ✅ → 24 ✅ → 25 ✅ → 26 ✅ → 27 ✅ → 28 ✅ → 29+
```

---

## Phase 21 — Multi-Mesh Scene (Sponza / Bistro) ✅ COMPLETE

**Priority:** ★★★ | **Effort:** Medium | **Dependencies:** None

### What Was Implemented

1. **glTF node tree traversal** — Both DX12 (`MeshLoader::LoadGLTF`) and Vulkan (`VulkanBackend::LoadMeshes`) now walk the node hierarchy via `cgltf_node_transform_local()` instead of iterating `data->meshes[]` directly. Each primitive gets the accumulated world transform from its parent node chain.

2. **Per-mesh transforms** — `LoadResult::transforms` stores one `XMFLOAT4X4` per mesh. `IRenderBackend::GetLastLoadTransforms()` exposes them to `SceneManager`.

3. **Transform raw matrix** — `Transform::SetWorldMatrix()` stores a raw `XMFLOAT4X4` that `GetWorldMatrix()` returns directly (bypasses SRT decomposition). Supports glTF nodes with non-trivial transforms.

4. **Texture decode dedup** — `MeshLoader::LoadGLTF` caches decoded `cgltf_image*` → pixel data so shared textures are decoded only once. Significant speedup for Sponza (many materials share normal maps).

5. **Configurable scene asset** — `SceneManager::SetSceneAsset()` + `--scene` CLI argument. Usage: `LunaApp.exe --scene Sponza/Sponza.gltf` or `LunaApp.exe --scene DamagedHelmet.glb` (default).

6. **SceneManager transform application** — Each `GameObject` receives its mesh's world transform via `Transform::SetWorldMatrix()`, which `MeshRenderer::Render()` feeds into `DrawMesh()`.

### How to Load Sponza

1. Download [Intel Sponza glTF](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/Sponza) or any glTF 2.0 scene
2. Place in `LunaApp/Assets/Sponza/`
3. Run: `LunaApp.exe --scene Sponza/Sponza.gltf` (DX12) or `LunaApp.exe --scene Sponza/Sponza.gltf -vk` (Vulkan)

---

## Phase 22 — GPU Profiler Overlay ✅ COMPLETE

**Priority:** ★★★ | **Effort:** Low | **Dependencies:** Phase 21 (useful with real scene)

### What Was Implemented

1. **Profiler infrastructure already existed** — `IGPUProfiler`, `DX12GPUProfiler`, `VulkanGPUProfiler`, `GPUProfilerOverlay` were already coded but not fully wired up.

2. **DX12 timestamps completed** — Added missing `InsertBeginTimestamp`/`InsertEndTimestamp` for CSM Shadows, DXR Shadows, GBuffer Fill passes in `DrawFrame()`. Fixed missing `InsertEndTimestamp` for Deferred Lighting pass.

3. **Vulkan profiler fully integrated** — Added `VulkanGPUProfiler _gpuProfiler` member to `VulkanBackend`. Wired `Init()`, `BeginFrame()`, `EndFrame()`, `ReadbackResults()`, `Shutdown()`. Added `vkCmdResetQueryPool` at frame start (required by spec). Timestamps for CSM, GBuffer, GPU Cull, Indirect Draw inserted manually.

4. **Automatic render graph timestamps** — Extended `VulkanRenderGraph::Execute()` to accept optional `VulkanGPUProfiler*`. When provided, auto-wraps each pass with `WriteBeginTimestamp`/`WriteEndTimestamp` using the pass name. All CompositeFrame passes (SSAO, SSAO Blur, RT Shadows, Deferred Lighting, SSR, Motion Blur, TAA, Bloom ×3, Tonemap) automatically instrumented.

5. **Overlay rendered from Application layer** — `GPUProfilerOverlay::Render()` called from `Application::Run()` before `ImGui::Render()`, using `IRenderContext::GetBackend()->GetGPUProfiler()`. Backend-agnostic — works with both DX12 and Vulkan.

### Measured Passes

| Pass | DX12 | Vulkan |
|------|------|--------|
| CSM Shadow | ✓ | ✓ |
| DXR/VK RT Shadows | ✓ | ✓ |
| G-Buffer Fill | ✓ | ✓ |
| GPU Frustum Cull | ✓ | ✓ |
| Indirect Draw | ✓ | ✓ |
| SSAO + Blur | ✓ | ✓ |
| Deferred Lighting | ✓ | ✓ |
| SSR Compute | ✓ | ✓ |
| Motion Blur | ✓ | ✓ |
| TAA | ✓ | ✓ |
| Bloom (3 passes) | ✓ | ✓ |
| Tonemap | ✓ | ✓ |

### Success Criteria

- [ ] All passes show correct non-zero GPU times
- [ ] Bar chart updates at 60Hz without flicker
- [ ] Total measured time ≈ actual frame time (±5%)

---

## Phase 23 — Hi-Z Occlusion Culling ✅ COMPLETE

**Priority:** ★★★ | **Effort:** Medium | **Dependencies:** Phase 21 (needs many meshes), Phase 22 (measure cull savings)

### What Was Implemented

1. **Hi-Z pyramid generation** (compute shader, both backends)
   - Blit depth buffer (D32_SFLOAT) → Hi-Z mip 0 (R32_SFLOAT) via copy/blit
   - Iterative min-downsample: `hiz_generate.comp.hlsl` (DX12) / `hiz_generate_vk.comp.glsl` (Vulkan)
   - 8×8 workgroups, each texel = min of 2×2 parent texels (conservative depth)
   - ~11 mip levels for 1080p (1920→960→…→1)
   - Per-mip descriptor sets (SRV source mip + UAV/storage dest mip)

2. **Occlusion cull in existing gpu_cull compute shader**
   - `HiZTestSphere()` added to `gpu_cull.comp.hlsl` / `gpu_cull_vk.comp.glsl`
   - Projects bounding sphere to screen-space AABB
   - Picks mip level where one texel covers the AABB footprint
   - Samples 4 AABB corners, takes min (closest occluder)
   - Rejects object if `ndc.z > occluderDepth` (fully behind geometry)
   - `enableHiZ` flag in CullConstants: 0=frustum-only, 1=frustum+Hi-Z

3. **Single-pass strategy with previous frame's depth**
   - `BuildHiZPyramid` called in `FlushDraws()` before cull dispatch (uses prev-frame depth)
   - Also called in `CompositeFrame()` after G-buffer pass ends (builds current-frame pyramid for next frame)
   - 1-frame depth lag acceptable — camera movement per frame is negligible
   - `_hizReady` flag gates activation (first frame = frustum-only until pyramid is populated)

4. **DX12 integration**
   - `CreateHiZResources()`: committed R32_FLOAT texture, per-mip UAV+SRV, `HiZGenerate` root signature + PSO
   - `BuildHiZPyramid()`: resource state barriers + Dispatch per mip level
   - Called at end of `FlushDraws()` after indirect draw

5. **Vulkan integration**
   - `CreateHiZResources()`: VkImage R32_SFLOAT with full mip chain, per-mip VkImageView, compute pipeline + descriptor pool
   - `BuildHiZPyramid(cmd)`: vkCmdBlitImage (depth→mip0) + vkCmdDispatch per mip with COMPUTE→COMPUTE barriers
   - Called in `FlushDraws()` pre-cull AND `CompositeFrame()` post-draw
   - Depth layout managed: READ_ONLY→ATTACHMENT→TRANSFER_SRC→ATTACHMENT→READ_ONLY (restore barrier for render graph)

6. **Bug fix: depth layout mismatch (Bug #009)**
   - `BuildHiZPyramid` leaves depth in `ATTACHMENT_OPTIMAL`, but render graph expects `READ_ONLY_OPTIMAL`
   - Added restore barrier in `CompositeFrame()` after Hi-Z build
   - See `Docs/bug-fix/009_hiz_depth_layout_mismatch.md`

### Expected Results (Sponza)

- Frustum cull: ~30-50% rejection (back half of scene)
- Hi-Z occlusion: additional ~20-40% rejection (walls occluding rooms behind)
- Net: 50-70% of draw calls eliminated

### Success Criteria

- [x] Hi-Z pyramid generated correctly (visual debug mip view)
- [x] Cull rate displayed in GPU profiler overlay
- [x] No visual artifacts from 1-frame depth lag
- [x] Both DX12 and Vulkan backends

---

## Phase 20 — Vulkan Async Compute ✅ COMPLETE

**Priority:** ★★☆ | **Effort:** Medium | **Dependencies:** None (but benefits from Phase 21 scene complexity)

### What Was Implemented

1. **Compute queue discovery** (`VulkanDevice`)
   - `FindQueueFamilies()` scans for a dedicated compute family (`VK_QUEUE_COMPUTE_BIT` without `VK_QUEUE_GRAPHICS_BIT`)
   - Fallback: second queue from graphics family (if `queueCount > 1`)
   - Final fallback: stay on graphics queue (`_asyncComputeSupported = false`)
   - `CreateLogicalDevice()` requests the extra queue (separate family or `queueCount=2` for same family)

2. **Per-frame compute resources** (`VulkanBackend`)
   - `VkComputeFrameResource`: per-frame `VkCommandPool`, `VkCommandBuffer`, `VkSemaphore` (compute done → graphics wait), `VkFence` (CPU wait for compute completion)
   - Fences created signaled for clean first-frame behavior
   - `CreateAsyncComputeResources()` / `DestroyAsyncComputeResources()` lifecycle

3. **`DispatchCullAsync()` — compute queue dispatch**
   - Waits for previous frame's compute fence, resets command buffer
   - Records: `vkCmdFillBuffer` (clear draw count) → transfer barrier → bind cull pipeline → push constants → `vkCmdDispatch`
   - Queue ownership release barriers (`srcQueueFamilyIndex = computeFamily, dstQueueFamilyIndex = graphicsFamily`) for `_indirectArgBuffer` and `_drawCountBuffer`
   - Submits to `_device->GetComputeQueue()`, signals semaphore + fence

4. **`FlushDraws()` restructured**
   - `if (_asyncComputeReady)`: calls `DispatchCullAsync()`, then records queue ownership acquire barriers on graphics command buffer
   - `else`: existing single-queue cull dispatch (Phase 15B path) unchanged
   - Hi-Z pyramid build stays on graphics queue (reads depth, avoids image ownership transfer complexity)

5. **`EndFrame()` semaphore chaining**
   - `VkSubmitInfo` now waits on **two** semaphores when async compute was active: `imageReady` (COLOR_ATTACHMENT_OUTPUT) + `computeDoneSemaphore` (DRAW_INDIRECT)
   - `_computeSubmittedThisFrame` flag tracks whether compute was dispatched this frame

6. **Same-family handling**
   - When `computeFamily == graphicsFamily` (e.g., integrated GPUs): queue ownership transfer barriers use `VK_QUEUE_FAMILY_IGNORED` (no transfer needed, semaphore handles execution ordering)
   - Different families: full queue ownership release/acquire barrier pair

### Success Criteria

- [x] GPU profiler shows compute and graphics work overlapping (when dedicated compute family available)
- [x] No validation errors for cross-queue sync
- [x] Graceful fallback to single-queue if async not available

---

## Phase 24 — Clustered Lighting ✅ COMPLETE

**Priority:** ★★☆ | **Effort:** High | **Dependencies:** Phase 21 (scene), Phase 22 (profiler)

### What Was Implemented

1. **Cluster grid** — 16×9×24 view-frustum clusters with logarithmic depth slicing (near=0.1, far=100). Total 3,456 clusters, max 128 lights per cluster, max 1,024 point lights.

2. **Cluster assignment compute shader** (`cluster_assign_vk.comp.glsl`, `cluster_assign.comp.hlsl`) — Dispatch (16, 9, 24), one thread per cluster. Reconstructs cluster AABB in view space from screen tile UV bounds + log-depth slice. Sphere-AABB intersection test per light. Writes matching light indices to `clusterLightIndex[]` SSBO, stores count in `clusterLightCount[]`.

3. **GPU data structures** — `GPUPointLight` (32B: position, radius, color, intensity in view space), `ClusterParams` UBO (invProj, near/far, screen dims, numLights), light SSBO (host-visible, 32 KB), cluster counts SSBO (device-local, ~14 KB), cluster indices SSBO (device-local, ~1.7 MB).

4. **Deferred lighting update** — `deferred_lighting_ibl_vk.frag.glsl` and `deferred_lighting_ibl.frag.hlsl` extended with cluster data bindings. Per-pixel cluster index derived from screen UV + view-space Z via log-depth lookup. Cook-Torrance BRDF (D_GGX + G_Smith + F_Schlick) per point light with smooth radius falloff attenuation.

5. **Pipeline layout expansion** — Deferred pipeline layout expanded to include cluster data. Vulkan: set=2 descriptor set. DX12: ClusterAssign root signature + additional SRV bindings.

6. **Render graph integration** — Cluster Assign compute pass inserted before Deferred Lighting pass. Includes `vkCmdFillBuffer`/ClearUAV clear → compute dispatch → COMPUTE_WRITE→FRAGMENT_READ buffer barriers.

7. **ImGui Point Light editor** — "Point Lights" panel with Add/Remove/Clear controls. Per-light: DragFloat3 position, DragFloat radius + intensity, ColorEdit3 color. New lights spawn at camera position. Calls `IRenderBackend::SetPointLights()` every frame.

8. **API extension** — `IRenderBackend::PointLightDesc` struct and `SetPointLights()` virtual method for backend-agnostic light upload.

### Success Criteria

- [x] Cluster compute shader compiles and dispatches without validation errors
- [x] Deferred lighting shader compiles with cluster bindings
- [x] Zero validation errors during runtime
- [x] ImGui light editor functional
- [x] DX12 backend parity
- [ ] 256+ point lights at ≥30 FPS (1080p, Sponza)

---

## Phase 25 — Mesh Shaders (DX12) ✅ COMPLETE

**Priority:** ★★☆ | **Effort:** Medium | **Dependencies:** Phase 21, Phase 23

### What Was Implemented

1. **Meshlet generation** (CPU, at load time in `BuildMergedGeometry`)
   - Greedy algorithm: walk triangles in order, fill meshlets up to 64 verts / 124 tris
   - Per-meshlet bounding sphere for AS frustum culling
   - Triangle indices packed as `uint8×3` into `uint32` — 25% index memory savings vs `3×uint32`
   - No external dependency (meshoptimizer not required)

2. **Amplification shader** (`meshlet_cull.as.hlsl`, SM 6.5)
   - `[numthreads(32,1,1)]` — one thread per meshlet
   - Transform meshlet bounding sphere to world space → 6-plane frustum test
   - Wave intrinsics (`WavePrefixCountBits`, `WaveActiveCountBits`) compact visible meshlets
   - `DispatchMesh(visibleCount, 1, 1)` — only visible meshlets proceed to MS

3. **Mesh shader** (`gbuffer_mesh.ms.hlsl`, SM 6.5)
   - `[numthreads(128,1,1)]`, `[outputtopology("triangle")]`
   - Reads `StructuredBuffer<PBRVertex>` + meshlet vertex/triangle indices
   - Transforms identical to `pbr_indirect.vert.hlsl`; outputs match `gbuffer.frag.hlsl` PSInput

4. **Pipeline infrastructure**
   - `MeshShaderGBuffer` root signature (10 params: b0-b2, t0-t5 space0, t0+ space1, s0)
   - PSO via `D3D12_PIPELINE_STATE_STREAM_DESC` + `ID3D12Device2::CreatePipelineState`
   - `DispatchMesh` via `ID3D12GraphicsCommandList6`

5. **Feature detection + graceful fallback**
   - `D3D12_FEATURE_DATA_D3D12_OPTIONS7::MeshShaderTier >= TIER_1`
   - If unsupported → Phase 12 `ExecuteIndirect` path unchanged

### Success Criteria

- [x] Meshlet-based rendering compiles and links
- [x] Amplification shader per-meshlet frustum cull
- [ ] Pixel-perfect match with indirect draw path (visual verification pending)
- [ ] Amplification shader cull rate visible in GPU profiler
- [ ] ≥ 10% frame time improvement on meshlet path vs indirect (with Sponza)

---

## Phase 26 — Vulkan Render Graph: Transient Resource Aliasing ✅ COMPLETE

**Priority:** ★☆☆ | **Effort:** Medium | **Dependencies:** Phase 18C (Vulkan render graph wired)

### What Was Implemented

1. **`VulkanRenderGraph` extended with transient image API** — `CreateTransientImage(name, VkImageCreateInfo, initialLayout)` declares a graph-owned image. `GetTransientImage(handle)` retrieves the VkImage after Compile(). Constructor now accepts `VkDevice + VkPhysicalDevice` (default ctor preserved for backward compatibility).

2. **DAG cull refactored** — `_CullPasses()` now uses the `_live` flag directly on PassBuilder (no separate vector), matching DX12 RenderGraph::_CullPasses() algorithm. Backward flood-fill from side-effect passes marks producers as live.

3. **Transient lifetime analysis** — `_ComputeTransientLifetimes()` computes `firstPass`/`lastPass` for each transient image, considering only live passes.

4. **Alias slot assignment** — `_AssignAliasingSlots()` uses greedy interval-graph colouring identical to DX12 Phase 14. Sorts transients by `firstPass`, reuses slots whose `lastPass < firstPass`. Memory type compatibility checked via `memTypeBits` intersection. Temporary VkImages created to query `vkGetImageMemoryRequirements`.

5. **Memory allocation + image creation** — `_CreateTransientResources()` allocates one `VkDeviceMemory` per alias slot (`VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT`), creates VkImages via `vkCreateImage`, binds at offset 0 via `vkBindImageMemory`. Memory savings logged at compile time.

6. **Aliasing barriers in Execute()** — Tracks per-slot current resident. When a different transient takes over a slot, emits `VkImageMemoryBarrier` with `oldLayout=UNDEFINED` to invalidate previous contents (Vulkan spec §12.4). `srcAccessMask = MEMORY_WRITE`, `dstAccessMask = MEMORY_READ|MEMORY_WRITE`.

7. **Cleanup** — `Shutdown()` destroys owned VkImages + frees VkDeviceMemory. Called by `Reset()` and destructor. `CompositeFrame()` updated to pass device info to graph constructor.

### Current State

All existing images remain imported (persistent). The aliasing infrastructure is ready to be activated when intermediate images (G-buffer, SSAO, bloom) are migrated to `CreateTransientImage()` declarations. No images are currently transient — the per-frame allocation cost is zero.

### Memory Savings (estimated, when wired)

| Slot | Image A (first) | Image B (second) | Heap Size | Saved |
|------|----------------|-------------------|-----------|-------|
| 0    | GBuf0 (8 MB)   | SSAO raw (2 MB)   | 8 MB      | 2 MB  |
| 1    | GBuf2 (8 MB)   | Bloom bright (4 MB)| 8 MB      | 4 MB  |

### Success Criteria

- [x] Transient image API compiles and links (build verified)
- [x] DAG cull refactored with _live flag on PassBuilder
- [x] Interval-graph colouring matches DX12 Phase 14 algorithm
- [x] Aliasing barrier emission in Execute() with oldLayout=UNDEFINED
- [ ] Transient images correctly aliased when wired (no visual corruption)
- [ ] Memory savings reported at init time via LUNA_LOG_INFO
- [ ] Validation layer clean (no memory aliasing warnings)

---

## Phase 27 — Vulkan Mesh Shaders (VK_EXT_mesh_shader) ✅ COMPLETE

**Priority:** ★★★ | **Effort:** Medium | **Dependencies:** Phase 25 (DX12 mesh shaders)

### What Was Implemented

1. **VK_EXT_mesh_shader extension probe + enable** — `VulkanDevice` probes `VkPhysicalDeviceMeshShaderFeaturesEXT` for `taskShader` + `meshShader` support. Extension conditionally added to device extension list. Graceful fallback to indirect draw when unavailable.

2. **Meshlet buffer generation** — `BuildMergedGeometry()` calls `BuildMeshlets()` (reusing Phase 25 `Meshlet.h/cpp`) per mesh, accumulates global meshlet/bounds/vertex/triangle arrays, uploads to device-local SSBO buffers via staging.

3. **GLSL task shader** (`meshlet_cull_vk.task.glsl`) — 32-thread workgroup, per-meshlet frustum cull via 6-plane test, shared-memory atomic compaction, `EmitMeshTasksEXT()`.

4. **GLSL mesh shader** (`gbuffer_mesh_vk.mesh.glsl`) — 128-thread workgroup, max 64 verts / 124 tris, SSBO vertex fetch from `PBRVertex[]`, outputs match `gbuffer_indirect_vk.frag.glsl` inputs exactly.

5. **Pipeline creation** — `CreateMeshShaderResources()`: descriptor set (6 SSBOs), pipeline layout (2 sets + 240B push constants), compile task+mesh+frag, `VkGraphicsPipeline` with null vertex input/input assembly states.

6. **FlushDraws() mesh shader path** — per-object push constants + `vkCmdDrawMeshTasksEXT(ceil(meshletCount/32), 1, 1)`. Falls back to existing indirect draw path when mesh shaders unavailable.

7. **Fragment shader reuse** — Reuses `gbuffer_indirect_vk.frag.glsl` unchanged (same bindless material bindings at set=1, same per-vertex input layout).

### Success Criteria

- [x] Build verified — zero compile/link errors
- [x] Meshlet data generated and uploaded for all scene meshes
- [x] Task + mesh + fragment pipeline created
- [x] Graceful fallback to indirect draw when VK_EXT_mesh_shader unavailable
- [ ] Visual parity with indirect draw path (runtime verification pending)
- [ ] GPU profiler shows mesh shader pass timing

---

## Phase 28 — Atmosphere / Sky Rendering (Hillaire 2020)

**Priority:** ★★★ | **Effort:** Medium | **Dependencies:** Phase 15C (IBL), Phase 10 (HDR pipeline)

Hillaire's "A Scalable and Production Ready Sky and Atmosphere Rendering Technique" (EGSR 2020). Dynamic physically-based sky with time-of-day. Highest visual-impact-per-effort phase — transforms portfolio screenshots immediately. Directly relevant to automotive sensor simulation (varying atmospheric conditions).

### Key Technical Elements
- Transmittance LUT (256×64, compute, Rayleigh + Mie + ozone absorption)
- Multi-scattering LUT (32×32, compute)
- Sky-view LUT (192×108, per-frame compute)
- Aerial perspective LUT (32×32×32 3D texture, volumetric in-scattering)
- Sun disk rendering + limb darkening
- Both backends; feeds into IBL as dynamic environment source

---

## Phase 29 — Volumetric Lighting / Fog

**Priority:** ★★☆ | **Effort:** Medium | **Dependencies:** Phase 28 (atmosphere), Phase 8 (CSM), Phase 24 (clustered lights)

Froxel-based volumetric fog (à la Frostbite) with ray-marched light scattering. Adds cinematic depth and is essential for sensor simulation (fog/rain conditions affect LiDAR/camera). Reuses clustered lighting grid for point light volumetric contribution.

### Key Technical Elements
- Froxel grid (160×90×64, exponential depth, RGBA16F 3D texture)
- Material injection compute pass (density + albedo from noise/height-based fog)
- Scattering compute pass (ray-march, CSM shadow lookup + cluster data reuse)
- Temporal reprojection (per-froxel jitter + history blend)
- Both backends; async compute candidate

---

## Phase 30 — Global Illumination (Screen-Space + Probe Hybrid)

**Priority:** ★★☆ | **Effort:** High | **Dependencies:** Phase 18D (VK RT), Phase 4C (DXR), Phase 23 (Hi-Z)

Two-tier GI: screen-space radiance cascades for near-field bounce light plus sparse irradiance probes (DDGI-lite) for off-screen/far-field. Central to every AAA title shipping today.

### Key Technical Elements
- SSGI: Hi-Z traced short rays in screen space, importance-sampled from GGX lobe, half-res
- Irradiance probes: 8×4×8 world-space grid, octahedral irradiance + depth maps
- Probe update: 1 probe/frame via RT RayQuery compute shader
- Temporal accumulation + hysteresis
- Fallback: SSAO-only ambient (current behaviour)

---

## Phase 31 — Order-Independent Transparency (OIT)

**Priority:** ★★☆ | **Effort:** Medium | **Dependencies:** Phase 24 (clustered lighting), Phase 7 (deferred pipeline)

Weighted blended OIT (McGuire & Bavoil 2013) with per-pixel linked list fallback. Solves the transparency gap in the deferred pipeline. Relevant to automotive (windshields, indicators) and game rendering (particles, glass).

### Key Technical Elements
- Weighted blended OIT: accumulation RT (RGBA16F) + revealage RT (R8)
- Per-pixel linked list mode: UAV counter + node pool SSBO, 8-deep fragment sort
- Forward-shaded with clustered lights (reuse Phase 24)
- Both backends; PPLL requires 64-bit atomics

---

## Phase 32 — Visibility Buffer Rendering

**Priority:** ★★☆ | **Effort:** High | **Dependencies:** Phase 25/27 (mesh shaders), Phase 12 (GPU-driven)

Replace G-buffer MRT with a thin visibility buffer (triangle ID + instance ID). State-of-the-art approach (Nanite/UE5). Demonstrates understanding that bandwidth — not ALU — is the bottleneck.

### Key Technical Elements
- Visibility buffer (R32G32_UINT): barycentrics + triangle ID + instance ID per-pixel
- Material classify compute pass: groups pixels by material for coherent texture fetches
- Deferred material evaluation compute: reconstructs attributes from vertex buffers using barycentrics
- Integrates with mesh shader path
- Fallback: existing G-buffer pipeline

---

## Phase 33 — Variable Rate Shading (VRS)

**Priority:** ★☆☆ | **Effort:** Low | **Dependencies:** Phase 10 (motion vectors), Phase 22 (profiler)

Tier 2 VRS with per-tile shading rate image driven by motion vectors + luminance variance. Low effort, high interview talking-point value — shows hardware feature awareness (Turing+, RDNA2+). Applicable to VR/automotive foveated rendering.

### Key Technical Elements
- DX12: `RSSetShadingRateImage` + `D3D12_SHADING_RATE_COMBINER`
- Vulkan: `VK_KHR_fragment_shading_rate` + `vkCmdSetFragmentShadingRateKHR`
- Shading rate compute pass: motion vectors (high → 2×2/4×4) + edge detection (edges → 1×1)
- Feature tier detection + graceful fallback
- GPU profiler extended with shading rate heatmap overlay

---

## Summary Timeline

```
Rendering Track:
  Week 1:  Phase 21 — Sponza/Bistro scene loading          ✅
  Week 2:  Phase 22 — GPU profiler overlay                  ✅
  Week 3:  Phase 23 — Hi-Z occlusion culling                ✅
  Week 4:  Phase 20 — Vulkan async compute                  ✅
  Week 5:  Phase 24 — Clustered lighting                    ✅
  Week 6:  Phase 25 — Mesh shaders                          ✅
           Phase 26 — VK transient aliasing                 ✅
  Next:    Phase 27 — VK mesh shaders                     ✅
           Phase 28 — Atmosphere / sky                     ✅
           Phase 29 — Volumetric fog
           Phase 30 — Global illumination
           Phase 31 — OIT
           Phase 32 — Visibility buffer
           Phase 33 — Variable rate shading

Simulation Track (deferred):
  S1:  Sensor simulation foundation (data + UI)             ✅
  S2:  Camera offscreen rendering
  S3:  LiDAR GPU raycasting
  S4:  Radar FFT
  S5:  Sensor data export
```

Each phase is self-contained with graceful fallback. The engine remains fully functional on both backends at every step.

