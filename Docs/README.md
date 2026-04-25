# LunaEngine

**Dual-Backend Real-Time Rendering Engine**

DX12 + Vulkan | DXR Hybrid Shadows | GPU-Driven Rendering | PBR + IBL | TAA/SSAO/SSR/Bloom

---

## Overview

LunaEngine은 DirectX 12와 Vulkan 듀얼 백엔드를 지원하는 실시간 렌더링 엔진이다. DXR 기반 하이브리드 섀도우, GPU-Driven 렌더링, 풀 PBR 파이프라인을 구현하며, 23개 Phase에 걸친 점진적 개발 과정을 거쳤다.

```
Platform:    Windows 11
Language:    C++17
Build:       Premake5 → Visual Studio 2022
APIs:        DirectX 12, Vulkan 1.3, DXR 1.0
```

---

## Architecture

```
LunaEngine-source/
├── LunaApp/                    # Application Layer
│   └── src/LunaApp.cpp         # Entry point, ExampleLayer
│
├── LunaEngine/src/LunaEngine/
│   ├── Application/            # Window, GLFW callbacks, Run loop
│   ├── Components/             # ECS: Transform, MeshRenderer, GameObject
│   ├── Graphics/               # IPipeline, IBuffer, Texture
│   ├── Renderer/
│   │   ├── HAL/                # IRenderBackend, IRenderContext, IRenderDevice
│   │   ├── DX12/               # DX12Backend, Pipeline, Buffer, RT
│   │   ├── Vulkan/             # VulkanBackend, RenderGraph
│   │   ├── Camera.h            # Orbital camera
│   │   └── MeshLoader.h        # cgltf glTF/GLB loader
│   ├── Scene/                  # Scene graph, SceneManager
│   ├── UI/                     # TransformGizmo, ViewGizmo
│   ├── Profiler/               # GPU timestamp profiler
│   └── Shaders/                # HLSL/GLSL shaders
│
└── vendor/                     # External dependencies
```

---

## Features

### Rendering Pipeline

| Feature | Description |
|---------|-------------|
| **Deferred Shading** | G-Buffer (Albedo, Normal, MetalRough, Emissive, Depth) |
| **PBR / Cook-Torrance** | GGX NDF, Smith-Schlick G, Fresnel-Schlick |
| **IBL** | Split-sum approximation: Irradiance + Prefiltered Env + BRDF LUT |
| **Cascaded Shadow Maps** | 4-cascade CSM with PCF filtering |
| **DXR Hybrid Shadows** | Ray-traced shadow rays for soft shadows (DX12) |
| **SSAO** | Screen-space ambient occlusion with blur pass |
| **SSR** | Screen-space reflections via Hi-Z ray marching |
| **TAA** | Temporal anti-aliasing with YCoCg neighbourhood clamping |
| **Bloom** | Threshold + multi-pass Gaussian blur |
| **Motion Blur** | Velocity buffer based |
| **Tonemapping** | ACES Filmic (hue-preserving) |

### GPU-Driven Rendering

| Component | Description |
|-----------|-------------|
| **Compute Culling** | Frustum + Hi-Z occlusion culling in compute shader |
| **Indirect Draw** | `ExecuteIndirect` / `vkCmdDrawIndexedIndirect` |
| **Merged Geometry** | Single VB/IB for entire scene |
| **Object Buffer** | Per-instance data in structured buffer |

### Engine Features

| Feature | Description |
|---------|-------------|
| **Dual Backend** | DX12 / Vulkan parity for all features |
| **Render Graph** | DAG-based pass scheduling, resource aliasing (DX12) |
| **GPU Profiler** | Per-pass timestamp queries, overlay UI |
| **Transform Gizmo** | Isaac Sim-style universal gizmo (translate/rotate/scale) |
| **Custom Title Bar** | Windows borderless with draggable title area |
| **glTF Loader** | cgltf-based, PBR material support |

---

## Technical Highlights

### Frames-in-Flight

```cpp
static constexpr UINT FRAMES_IN_FLIGHT = 2;
struct FrameResource {
    ComPtr<ID3D12CommandAllocator> cmdAllocator;
    ComPtr<ID3D12Resource>         mvpCB;
    void*                          mvpCBMapped;
    UINT64                         fenceValue;
};
```

CPU-GPU overlap을 위한 링 버퍼 패턴. `BeginFrame()`에서 가장 오래된 프레임 대기, `EndFrame()`에서 fence signal.

### D3D12MA / VMA

DEFAULT heap으로 GPU 리소스 생성. UPLOAD heap staging → CopyBufferRegion → barrier 패턴.

```
Allocate DEFAULT buffer (D3D12MA)
Allocate UPLOAD staging
Map → memcpy → Unmap
CopyBufferRegion(default ← staging)
Barrier: COPY_DEST → target state
Execute → Fence wait
Release staging
```

### DXR Pipeline

```hlsl
[shader("raygeneration")] void RayGen() {
    float3 worldPos = ReconstructWorldPos(depth, invVP);
    RayDesc ray = { worldPos + N*0.001, -lightDir, 0.001, 1e4 };
    TraceRay(tlas, RAY_FLAG_ACCEPT_FIRST_HIT, 0xFF, 0,0,0, ray, payload);
    shadowOutput[idx] = payload.shadowed ? 0.0 : 1.0;
}
```

