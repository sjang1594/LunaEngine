# LunaEngine

Cross-API real-time renderer — **DirectX 12** + **Vulkan 1.3**, C++17.

<https://github.com/sjang1594/LunaEngine>

---

## Why this exists

I spent several years building physically based sensor simulation — FMCW radar and LiDAR
models — *inside* someone else's engine. That works until you need to change how rays are
traced, how the depth pipeline is structured, or how a frame is scheduled. Then you're stuck.

LunaEngine is the renderer I needed underneath that work. The long-term target is a **sensor
simulation engine**: camera, LiDAR and radar models that share one GPU-driven scene
representation, with a path toward differentiable rendering. The graphics feature set below is
the foundation, not the goal.

---

## Layout

```
LunaEngine/src/LunaEngine/
├── Application/     window + main loop (GLFW)
├── Components/      Transform, MeshRenderer, CameraComponent, GameObject
├── Graphics/        IPipeline / IBuffer / Texture / Material abstractions
├── Loader/          glTF (cgltf), nuScenes dataset loader
├── Profiler/        GPU timestamp profiler + ImGui overlay
├── Renderer/
│   ├── HAL/         IRenderBackend / IRenderContext / IRenderDevice
│   ├── RenderGraph  DX12 frame graph (DAG cull, barriers, aliasing)
│   ├── Meshlet.cpp  meshlet builder (own implementation)
│   ├── DX12/        Public/ + Private/ — backend, device, pipeline, DXR
│   └── Vulkan/      Public/ + Private/ — backend, device, subsystems
├── Sensor/          ISensor, CameraSensor, LiDARSensor, RadarSensor
├── Scene/           scene graph
└── Shaders/         HLSL (DXC) + GLSL (glslang), ~120 files
```

