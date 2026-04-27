# LunaEngine

## Overview
LunaEngine — Cross-API Real-Time Renderer
C++20, DirectX 12 Ultimate + Vulkan 1.3 | github.com/sjang1594/luna-engine

Working:
- Hardware Abstraction Layer over DX12 Ultimate + Vulkan 1.3 with
  capability-flag feature exposure.
- Frame-graph compile step with automatic barrier insertion, layout
  transitions, and transient-resource aliasing across ~30 passes.
- Mesh-shader GPU geometry pipeline: meshlet generation
  (meshopt_buildMeshlets) → task/amplification two-phase culling
  (frustum + cone + Hi-Z) → DispatchMeshIndirect with GPU-side
  argument generation.
- Frames-in-flight ring buffer with per-frame command allocators,
  fence values, and persistently-mapped UPLOAD-heap constant
  buffers (2-3 frame CPU/GPU overlap).
- Cook-Torrance PBR with Image-Based Lighting (split-sum
  approximation: irradiance convolution + prefiltered environment
  maps on compute queue).
- 4-cascade Cascaded Shadow Maps with PSSM split selection,
  slope-scaled depth bias, and 3x3 PCF.
- Hillaire 2020 sky-atmosphere (transmittance, multi-scattering,
  sky-view, aerial perspective LUTs — all compute).
- HDR post stack: TAA (history reprojection + neighborhood AABB
  clamp), SSAO with bilateral blur, hierarchical SSR, bloom, ACES
  tonemapping with auto-exposure.
- Embedded GPU profiler with D3D12 timestamp queries and
  VK_KHR_performance_query, surfaced via ImGui dockspace overlay.

In development:
- DXR Tier 1.1 / VK_KHR_ray_tracing pipeline (BLAS/TLAS/SBT
  lifecycle implemented; bring-up in progress).
- Slang shader integration alongside HLSL (DXC) and GLSL (glslang)
  via custom IDxcBlob adapter, building toward auto-diff support
  for differentiable rendering experiments.

## Features

- **Backends:** DirectX 12 Ultimate, Vulkan 1.3
- **Shaders:** HLSL (DXC), GLSL (glslang), Slang
- **Rendering:** Deferred PBR, IBL, clustered forward+ lighting
- **Shadows:** Cascaded Shadow Maps, DXR / VK_KHR ray-traced shadows
- **Geometry:** Mesh shaders + GPU-driven culling (frustum + Hi-Z)
- **Post FX:** TAA, SSAO, SSR, Bloom, ACES tonemapping
- **Atmosphere:** Hillaire 2020 sky model
- **Architecture:** Render graph with automatic barrier tracking
- **Assets:** glTF 2.0, KTX2 / DDS

## Build

### Requirements
- Visual Studio 2022 (C++20)
- Windows SDK 10.0.22621+
- Vulkan SDK 1.4 (optional — enables Vulkan backend)
- GPU with mesh shader + DXR support

### Steps
```bat
scripts\generateProject.bat
msbuild LunaApp.sln /p:Configuration=Debug /p:Platform=x64 /m
```

The Vulkan backend is built only if `VULKAN_SDK` is set; otherwise the engine builds DX12-only.

### Checkpoints w RenderDoc

## License

MIT. Third-party dependencies retain their respective licenses (see `vendor/`).
