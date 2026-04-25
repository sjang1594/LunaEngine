# LunaEngine — Next Phases Plan

**Last updated:** 2026-04-25
**Current state:** 24 phases complete (DX12 + Vulkan dual-backend, deferred PBR, DXR/VK RT shadows, GPU-driven indirect, render graph, full PP stack, IBL, multi-mesh scene support, GPU profiler, Hi-Z occlusion culling, Vulkan async compute)

---

## Execution Order

```
Phase 21 ✅ → 22 ✅ → 23 ✅ → 20 ✅ → 24 → 25 → 26
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

## Phase 24 — Clustered/Tiled Forward Lighting

**Priority:** ★★☆ | **Effort:** High | **Dependencies:** Phase 21 (scene), Phase 22 (profiler)

### Goal

Support hundreds of point/spot lights via a clustered lighting scheme. Currently only a single directional light exists.

### Implementation Plan

1. **Light data structure**
   - `GPUPointLight { float3 position; float radius; float3 color; float intensity; }`
   - `GPUSpotLight { ... + float3 direction; float innerCone; float outerCone; }`
   - SSBO uploaded per frame (up to 1024 lights)

2. **Cluster assignment (compute shader)**
   - Divide screen into 16×9×24 clusters (X × Y × depth slices, logarithmic depth)
   - Each cluster: list of light indices that intersect its frustum volume
   - Output: `clusterLightIndices[]` + `clusterLightCounts[]` SSBOs

3. **Deferred lighting update**
   - `deferred_lighting_ibl.frag.hlsl` iterates over cluster's light list
   - Same Cook-Torrance BRDF, but summed over N lights per pixel
   - Directional light remains separate (not clustered)

4. **Scene setup**
   - Add light placement to Sponza (e.g., torch positions)
   - ImGui light editor: add/remove/move lights, adjust colour/intensity

### Success Criteria

- [ ] 256+ point lights at ≥30 FPS (1080p, Sponza)
- [ ] No per-light draw calls — single deferred pass reads cluster data
- [ ] Both DX12 and Vulkan backends

---

## Phase 25 — Mesh Shaders (DX12)

**Priority:** ★★☆ | **Effort:** Medium | **Dependencies:** Phase 21, Phase 23

### Goal

Replace the traditional vertex/index pipeline with DX12 mesh shaders (SM 6.5). Amplification shader performs meshlet-level frustum + occlusion culling; mesh shader outputs triangles directly.

### Implementation Plan

1. **Meshlet generation** (offline, at load time)
   - Split each mesh into meshlets (max 64 verts, 124 triangles per meshlet)
   - Use `meshoptimizer` library or manual greedy algorithm
   - Store `MeshletDesc { vertexOffset, vertexCount, triangleOffset, triangleCount, boundingSphere }`

2. **Amplification shader** (`meshlet_cull.as.hlsl`)
   - One thread group per meshlet batch (e.g., 32 meshlets)
   - Per-meshlet frustum + Hi-Z occlusion test
   - `DispatchMesh()` for visible meshlets only

3. **Mesh shader** (`meshlet_draw.ms.hlsl`)
   - Read meshlet vertex/index data from SSBO
   - Output up to 124 triangles per thread group
   - Write to G-buffer (same MRT layout as current gbuffer pipeline)

4. **Fallback**
   - Feature-detect `D3D12_FEATURE_D3D12_OPTIONS7::MeshShaderTier`
   - If not supported, fall back to Phase 12 indirect draw path

### Success Criteria

- [ ] Meshlet-based rendering matches pixel-perfect with indirect draw path
- [ ] Amplification shader cull rate visible in GPU profiler
- [ ] ≥ 10% frame time improvement on meshlet path vs indirect (with Sponza)

---

## Phase 26 — Vulkan Render Graph: Transient Resource Aliasing

**Priority:** ★☆☆ | **Effort:** Medium | **Dependencies:** Phase 18C (Vulkan render graph wired)

### Goal

Port DX12 Phase 14's placed-resource aliasing to Vulkan. Graph-owned transient images share the same `VkDeviceMemory` when their lifetimes don't overlap.

### Implementation Plan

1. **`CreateTransientImage()`** on `VulkanRenderGraph`
   - Declares a graph-owned image (format, extent, usage)
   - Not backed by memory yet

2. **Lifetime analysis** (in `Compile()`)
   - Compute `firstPass` / `lastPass` per transient image
   - Interval-graph colouring assigns alias slots (same as DX12 Phase 14)

3. **Memory allocation**
   - Per alias slot: `vkAllocateMemory` with size = max of all images in that slot
   - Each image: `vkBindImageMemory` to the slot's memory at offset 0

4. **Aliasing barriers**
   - Emit `VkImageMemoryBarrier` with `oldLayout=UNDEFINED` when switching to a new resident in the same slot
   - Ensures cache invalidation between aliased resources

### Memory Savings (estimated, 1080p)

| Slot | Image A (first) | Image B (second) | Heap Size | Saved |
|------|----------------|-------------------|-----------|-------|
| 0    | GBuf0 (8 MB)   | SSAO raw (2 MB)   | 8 MB      | 2 MB  |
| 1    | GBuf2 (8 MB)   | Bloom bright (4 MB)| 8 MB      | 4 MB  |

### Success Criteria

- [ ] Transient images correctly aliased (no visual corruption)
- [ ] Memory savings reported at init time via `LUNA_LOG_INFO`
- [ ] Validation layer clean (no memory aliasing warnings)

---

## Summary Timeline

```
Week 1:  Phase 21 — Sponza/Bistro scene loading
Week 2:  Phase 22 — GPU profiler overlay
Week 3:  Phase 23 — Hi-Z occlusion culling
Week 4:  Phase 20 — Vulkan async compute
Week 5+: Phase 24 — Clustered lighting
         Phase 25 — Mesh shaders
         Phase 26 — VK transient aliasing
```

Each phase is self-contained with graceful fallback. The engine remains fully functional on both backends at every step.

