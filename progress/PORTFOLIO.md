# LunaEngine — Portfolio Engineering Log

**Author:** Senior Graphics Engineer
**Platform:** Windows 11, DirectX 12 + Vulkan
**Language:** C++17
**Build:** Premake5 → Visual Studio 2022
**Completed:** 2026-04-11

---

## Overview

LunaEngine is a dual-backend (DX12 + Vulkan) real-time rendering engine built as a senior-level
portfolio project. It demonstrates fluency with modern GPU APIs, driver-level optimisation
patterns, and production-grade code structure. Six engineering sessions were conducted over
consecutive days, taking the codebase from a non-rendering stub to a full DXR hybrid-shadow
pipeline with a scene-graph-driven glTF mesh renderer.

```
LunaEngine-source/
├── LunaApp/src/LunaApp.cpp        — entry layer, ExampleLayer, CreateApplication()
├── LunaEngine/src/LunaEngine/
│   ├── Application/               — window init, GLFW callbacks, Run() loop
│   ├── Components/                — Component, Transform, MeshRenderer, GameObject
│   ├── Graphics/                  — IPipeline, IBuffer, Texture
│   ├── Manager/                   — SceneManager
│   ├── Renderer/
│   │   ├── Camera.h/cpp           — orbital camera
│   │   ├── Mesh.h                 — PBRVertex + Mesh (D3D12MA VB/IB)
│   │   ├── MeshLoader.h/cpp       — cgltf glTF/GLB → DEFAULT heap
│   │   ├── HAL/                   — IRenderBackend, IRenderContext, IRenderDevice
│   │   ├── DX12/                  — DX12Backend, DX12Device, DX12Pipeline,
│   │   │                            DX12Buffer, DX12AccelStructure, DX12RTPipeline
│   │   └── Vulkan/                — VulkanBackend, VulkanDevice
│   ├── Scene/                     — Scene (GameObject list)
│   └── Shaders/
│       ├── constantbuffer.vert/frag.hlsl   — triangle (SM 6.0)
│       ├── pbr.vert/frag.hlsl              — Cook-Torrance PBR (SM 6.0)
│       ├── shadows.hlsl                    — DXR RayGen/Miss/ClosestHit (SM 6.5)
│       └── mesh_preview.vert/frag.hlsl     — normal-diffuse preview (SM 6.0)
└── progress/code-review.md        — full item-by-item engineering log
```

---

## Session 1 — Critical Crash & Correctness Fixes

**Status:** DX12 device was created inside `DX12Device` but never wired into `DX12Backend`.
Every D3D12 call was a null-pointer dereference before the window even appeared.

| Fix | File | Detail |
|-----|------|--------|
| Device wiring | `DX12Backend.cpp` | `_device = _dx12Device->GetDeviceComPtr()` — was uninitialised |
| Getter additions | `DX12Device.h/cpp` | `GetDeviceComPtr()`, `GetMSAAQuality()`, called `SetMultiSampleQualityLevels()` from constructor (was declared but never invoked) |
| Stale declarations removed | `DX12Backend.h` | Removed `_adapter`, `msQualityLevels`, and three `Create*` methods that belonged only to `DX12Device` |
| Shutdown | `DX12Backend.cpp` | Added `WaitSync()` + `CloseHandle(_fenceEvent)` — destructor was `= default`, HANDLE leaked |
| Back-buffer index | `DX12Backend.cpp` | `EndFrame()` uses `_swapChain->GetCurrentBackBufferIndex()` — manual counter would desync |
| Static dispatch | `DX12Backend.cpp` | `dynamic_cast` → `static_cast` in `BindPipeline()` / `SetVertexBuffer()` — types are statically known |
| COLOR format | `DX12Pipeline.cpp` | `DXGI_FORMAT_R32G32B32_FLOAT` → `DXGI_FORMAT_R32G32B32A32_FLOAT` — 4 bytes of GPU garbage per vertex |
| Vertex shader | `triangle.vert.hlsl` | `float3 col` → `float4 col`, added MVP transform, added `return output` |
| Pixel shader | `triangle.frag.hlsl` | File was empty — added passthrough of vertex color |
| Vulkan instance | `VulkanBackend.cpp` | `glfwGetRequiredInstanceExtensions()` not called — `VK_KHR_surface` missing → guaranteed surface creation failure |
| Vulkan ImGui shutdown | `VulkanBackend.cpp` | `ImGui_ImplVulkanH_Window()` was constructing a temporary; replaced with actual shutdown sequence |
| IRenderContext guards | `IRenderContext.cpp` | `Init()` failure resets `s_Backend`; `Shutdown()` null-checks; `VulkanMolt` early-returns |
| Application bugs | `Application.cpp` | `Get()` returned a default-constructed instance (not the real one); `GetNativeWindow()` had no return; duplicate `Shutdown()` call in Run loop |

