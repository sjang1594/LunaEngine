# LunaEngine

A cross-API real-time rendering engine written in C++20, targeting **DirectX 12** and **Vulkan 1.3**.

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

## License

MIT. Third-party dependencies retain their respective licenses (see `vendor/`).