Both backends implement the same HAL. The render graph exists per-backend
(`RenderGraph` for DX12, `VulkanRenderGraph` for Vulkan) rather than as one API-neutral
abstraction — see [Design notes](#design-notes).

---

## Implemented

### Frame graph
- Pass declaration with `Read`/`Write` resource states, DAG reference-count culling of
  passes whose outputs are never consumed.
- **Automatic barrier scheduling** — per-pass pre-barriers plus end-of-frame restore.
- **Transient-resource aliasing** — per-resource lifetime intervals, greedy interval-graph
  colouring, one heap per alias slot, placed resources, aliasing barriers on slot reuse.
  Implemented on both backends. *See [Known limitations](#known-limitations).*

### Geometry
- **GPU-driven indirect rendering** — per-object frustum + Hi-Z occlusion culling in a
  compute pass, visible objects appended to an indirect argument buffer via an atomic counter,
  drawn with `ExecuteIndirect` / `vkCmdDrawIndexedIndirect`.
- **Mesh shader pipeline** (SM 6.5) — own meshlet builder (`Renderer/Meshlet.cpp`),
  per-meshlet frustum culling in an amplification shader with wave-intrinsic compaction,
  then `DispatchMesh`.
- **Hi-Z pyramid** — depth pyramid built in compute; screen-space AABB projection selects
  the mip where one texel covers the bounds, then a four-corner min test.
- **Visibility buffer** path — visibility pass + deferred shading compute.

### Shading
- Deferred **Cook-Torrance PBR**, clustered light assignment.
- **IBL** — split-sum: irradiance convolution, prefiltered environment map, BRDF LUT.
- **Cascaded shadow maps** with slope-scaled depth bias and PCF.
- **Ray-traced shadows** — DXR on DX12, `VK_KHR_ray_tracing` on Vulkan.
- **Global illumination** — SSGI plus a light-probe update pass.
- **Order-independent transparency** — forward accumulation + composite.

### Atmosphere & post
- **Hillaire 2020 sky-atmosphere** — transmittance, multi-scattering, sky-view and aerial
  perspective LUTs, all compute.
- **Volumetric fog**.
- HDR post stack: **TAA** (history reprojection + neighbourhood clamp, Halton jitter),
  **SSAO** with bilateral blur, **SSR**, **bloom**, ACES tonemapping.

### Sensor simulation (in progress)
- `Sensor/` — `ISensor` interface with camera, LiDAR and radar implementations.
- **LiDAR raycast** compute shader.
- **Camera lens distortion** compute shader.
- Sensor-specific lighting pass and **point-cloud rendering**.
- **nuScenes** dataset loader for driving real trajectories through the renderer.

### Tooling
- GPU profiler using **D3D12 timestamp queries** / `vkCmdWriteTimestamp`, per-pass timings
  surfaced in an ImGui overlay.
- Transform and view gizmos, scene inspector.

---

## Known limitations

Stated deliberately — these are the parts I would not claim as finished.

| Area | Status |
|---|---|
| **Transient aliasing** | Fully implemented on both backends, but **no pass uses it yet** — every render target is still imported as persistent, so it currently saves no memory. Migrating the G-buffer and post chain is the next step. |
| **Async compute** | Implemented on both backends (dedicated queue, per-frame pools/fences, queue-family ownership transfer as a release/acquire pair) but **disabled**. On DX12 the sync used a queue-level `Wait()`, which blocks the entire next `ExecuteCommandLists` — producing *zero* GPU overlap while still introducing cross-queue races on the Hi-Z texture and shared object buffer. The single-queue path is race-free with identical GPU behaviour, so it is forced. The fix is to submit the cull as its own command list ahead of the frame's list. See [`Docs/bug-fix/010`](Docs/bug-fix/010_dx12_hiz_async_compute_race.md). |
| **Occlusion culling latency** | The Hi-Z pyramid is built from the **previous frame's** depth, so there is a one-frame lag and false positives under fast camera motion. Two-phase culling (draw last frame's visible set → rebuild HZB → re-test the rejected set) is not implemented. Meshlet-level culling is frustum-only for the same reason — it would inherit the same stale depth. |
| **Render graph** | Two parallel implementations, not one API-neutral graph. |
| **Barriers** | No split barriers. |
| **Descriptors** | Descriptor *indexing* rather than full bindless — fixed-size arrays sized at build time with `PARTIALLY_BOUND`; no `VARIABLE_DESCRIPTOR_COUNT` or `UPDATE_AFTER_BIND`. |
| **Slang** | Integrated for shader compilation; auto-diff / differentiable rendering is exploratory, not working. |

---

## Engineering log

The interesting part of a dual-backend renderer is not the feature list — it is that nothing
errors when the two backends silently disagree.

- [`Docs/bug-fix/`](Docs/bug-fix/) — 15 written root-cause investigations. Representative:
  - [`005`](Docs/bug-fix/005_vulkan_dx12_rendering_parity.md) — DX12 and Vulkan rendered the
    same asset completely differently. The DX12 IBL shader declared the irradiance, prefiltered
    and BRDF textures, and C++ bound them correctly to descriptor tables — but the shader body
    never sampled them; ambient was a hardcoded constant. Compounded by a legacy Vulkan
    G-buffer writing `albedo.a = 1.0` while the lighting shader unpacked emissive from alpha,
    adding red emissive to every pixel.
  - [`002`–`004`](Docs/bug-fix/) — three sessions tracking diagonal stripe artifacts down to a
    missing Halton jitter in the Vulkan TAA path.
  - [`010`](Docs/bug-fix/010_dx12_hiz_async_compute_race.md) — Hi-Z race and the async compute
    decision above.
- [`Docs/progress/`](Docs/progress/) — per-subsystem implementation notes.

---

## Design notes

**Why two render graphs instead of one abstraction.** The DX12 and Vulkan barrier models differ
enough — resource states versus image layouts plus pipeline stage/access masks — that a
lowest-common-denominator API would have leaked both models anyway. At the current pass count
the duplication is cheaper than the abstraction. This is worth revisiting as the graph grows.

**Why an own meshlet builder.** `Renderer/Meshlet.cpp` builds meshlets directly rather than
pulling in meshoptimizer, to keep the vertex/triangle packing under my control while the
mesh-shader path was being brought up.

---

## Build

**Requirements**
- Visual Studio 2022, C++17
- Windows SDK 10.0.22621+
- Vulkan SDK (optional — enables the Vulkan backend)
- GPU with mesh shader and DXR support

```bat
scripts\generateProject.bat
msbuild LunaApp.sln /p:Configuration=Debug /p:Platform=x64 /m
```

The Vulkan backend builds only when `VULKAN_SDK` is set; otherwise the engine builds DX12-only.

**Third-party** (`vendor/`): cgltf, D3D12MemoryAllocator, DirectXTex, DirectX-Headers, DXC,
GLFW, GLM, Dear ImGui, nlohmann/json, stb_image.

---

## License

MIT. Third-party dependencies retain their respective licenses — see `vendor/`.