---

## Session 2 — Logging Unification

Replaced all `std::cerr`, `wprintf`, and stream-based log macros with a unified
`LUNA_LOG_INFO/WARN/ERROR(fmt, ...)` printf-style system with `do{}while(0)` guards.
Covers `DX12Backend`, `DX12Device`, `DX12Pipeline`, `DX12Shader`, `VulkanBackend`.

---

## Session 3 — Phase 1: First Triangle

Brought the engine from "compiles but crashes" to "DX12 triangle renders in window."

| Fix | Detail |
|-----|--------|
| `IPipeline.h` | Renamed `~IPipelineState` → `~IPipeline` (undefined base class typo) |
| Include path | `DX12Buffer.cpp` — `"Renderer/IRenderContext.h"` → `"Renderer/HAL/Public/IRenderContext.h"` |
| HLSL shaders | `constantbuffer.vert.hlsl` — added `row_major float4x4`, MVP transform, `return output` |
| `Component.h` | `FIXED_COMPONENT_COUNT = END - 1` → `= END` — Transform slot (index 2) was out of range and silently dropped |
| `Transform.h/cpp` | Added `position/rotation/scale` fields + `GetWorldMatrix()` (SRT decomposition via XMMatrix) |
| `GameObject.cpp` | `GetTransform()` was `return nullptr`; now `static_pointer_cast<Transform>(_components[TRANSFORM])` |
| Swapchain MSAA | Removed MSAA on swapchain — `DXGI_SWAP_EFFECT_FLIP_DISCARD` is incompatible with `Count > 1` |
| Depth buffer | Added `CreateDepthBuffer()` — DSV heap + D32_FLOAT resource, recreated on resize |
| Vertex buffer | Added `CreateVertexBuffer()` — UPLOAD heap upload of `s_Vertices` |
| MVP CB | Added `CreateMVPConstantBuffer()` — persistently-mapped UPLOAD buffer |
| `BeginFrame` | `ClearDepthStencilView` + `OMSetRenderTargets` passes `&_dsvHandle` |
| `DrawFrame` | Wired `_cbvPipeline` bind → `IASetVertexBuffers` → `SetGraphicsRootConstantBufferView(0)` → `Draw(3)` |

**Result:** DX12 triangle renders with MVP constant buffer.

---

## Session 4 — Phases 2–4: GPU Architecture + PBR + DXR

### Phase 2A — Frames-in-Flight Ring Buffer

Eliminated the per-frame CPU stall caused by a single command allocator.

```cpp
// DX12Backend.h
static constexpr UINT FRAMES_IN_FLIGHT = 2;
struct FrameResource {
    ComPtr<ID3D12CommandAllocator> cmdAllocator;
    ComPtr<ID3D12Resource>         mvpCB;
    void*                          mvpCBMapped  = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS      mvpCBGPUAddr = 0;
    UINT64                         fenceValue   = 0;
};
FrameResource _frames[FRAMES_IN_FLIGHT];
UINT64        _globalFenceValue = 0;
UINT          _frameIndex       = 0;
```

- `BeginFrame()` waits at **start** (not end) for the oldest frame slot via `WaitForFrame(_frameIndex)`
- `EndFrame()` signals `++_globalFenceValue`, records it in `_frames[_frameIndex].fenceValue`, advances ring index
- `Shutdown()` calls `WaitAllFrames()` before any resource release

