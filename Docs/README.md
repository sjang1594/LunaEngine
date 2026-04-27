# LunaEngine

**Dual-Backend Real-Time Rendering Engine**

GPU-Driven Rendering | DXR Ray Tracing | DX12 + Vulkan | PBR + IBL | Render Graph

---

## Key Technical Achievements

| Feature | Complexity | Description |
|---------|------------|-------------|
| **GPU-Driven Rendering** | ★★★ | Compute frustum/Hi-Z cull → ExecuteIndirect, single draw call |
| **DXR Hybrid Shadows** | ★★★ | BLAS/TLAS, RTPSO, shader table, TraceRay integration |
| **Dual Backend Parity** | ★★★ | DX12 + Vulkan feature-identical implementation |
| **Render Graph** | ★★☆ | DAG barrier scheduling, transient resource aliasing |
| **Hi-Z Occlusion Culling** | ★★☆ | Hierarchical-Z pyramid, GPU sphere-AABB test |
| **Full IBL Pipeline** | ★★☆ | Equirect→Cube→Irradiance→Prefilter→BRDF LUT |
| **Async Compute** | ★★☆ | Cross-queue fence sync, overlapped cull dispatch |

---

## Overview

LunaEngine은 DirectX 12와 Vulkan 듀얼 백엔드를 지원하는 실시간 렌더링 엔진이다. 23개 Phase에 걸친 점진적 개발을 통해 GPU-Driven 렌더링, DXR 레이트레이싱, 풀 PBR 파이프라인을 구현했다.

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

## Technical Deep Dives

### 1. GPU-Driven Rendering Pipeline

```
CPU: Upload GPUObjectData[] (model, bounding sphere, mesh/material index)
     ↓
GPU: Compute Cull Dispatch
     ├─ Frustum Test (6-plane sphere)
     ├─ Hi-Z Occlusion Test (project → AABB → sample pyramid)
     └─ Atomic append survivors → IndirectDrawCommand[]
     ↓
GPU: ExecuteIndirect / vkCmdDrawIndexedIndirectCount
     └─ Single draw call for entire scene
```

**핵심**: CPU는 인스턴스 데이터만 업로드. 모든 컬링과 드로우 결정은 GPU에서 수행.

### 2. DXR Ray Tracing Integration

```hlsl
[shader("raygeneration")] void RayGen() {
    float3 worldPos = ReconstructWorldPos(depth, invVP);
    RayDesc ray = { worldPos + N*0.001, -lightDir, 0.001, 1e4 };
    TraceRay(tlas, RAY_FLAG_ACCEPT_FIRST_HIT, 0xFF, 0,0,0, ray, payload);
    shadowOutput[idx] = payload.shadowed ? 0.0 : 1.0;
}
```

**구성**: Per-mesh BLAS → Scene TLAS → RTPSO (lib_6_5) → Shader Table (RayGen/Miss/Hit)

### 3. Render Graph (DAG + Resource Aliasing)

```cpp
rg.AddPass("SSAO")
    .Read(hDepth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
    .Write(hSSAO, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    .SideEffect()
    .Execute([](ID3D12GraphicsCommandList* cmd) { ... });

rg.Compile();  // DAG cull + barrier scheduling + aliasing slot assignment
rg.Execute();  // Emit barriers + run passes
```

**기능**: Dead pass cull, automatic barrier insertion, transient resource memory reuse

### 4. Hi-Z Occlusion Culling

```hlsl
bool HiZTestSphere(float3 centre, float radius) {
    float4 clip = mul(float4(centre, 1), viewProj);
    float2 screenRadius = abs(radius * float2(P[0][0], P[1][1]) / clip.w);
    uint mip = ceil(log2(max(screenRadius * screenSize)));
    float occluder = min(HiZ.SampleLevel(uvMin, mip), ...);
    return ndc.z <= occluder;  // visible if in front
}
```

**파이프라인**: Depth → Mip 0 blit → Compute min-downsample → 11 mips (1080p)

---

## Rendering Features

| Category | Features |
|----------|----------|
| **Shading** | Deferred G-Buffer, Cook-Torrance PBR, IBL (split-sum) |
| **Shadows** | 4-cascade CSM (PCF), DXR ray-traced shadows |
| **Post-Process** | TAA (YCoCg clamp), SSAO, SSR (Hi-Z march), Bloom, Motion Blur |
| **Culling** | GPU Frustum + Hi-Z Occlusion |
| **Tonemapping** | ACES Filmic (hue-preserving) |