### Vulkan Render Graph

```cpp
auto hDepth  = rg.ImportImage(_depthImage, VK_IMAGE_LAYOUT_...);
auto hOutput = rg.CreateImage("output", ...);

rg.AddPass("SSAO")
    .Read(hDepth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, ...)
    .Write(hOutput, ...)
    .Execute([](VkCommandBuffer cmd) { ... });

rg.Compile();
rg.Execute(cmd);
```

---

## Shader Summary

| Shader | Stage | Purpose |
|--------|-------|---------|
| `pbr.vert/frag.hlsl` | Raster | Forward PBR (fallback) |
| `gbuffer.vert/frag.hlsl` | Raster | Deferred G-buffer fill |
| `deferred_lighting_ibl.frag.hlsl` | Fullscreen | Deferred lighting + IBL |
| `shadows.hlsl` | DXR lib_6_5 | RayGen/Miss/ClosestHit |
| `gpu_cull.comp.hlsl` | Compute | Frustum + Hi-Z culling |
| `hiz_generate.comp.hlsl` | Compute | Hi-Z pyramid generation |
| `ssao.frag.hlsl` | Fullscreen | SSAO sampling |
| `ssr.frag.hlsl` | Fullscreen | SSR ray march |
| `taa.frag.hlsl` | Fullscreen | Temporal resolve |
| `bloom_*.frag.hlsl` | Fullscreen | Bloom threshold + blur |
| `tonemapping.frag.hlsl` | Fullscreen | ACES + gamma |

---

## Build

### Prerequisites

| Library | Version | Location |
|---------|---------|----------|
| Vulkan SDK | 1.3+ | `$VULKAN_SDK` |
| DirectX Headers | latest | `vendor/dxheaders/` |
| DXC | 1.8+ | `vendor/dxc/` |
| D3D12MA | 2.x | `vendor/d3d12ma/` |
| GLFW | 3.4 | `vendor/glfw/` |
| ImGui | 1.91+ | `vendor/imgui/` |
| cgltf | 1.14 | `vendor/cgltf/` |
| stb_image | 2.29 | `vendor/stb_image/` |

### Commands

```powershell
# Generate VS solution
.\premake5.exe vs2022

# Build
MSBuild LunaApp.sln /p:Configuration=Release /p:Platform=x64 /m

# Run (DX12 default)
.\LunaApp\bin\Release-windows-x86_64\LunaApp\LunaApp.exe

# Run (Vulkan)
.\LunaApp\bin\Release-windows-x86_64\LunaApp\LunaApp.exe --vulkan
```

---

## Development Phases

| Phase | Description |
|-------|-------------|
| 1 | Device init, first triangle |
| 2 | Frames-in-flight, DXC SM 6.x, D3D12MA |
| 3 | Orbital camera, glTF loader, PBR pipeline |
| 4 | DXR BLAS/TLAS, shadow rays |
| 5 | Vulkan backend parity |
| 6 | Scene graph, MeshRenderer |
| 7-10 | CSM shadows, SSAO |
| 11-14 | Deferred shading, IBL precompute |
| 15-17 | SSR, bloom, TAA, motion blur |
| 18-20 | GPU-driven rendering (merged geometry, compute cull) |
| 21-23 | Hi-Z pyramid, occlusion culling, profiler |

---

## Bug Fix History

10개 주요 버그 수정 기록. 상세 내용은 [bug-fix/README.md](bug-fix/README.md) 참조.

| # | Issue | Status |
|---|-------|--------|
| 001 | TAA YCoCg 채널 스왑 | ✅ |
| 002-004 | Vulkan diagonal stripe | ✅ |
| 005 | DX12/Vulkan 렌더링 패리티 | ✅ |
| 006-007 | Vulkan device lost (resize/IBL) | ✅ |
| 008 | Custom title bar init order | ✅ |
| 009 | Hi-Z depth layout mismatch | ✅ |
| 010 | DX12 GPU-driven flickering | ⚠️ Hi-Z disabled |

---

## Known Issues & TODO

| Priority | Item |
|----------|------|
| HIGH | Hi-Z double-buffer로 occlusion culling 재활성화 |
| MEDIUM | Async compute separate command lists |
| MEDIUM | VulkanBackend god-object decomposition |
| LOW | VMA 적용 (Vulkan memory allocator) |
| LOW | Vulkan shutdown validation 에러 정리 |

---

## Evaluation Summary

> "The codebase is solid for a portfolio project. The core rendering pipeline is correct, the dual-backend parity is impressive, and the phased development approach is well-documented."

**Strengths:**
- Clean `IRenderBackend` abstraction
- DX12/Vulkan feature parity (RT, SSR, SSAO, TAA, Hi-Z 등)
- DAG-based render graph with resource aliasing
- GPU-driven rendering pipeline

**Areas for Improvement:**
- VulkanBackend ~800 lines → subsystem extraction 필요
- Raw Vulkan handles → RAII wrapper 또는 VMA
- Single-threaded → multi-threaded command recording

---

## License

Portfolio project. Not for production use.