### Phase 2B — DXC / SM 6.x

Replaced FXC (`D3DCompileFromFile`) with DXC (`IDxcCompiler3::Compile`).

```cpp
// DX12Pipeline.h
static ComPtr<IDxcUtils>     s_DxcUtils;
static ComPtr<IDxcCompiler3> s_DxcCompiler;
static void EnsureDXCInitialized();
```

- Targets: `vs_6_0` / `ps_6_0` (VS/PS), `lib_6_5` (DXR)
- Debug builds add `-Zs -Od`; HLSL 2021 via `-HV 2021`
- `IDxcBlob::GetBufferPointer/Size` is compatible with `D3D12_SHADER_BYTECODE` — no PSO changes

### Phase 2C — D3D12MA + DEFAULT Heap

Moved vertex buffer from UPLOAD heap to DEFAULT heap via D3D12 Memory Allocator.

```
Allocate DEFAULT buffer (D3D12MA) ──→ COPY_DEST
Allocate UPLOAD staging (D3D12MA)
Map staging → memcpy → Unmap
CopyBufferRegion(default ← staging)
Barrier: COPY_DEST → VERTEX_AND_CONSTANT_BUFFER
Execute → Signal → WaitForFrame(0)   ← blocks once at startup (acceptable)
Release staging immediately
```

`D3D12MemAlloc.cpp` compiled in its own non-PCH translation unit (premake `NoPCH` filter).

### Phase 3A — Orbital Camera + GLFW Callbacks

```cpp
class Camera {
    float _yaw=0.f, _pitch=30.f, _radius=3.f;
    XMFLOAT3 _target={0,0,0};
    float _fovRad, _aspect, _nearZ, _farZ;
public:
    void     Orbit(float dYaw, float dPitch);   // left-drag
    void     Zoom(float delta);                  // scroll wheel
    void     SetAspect(float aspect);            // on resize
    XMMATRIX GetViewMatrix() const;              // XMMatrixLookAtLH
    XMMATRIX GetProjectionMatrix() const;        // XMMatrixPerspectiveFovLH
    XMFLOAT3 GetEyePosition() const;
};
```

GLFW callbacks registered via `glfwSetWindowUserPointer`: framebuffer-resize, mouse-button, mouse-move (0.3°/px sensitivity), scroll. MVP is built from camera matrices and passed to `IRenderContext::UpdateMVP()` once per frame before `BeginFrame()`.

### Phase 3B — cgltf glTF Loader

`MeshLoader::LoadGLTF()` uses cgltf (single-header, `CGLTF_IMPLEMENTATION` in `MeshLoader.cpp`):

```cpp
struct PBRVertex {
    XMFLOAT3 position;  // POSITION : 0  — 12 B
    XMFLOAT3 normal;    // NORMAL   : 0  — 12 B
    XMFLOAT2 uv;        // TEXCOORD : 0  —  8 B
    XMFLOAT4 tangent;   // TANGENT  : 0  — 16 B
};  // stride: 48 B
```

Reads POSITION/NORMAL/TEXCOORD_0/TANGENT accessors, interleaves into `PBRVertex[]`, uploads VB+IB to DEFAULT heap via the same D3D12MA staging pattern from Phase 2C. Returns `vector<unique_ptr<Mesh>>`.

### Phase 3C — PBR Pipeline + Cook-Torrance Shaders

`pbr.frag.hlsl` implements the full Cook-Torrance BRDF:

- **D term:** GGX normal distribution (`D_GGX`)
- **G term:** Smith-Schlick geometry shadowing (`G_SmithSchlick`)
- **F term:** Schlick approximation (`F_Schlick`)
- Normal mapping via TBN matrix from tangent-space normal map
- Shadow occlusion factor from `t3 = shadowMap` (DXR output)
- Reinhard tone mapping + γ 2.2 correction

SRV heap expanded to 1024 slots with `AllocateSRVSlot()` returning CPU+GPU handle pairs:
- `[0]` = ImGui font SRV
- `[1..N]` = texture SRVs (albedo/normal/metalRough per material)
- `[N+1..]` = DXR UAV

### Phase 3D — Texture Loading