---

## Development Phases (Chronological)

### Foundation (Phase 1-5)
| Phase | Feature |
|-------|---------|
| 1 | DX12 device, depth buffer, MVP CB, first triangle |
| 2 | Frames-in-flight, DXC SM 6.x, D3D12MA DEFAULT heap |
| 3 | Orbital camera, cgltf loader, Cook-Torrance PBR |
| 4 | DXR BLAS/TLAS, RTPSO, hybrid shadow rays |
| 5 | Vulkan backend parity |

### Core Rendering (Phase 6-10)
| Phase | Feature |
|-------|---------|
| 6 | Render graph barrier scheduling |
| 7 | Deferred rendering (G-buffer + lighting pass) |
| 8 | Cascaded Shadow Maps (4-cascade, PCF) |
| 9 | SSAO (half-res, 16-tap, blur) |
| 10 | Post-process: HDR, TAA (Halton jitter), Bloom |

### Advanced GPU (Phase 11-14)
| Phase | Feature |
|-------|---------|
| 11 | Bindless textures (unbounded SRV array) |
| 12 | GPU-driven rendering (merged VB/IB, compute cull, ExecuteIndirect) |
| 13 | Async compute (cross-queue fence sync) |
| 14 | Render graph: DAG cull + transient resource aliasing |

### Vulkan Parity (Phase 15-18)
| Phase | Feature |
|-------|---------|
| 15 | Vulkan GPU-driven + IBL precompute |
| 16 | SSAO + SSR (both backends) |
| 17 | Vulkan PP stack (TAA, bloom, tonemap) |
| 18 | Motion blur, VK render graph, VK ray tracing |

### Polish (Phase 19-23)
| Phase | Feature |
|-------|---------|
| 19-21 | DX12/VK parity bug-fix, multi-mesh scene |
| 22 | GPU profiler overlay (timestamp queries) |
| 23 | Hi-Z occlusion culling |

---

## Shader Summary

| Shader | Stage | Purpose |
|--------|-------|---------|
| `gpu_cull.comp.hlsl` | Compute | Frustum + Hi-Z culling |
| `shadows.hlsl` | DXR lib_6_5 | RayGen/Miss/ClosestHit |
| `hiz_generate.comp.hlsl` | Compute | Hi-Z pyramid generation |
| `deferred_lighting_ibl.frag.hlsl` | Fullscreen | Deferred lighting + IBL |
| `gbuffer.vert/frag.hlsl` | Raster | Deferred G-buffer fill |
| `taa.frag.hlsl` | Fullscreen | Temporal resolve |
| `ssr.frag.hlsl` | Fullscreen | SSR ray march |
| `ssao.frag.hlsl` | Fullscreen | SSAO sampling |

---

## Build

### Prerequisites

| Library | Version | Location |
|---------|---------|----------|
| Vulkan SDK | 1.3+ | `$VULKAN_SDK` |
| DXC | 1.8+ | `vendor/dxc/` |
| D3D12MA | 2.x | `vendor/d3d12ma/` |
| GLFW | 3.4 | `vendor/glfw/` |
| ImGui | 1.91+ | `vendor/imgui/` |
| cgltf | 1.14 | `vendor/cgltf/` |

### Commands

```powershell
.\premake5.exe vs2022
MSBuild LunaApp.sln /p:Configuration=Release /p:Platform=x64 /m

# DX12
.\LunaApp\bin\Release-windows-x86_64\LunaApp\LunaApp.exe

# Vulkan
.\LunaApp\bin\Release-windows-x86_64\LunaApp\LunaApp.exe --vulkan
```

---

## Bug Fix Log

10개 주요 버그 수정. 상세: [bug-fix/README.md](bug-fix/README.md)

| # | Root Cause | Impact |
|---|------------|--------|
| 001 | TAA YCoCg Co/Cg swap | 색상 스크램블 |
| 002-004 | VK TAA jitter 미적용 | 대각선 줄무늬 |
| 005 | DX12 IBL 미샘플링 | 렌더링 불일치 |
| 006-008 | VK resize/init race | Device lost |
| 009 | Hi-Z depth layout | Device lost |
| 010 | Frustum plane NaN | DX12 flickering |

---

## TODO

| Priority | Item |
|----------|------|
| HIGH | Hi-Z double-buffer (occlusion culling 재활성화) |
| MEDIUM | VulkanBackend subsystem extraction |
| LOW | Clustered forward lighting |

---

## License

Portfolio project. Not for production use.