`Texture::Load()` uses stb_image → uploads RGBA8 `Texture2D` to DEFAULT heap:
- Row pitch aligned to `D3D12_TEXTURE_DATA_PITCH_ALIGNMENT` (256 B)
- UPLOAD staging → `CopyTextureRegion` → barrier → `CreateSRV()`

### Phase 4A — DXR Capability Check

```cpp
bool DX12Device::SupportsDXR() const {
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 opts5 = {};
    _device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opts5, sizeof(opts5));
    return opts5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_0;
}
```

`_dxrSupported` flag gates all DXR paths — engine degrades gracefully to raster-only on older GPUs.

### Phase 4B — BLAS / TLAS

`DX12AccelStructure` builds per-mesh BLASes and one scene TLAS:
- **BLAS:** `D3D12_RAYTRACING_GEOMETRY_DESC` with DEFAULT-heap VB+IB, prebuild info, scratch+result on DEFAULT heap, UAV barrier after build
- **TLAS:** identity `D3D12_RAYTRACING_INSTANCE_DESC[]` in UPLOAD heap, `BuildRaytracingAccelerationStructure`, UAV barrier

### Phase 4C — DXR Pipeline + Shader Table

`shadows.hlsl` (SM 6.5, `lib_6_5`):

```hlsl
[shader("raygeneration")] void RayGen() {
    // Reconstruct world position from depth buffer + inverse VP matrix
    float depth = depthBuffer[idx].r;
    float4 ndcPos = float4(ndcXY, depth, 1.0);
    float4 worldPos = mul(invVP, ndcPos) / w;
    // Trace shadow ray toward directional light
    RayDesc ray; ray.Origin = worldPos.xyz + N*0.001; ray.Direction = -lightDir;
    ray.TMin = 0.001; ray.TMax = 1e4;
    ShadowPayload p = { false };
    TraceRay(sceneTLAS, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFF, 0,0,0, ray, p);
    shadowOutput[idx] = p.shadowed ? 0.0 : 1.0;
}
```

`DX12RTPipeline`: RTPSO subobjects → DXIL library, hit group, shader/pipeline config, global root signature (b0 inline CBV, t0 inline TLAS SRV, t1 depth table, u0 shadow UAV table). Shader table: 3 records (RayGen/Miss/HitGroup) at 64-byte alignment.

### Phase 4D — DrawFrame Integration

```
BeginFrame()
  ├─ depth barrier: DEPTH_WRITE → NON_PIXEL_SHADER_RESOURCE
  ├─ cmdList.As<ID3D12GraphicsCommandList4>() → DispatchRays (shadow pass)
  ├─ barrier: shadowUAV UAV → PIXEL_SHADER_RESOURCE
  ├─ barrier: depth → DEPTH_WRITE (restore)
  ├─ PBR raster pass (bind TLAS + shadowMap SRVs + per-material textures)
  └─ Restore shadowUAV → UNORDERED_ACCESS for next frame
```

Shadow UAV (`R32_FLOAT`, viewport-sized, DEFAULT heap). On resize: slot index is cached (`_shadowUAVSRVIndex != 0` prevents heap leak).

---

## Session 5 — Vulkan Parity

Replaced the `ImGui_ImplVulkanH_Window` demo helper with a production Vulkan frame loop.

### Swapchain ownership

```cpp
// VulkanBackend.h
static constexpr uint32_t FRAMES_IN_FLIGHT = 2;
struct VkFrameResource {
    VkCommandPool   cmdPool    = VK_NULL_HANDLE;
    VkCommandBuffer cmdBuffer  = VK_NULL_HANDLE;
    VkFence         fence      = VK_NULL_HANDLE;  // starts signalled
    VkSemaphore     imageReady = VK_NULL_HANDLE;  // from vkAcquireNextImageKHR
    VkSemaphore     renderDone = VK_NULL_HANDLE;  // from vkQueueSubmit
};
VkFrameResource _frames[FRAMES_IN_FLIGHT];
uint32_t _frameIndex  = 0;
bool     _frameActive = false;  // false if acquire returned OUT_OF_DATE
```

### Frame loop

```
BeginFrame():  wait fence → vkAcquireNextImageKHR → handle OUT_OF_DATE →
               reset fence → vkResetCommandBuffer → vkBeginCommandBuffer →
               vkCmdBeginRenderPass → set viewport/scissor → _frameActive = true

DrawFrame():   (empty in Vulkan — meshes via SceneManager::Update)

EndFrame():    if (!_frameActive) return →
               vkCmdEndRenderPass → vkEndCommandBuffer →
               vkQueueSubmit(wait imageReady, signal renderDone, fence) →
               vkQueuePresentKHR → handle OUT_OF_DATE → advance _frameIndex
```

### Key details

- `VkDebugUtilsMessengerEXT` loaded at runtime via `vkGetInstanceProcAddr` (not in dispatch table); WARNING/ERROR routed to `LUNA_LOG_ERROR`; guarded by `#if defined(_DEBUG)`
- Swapchain format: `VK_FORMAT_B8G8R8A8_UNORM`, present mode: `VK_PRESENT_MODE_FIFO_KHR`
- ImGui initialised with `_renderPass` (not `_windowData.RenderPass`); font texture uploaded via one-shot on `_frames[0]`
- Static `DestroyDebugMessengerEXT()` helper loads the destroy function pointer at call site

---

## Session 6 — Scene Graph Wiring

Completed the full `SceneManager → Scene → GameObject → MeshRenderer → IRenderContext → DX12Backend` draw chain.

### Mesh preview pipeline

```hlsl
// mesh_preview.vert.hlsl — SM 6.0
cbuffer Transform : register(b0) { row_major float4x4 model, view, proj; };
struct VSInput  { float3 position:POSITION; float3 normal:NORMAL;
                  float2 uv:TEXCOORD0;     float4 tangent:TANGENT; };
struct VSOutput { float4 position:SV_POSITION; float3 worldNormal:NORMAL; };
VSOutput main(VSInput i) {
    float4 wp = mul(float4(i.position, 1.f), model);
    o.position    = mul(mul(wp, view), proj);
    o.worldNormal = mul((float3x3)model, i.normal);
}

// mesh_preview.frag.hlsl — SM 6.0
float4 main(PSInput i) : SV_TARGET {
    float3 N = normalize(i.worldNormal);
    float  d = saturate(dot(N, normalize(float3(0.4,1.0,0.6)))) * 0.85 + 0.15;
    return float4(float3(0.72,0.76,0.82) * d, 1.0);
}
```

### Vertex layout switch

Added `VertexLayout` enum to `PipelineStateDesc`. `DX12Pipeline::CreatePipelineState()` selects the input layout at PSO creation:

```
Triangle: POSITION(RGB32F, 0) | COLOR(RGBA32F, 12)          — stride 28 B
PBR:      POSITION(RGB32F, 0) | NORMAL(RGB32F, 12) |
          TEXCOORD(RG32F, 24) | TANGENT(RGBA32F, 32)        — stride 48 B
```

### Call chain

```
Application::Run()
  UpdateMVP(identity, camera.view, camera.proj)   ← caches _lastView/_lastProj
  BeginFrame()                                    ← opens command list
  DrawFrame()                                     ← DXR + triangle (if no meshes)
  SceneManager::Update()
    Scene::Update()
      GameObject::Update()
        MeshRenderer::Update() → Render()
          transform = GetTransform()->GetWorldMatrix()
          IRenderContext::DrawMesh(_mesh.get(), model)
            s_Backend->DrawMesh(mesh, model)       ← virtual dispatch
              DX12Backend::DrawMesh():
                update MVP CB (model + _lastView/_lastProj)
                BindPipeline(_meshPreviewPipeline)
                IASetVertexBuffers / IASetIndexBuffer
                SetGraphicsRootConstantBufferView(0, mvpCBGPUAddr)
                DrawIndexedInstanced(mesh->indexCount, 1, 0, 0, 0)
  RenderImGui()                                   ← descriptor heap set, ImGui draw
  EndFrame()                                      ← close, execute, present
```

### LoadMeshes flow

```
Application::Init() → SceneManager::LoadScene("Test")
  → LoadTestScene()
      dx12->LoadMeshes("Assets/DamagedHelmet.glb")
        WaitAllFrames()
        frame[0].cmdAllocator->Reset() / commandList->Reset()
        MeshLoader::LoadGLTF(path, device, allocator, commandList)
          cgltf_parse_file + cgltf_load_buffers
          interleave POSITION/NORMAL/TEXCOORD_0/TANGENT → PBRVertex[]
          UploadBuffer(VB) + UploadBuffer(IB)  ← DEFAULT heap via D3D12MA staging
        commandList->Close() → Execute → Signal(_globalFenceValue++) → WaitForFrame(0)
        unique_ptr<Mesh> → shared_ptr<Mesh> → _sceneMeshes
      for each mesh: GameObject::Init() + MeshRenderer::SetMesh(mesh)
      scene->AddGameObject(go)
```

### Lifetime management

`Application::Shutdown()` calls `SceneManager::ResetActiveScene()` **before** `IRenderContext::Shutdown()`. This destroys all `GameObjects → MeshRenderers → shared_ptr<Mesh>` references. `DX12Backend::Shutdown()` then calls `_sceneMeshes.clear()`, at which point the ref-count reaches 0, `~Mesh()` fires, and `D3D12MA::Allocation::Release()` executes while the allocator is still alive.

---

## Vendor Prerequisites

> All vendor libraries are gitignored. You must install them manually before building.

| Library | Version | Location | Purpose |
|---------|---------|----------|---------|
| DirectX Headers | latest | `vendor/dxheaders/` | d3d12.h, dxgi.h, CD3DX12 helpers |
| DXC | 1.8+ | `vendor/dxc/` | HLSL SM 6.x compiler (dxcapi.h + dxcompiler.lib) |
| D3D12MA | 2.x | `vendor/d3d12ma/` | D3D12 Memory Allocator |
| DirectXTex | latest | `vendor/DirectXTex/` | Texture utilities |
| GLFW | 3.4 | `vendor/glfw/` | Window + input |
| ImGui | 1.91+ | `vendor/imgui/` | UI overlay |
| GLM | 0.9.9 | `vendor/glm/` | Math (supplementary) |
| Vulkan SDK | 1.3+ | `$VULKAN_SDK` | Vulkan headers + loader |
| cgltf | 1.14 | `vendor/cgltf/cgltf.h` | Single-header glTF loader |
| stb_image | 2.29 | `vendor/stb_image/` | Single-header image loader |

### Build steps

```bat
:: 1. Install Vulkan SDK and set VULKAN_SDK environment variable
:: 2. Place vendor libraries in vendor/ as shown above
:: 3. Generate Visual Studio solution
premake5 vs2022

:: 4. Open LunaEngine.sln and build in Debug or Release
:: 5. Place test asset
mkdir LunaApp\Assets
copy DamagedHelmet.glb LunaApp\Assets\

:: 6. Run from LunaApp\bin\Debug\ or set working directory in project properties
```

---

## Remaining Work

| ID | Priority | Description |
|----|----------|-------------|
| P0-04 | Medium | `DX12Buffer::GetBackend()` uses runtime `dynamic_cast` — pass device at construction |
| P0-05 | High | `IBuffer::CreateBuffer()` hardcodes `DX12Buffer` regardless of backend — crashes under Vulkan |
| P0-06 | Medium | `IRenderDevice::Initialize(VkInstance, VkSurfaceKHR)` — Vulkan types in backend-agnostic interface |
| P1-04 | Low | Vulkan: no dedicated transfer queue for async uploads |
| P2-04 | Low | `Present(1,0)` hardcodes vsync — add `ApplicationSpecification::vsync` toggle |
| P2-05 | Low | `DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH` without `MakeWindowAssociation(DXGI_MWA_NO_ALT_ENTER)` |
| P3-01 | Low | `using namespace std/DirectX/WRL` in PCH — pollutes every TU |
| P3-04 | Low | `GetDeviceName()` returns a literal, not the actual DXGI adapter name |
| P4-06 | Low | `IRenderSwapChain.h` is an empty interface — never implemented |
