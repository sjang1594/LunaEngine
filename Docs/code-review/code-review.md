# LunaEngine — Engineering Tracking Document

**Branch:** `abstraction-level`
**Last Updated:** 2026-04-11
**Reviewer:** Senior Graphics Engineer (Claude)

> **Session 4 (2026-04-10):** Implemented Phases 2–4 — frames-in-flight (2A), DXC/SM6.x (2B), D3D12MA DEFAULT heap (2C), orbital camera + GLFW callbacks (3A), cgltf glTF mesh loader (3B), Cook-Torrance PBR pipeline + shaders (3C), stb_image texture upload (3D), DXR device check (4A), BLAS/TLAS acceleration structures (4B), DXR RTPSO + shadows.hlsl (4C), DrawFrame DXR integration with fallback (4D).
>
> **Session 5 (2026-04-10):** Vulkan parity — replaced `ImGui_ImplVulkanH_Window` with a proper `VkSwapchainKHR` + `VkRenderPass` + per-frame `VkCommandBuffer`/semaphore/fence ring buffer. Implemented `BeginFrame`, `DrawFrame`, `EndFrame`, `Resize`, swapchain recreation on `VK_ERROR_OUT_OF_DATE_KHR`. Added `VkDebugUtilsMessenger` for validation output (P1-05). Added `GetPresentQueueFamily()` to `VulkanDevice`. Resolves P0-01, P0-02, P1-05.
>
> **Session 6 (2026-04-11):** Scene graph wiring — implemented full MeshRenderer→IRenderContext→DX12Backend draw path. New `mesh_preview.vert/frag.hlsl` (PBR vertex, normal-diffuse shading). Added `VertexLayout` enum to `PipelineStateDesc`; `DX12Pipeline::CreatePipelineState()` switches between Triangle (POSITION+COLOR) and PBR (POSITION+NORMAL+TEXCOORD+TANGENT) input layouts. `DX12Backend::LoadMeshes()` loads glTF via `MeshLoader`, blocks on GPU upload, stores `shared_ptr<Mesh>` in `_sceneMeshes`. `DX12Backend::DrawMesh()` binds `_meshPreviewPipeline`, updates MVP CB with per-object model + cached camera matrices, calls `DrawIndexedInstanced`. `MeshRenderer::Render()` reads Transform world matrix and calls `IRenderContext::DrawMesh()`. Fixed `MeshRenderer` private inheritance bug. Fixed `SceneManager::_instance` missing definition and `LoadTestScene()` empty body. `Application` wired: `LoadScene()` in `Init()`, `SceneManager::Update()` after `DrawFrame()` in `Run()`, `ResetActiveScene()` at start of `Shutdown()` to ensure `~Mesh()` fires before D3D12MA allocator is released. Resolves P4-02, P4-03.

---

## Table of Contents

1. [Completed Fixes](#completed-fixes)
2. [Remaining Work — Categorized by Layer](#remaining-work)
   - [P0 — Correctness / Won't Work Without This](#p0--correctness)
   - [P1 — GPU Architecture / Performance](#p1--gpu-architecture--performance)
   - [P2 — API Best Practice](#p2--api-best-practice)
   - [P3 — Code Quality / Architecture](#p3--code-quality--architecture)
   - [P4 — Feature Completeness](#p4--feature-completeness)

---

## Completed Fixes

All fixes are cross-referenced by file and the specific defect they resolve.

### Session 1 — Critical Crash & Correctness Fixes

| # | File(s) | What Was Fixed |
|---|---------|---------------|
| 1 | `DX12Backend.h/cpp` | Wired `DX12Device` into `DX12Backend` — `_device` and `_mdxgiFactory` were never initialized; `Init()` would null-deref on `_device->CreateCommandQueue()` |
| 2 | `DX12Device.h/cpp` | Added `GetDeviceComPtr()`, `GetMSAAQuality()` getters; `SetMultiSampleQualityLevels()` now called in constructor (was declared but never invoked) |
| 3 | `DX12Backend.h` | Removed redundant `_adapter`, `msQualityLevels`, `CreateDebugLayer/CreateFactoryAndAdapter/CreateDevice` declarations — these belonged exclusively to `DX12Device` |
| 4 | `DX12Backend.cpp` | Added proper `Shutdown()` — calls `WaitSync()` then `CloseHandle(_fenceEvent)`; destructor was `= default` leaving the HANDLE leaked |
| 5 | `DX12Backend.cpp` | `EndFrame()` uses `_swapChain->GetCurrentBackBufferIndex()` exclusively — manual `_backbufferIndex` counter would desync from the real swapchain index |
| 6 | `DX12Backend.cpp` | Replaced `dynamic_cast<DX12Pipeline*>` / `dynamic_cast<DX12Buffer*>` with `static_cast` in `BindPipeline()` and `SetVertexBuffer()` — types are statically known at call site |
| 7 | `DX12Pipeline.cpp` | `COLOR` input layout corrected: `DXGI_FORMAT_R32G32B32_FLOAT` → `DXGI_FORMAT_R32G32B32A32_FLOAT` to match `struct Vertex { Vec3, Vec4 }` — GPU was reading 4 bytes of garbage per vertex |
| 8 | `triangle.vert.hlsl` | Updated `float3 col` → `float4 col` throughout — must match input layout and vertex struct |
| 9 | `triangle.frag.hlsl` | Updated `float3 col` → `float4 col`; removed redundant `float4(col, 1.0)` packing |
| 10 | `VulkanBackend.cpp` | `CreateInstance()` now calls `glfwGetRequiredInstanceExtensions()` and passes result to `VkInstanceCreateInfo` — `VK_KHR_surface` + `VK_KHR_win32_surface` were missing, making `glfwCreateWindowSurface()` guaranteed to fail |
| 11 | `VulkanBackend.cpp` | Added `VK_EXT_DEBUG_UTILS_EXTENSION_NAME` + `VK_LAYER_KHRONOS_validation` in `#if _DEBUG` builds |
| 12 | `VulkanBackend.cpp` | `ShutdownImGui()` — was calling `ImGui_ImplVulkanH_Window()` (constructs a temporary, does nothing); replaced with `ImGui_ImplVulkan_Shutdown()` + `ImGui_ImplVulkanH_DestroyWindow()` |
| 13 | `VulkanBackend.cpp` | `Resize()` — destroyed old `_imguiDescriptorPool` before calling `SetupImGui()` (now `CreateImGuiDescriptorPool()`); previously the old pool was leaked every resize |
| 14 | `VulkanBackend.cpp` | `StartImGui()` — added missing `ImGui_ImplGlfw_NewFrame()` between `ImGui_ImplVulkan_NewFrame()` and `ImGui::NewFrame()`; mouse/keyboard input was not updating |
| 15 | `VulkanBackend.cpp` | Renamed `SetupImGui()` → `CreateImGuiDescriptorPool()` to clarify its purpose and remove ambiguity with the full ImGui init path |
| 16 | `VulkanBackend.cpp` | `Shutdown()` — added `vkDeviceWaitIdle()` before resource destruction; orders teardown correctly |
| 17 | `VulkanBackend.h` | Removed stale `SetupImGui()` from private API; matched header to implementation |
| 18 | `IRenderContext.cpp` | `VulkanMolt` case early-returns instead of falling through to `s_Backend->Init()` on a null pointer |
| 19 | `IRenderContext.cpp` | `Initialize()` — checks return value of `Init()`; resets `s_Backend` on failure instead of leaving a partially-constructed backend live |
| 20 | `IRenderContext.cpp` | `Shutdown()` — added null check + `s_Backend.reset()` (all other methods had this guard; Shutdown alone did not) |
| 21 | `Application.cpp` | `Get()` — was `static Application instance; return instance;` (default-constructed, different object from `g_instance`); fixed to `return *g_instance;` |
| 22 | `Application.cpp` | `GetNativeWindow()` — returned `void*` with no return statement (undefined behavior); now returns `glfwGetWin32Window()` on Win32, raw `GLFWwindow*` elsewhere |
| 23 | `Application.cpp` | `GLFW_EXPOSE_NATIVE_WIN32` was unconditionally defined then included `glfw3native.h` on all platforms; now guarded with `#ifdef _WIN32` |
| 24 | `Application.cpp` | Backend selection rewritten — was `#ifdef _WIN32 / DX12 only`, blocking Vulkan on Windows; now driven by `_specification.backend` regardless of OS |
| 25 | `Application.cpp` | `Run()` — removed `Shutdown()` call at end of loop; destructor already owns shutdown, causing GLFW to terminate twice |
| 26 | `Application.cpp` | `Shutdown()` — added `if (!_windowHandle) return;` re-entry guard; `_windowHandle = nullptr` after destroy; `_layerStack.clear()` before renderer teardown (prevents layer destructors from running after GPU is gone) |
| 27 | `Application.cpp` | `LUNA_LOG_ERROR("Failed to load icon: " + path)` — string concatenation doesn't work with printf-style macros; fixed to `LUNA_LOG_ERROR("...: %s", path.c_str())` |

---

### Session 2 — Logging Unification

| # | File(s) | What Was Fixed |
|---|---------|---------------|
| 28 | `Logger.h` | Upgraded from stream-based macros (`std::cout << x`) to printf-style `LUNA_LOG_INFO/WARN/ERROR(fmt, ...)` with `do { } while(0)` guards — old macros couldn't handle HRESULT hex values or wide strings |
| 29 | `DX12Device.cpp` | `wprintf(L"Selected Adapter: %s")` → `LUNA_LOG_INFO("Selected adapter: %ls", desc.Description)` |
| 30 | `DX12Backend.cpp` | All `std::cerr` with HRESULT values → `LUNA_LOG_ERROR("... HRESULT 0x%08lX", static_cast<unsigned long>(hr))` |
| 31 | `DX12Pipeline.cpp` | All `std::cerr` / bare `cout` → Logger macros; `device == nullptr` guard kept but logging unified |
| 32 | `DX12Shader.cpp` | All `std::cerr` → Logger macros; wide-string path uses `%ls` format |
| 33 | `VulkanBackend.cpp` | ImGui `CheckVkResultFn` lambda — `std::cerr` → `LUNA_LOG_ERROR("... VkResult %d", err)` |
| 34 | `LunaPCH.h` | Added `TODO` comment above `using namespace std` explaining the issue and tracking reference |

---

### Session 3 — Phase 1: Make it Actually Render

| # | File(s) | What Was Fixed |
|---|---------|---------------|
| 35 | `IPipeline.h` | Renamed `IPipelineState` → `IPipeline` — `DX12Pipeline : public IPipeline` was referencing an undefined class (only forward-declared in `IRenderBackend.h`); compile blocker |
| 36 | `DX12Buffer.cpp:4` | Fixed include path `"Renderer/IRenderContext.h"` → `"Renderer/HAL/Public/IRenderContext.h"` — compile blocker (P0-03) |
| 37 | `constantbuffer.vert.hlsl` | Added `row_major float4x4` qualifiers to cbuffer; implemented MVP transform (`proj * view * model * pos`) + `return output` — shader was a stub with no return (P0-07) |
| 38 | `constantbuffer.frag.hlsl` | Implemented passthrough pixel shader — file was empty; returns vertex color (P4-04) |
| 39 | `Component.h` | `FIXED_COMPONENT_COUNT = END - 1` → `= END` — old value was 2, TRANSFORM index is 2, so `2 < 2` failed and Transform was never stored in `_components` array (silent data loss) |
| 40 | `Transform.h/cpp` | Added `position`, `rotation`, `scale` fields + `GetWorldMatrix()` (S→R→T decomposition via XMMatrix) — component existed but had no data (P4-01) |
| 41 | `GameObject.h/cpp` | `GetTransform()` was `return nullptr` inline; now properly returns `static_pointer_cast<Transform>(_components[TRANSFORM])` (P0-08) |
| 42 | `DX12Backend.cpp` | `CreateSwapChain()` — removed conditional MSAA on swapchain: `DXGI_SWAP_EFFECT_FLIP_DISCARD` is incompatible with `SampleDesc.Count > 1`; was causing `CreateSwapChainForHwnd` to fail on most GPUs (P2-02 partial) |
| 43 | `DX12Backend.cpp` | Added `CreateDepthBuffer()` — DSV descriptor heap + D32_FLOAT resource created on DEFAULT heap at init and resize time (P1-03) |
| 44 | `DX12Backend.cpp` | Added `CreateVertexBuffer()` — uploads `s_Vertices` to UPLOAD heap; sets `_vertexBufferView` |
| 45 | `DX12Backend.cpp` | Added `CreateMVPConstantBuffer()` — persistently-mapped UPLOAD heap CBV; identity matrices by default (P2-03 partial) |
| 46 | `DX12Backend.cpp` | `BeginFrame()` — added `ClearDepthStencilView`; fixed `OMSetRenderTargets` to pass `&_dsvHandle` instead of `nullptr` (P1-03) |
| 47 | `DX12Backend.cpp` | `DrawFrame()` — replaced stub with: bind `_cbvPipeline`, `IASetVertexBuffers`, `SetGraphicsRootConstantBufferView(0, ...)`, `Draw(3)` (P2-03) |
| 48 | `DX12Backend.cpp` | `Resize()` — added `_depthBuffer.Reset()` + `CreateDepthBuffer()` so depth buffer tracks window size |
| 49 | `DX12Backend.h/cpp` | Replaced `_trianglePipeline` (triangle.vert/frag) with `_cbvPipeline` (constantbuffer.vert/frag, depth test enabled) |
| 50 | `DX12Pipeline.cpp` | Root signature reduced from 2 CBV params to 1 (b0 = MVP transform); second param was unused and caused validation warnings |

---

### Session 4 — Phases 2–4: GPU Architecture + PBR Scene + DXR Shadows

#### Phase 2A — Frames-in-Flight Ring Buffer (resolves P1-01)

| # | File(s) | What Was Implemented |
|---|---------|---------------------|
| 51 | `DX12Backend.h` | Added `FrameResource` struct (cmdAllocator, mvpCB, mapped ptr, GPU addr, fenceValue); `FRAMES_IN_FLIGHT=2`; ring buffer `_frames[2]`, `_frameIndex`, `_globalFenceValue`; removed old single-allocator members |
| 52 | `DX12Backend.cpp` | `CreateCommandQueueAndFenceEvent()` — creates N allocators; CL created from `_frames[0]` |
| 53 | `DX12Backend.cpp` | `CreateMVPConstantBuffer()` — creates N per-frame CBs, all mapped, identity defaults |
| 54 | `DX12Backend.cpp` | `BeginFrame()` — `WaitForFrame(_frameIndex)` → reset current frame allocator → reset CL; no end-of-frame stall |
| 55 | `DX12Backend.cpp` | `EndFrame()` — signals `++_globalFenceValue`, saves to `_frames[_frameIndex].fenceValue`, advances `_frameIndex = (_frameIndex+1) % FRAMES_IN_FLIGHT` |
| 56 | `DX12Backend.cpp` | `WaitForFrame(i)` / `WaitAllFrames()` — `WaitAllFrames()` called in `Shutdown()` before resource release |
| 57 | `DX12Backend.cpp` | `UpdateMVP()` — `memcpy` into `_frames[_frameIndex].mvpCBMapped` (new public method on IRenderBackend) |

#### Phase 2B — DXC / SM 6.x Integration (resolves P2-01)

| # | File(s) | What Was Implemented |
|---|---------|---------------------|
| 58 | `DX12Pipeline.h` | Replaced `ComPtr<ID3DBlob>` with `ComPtr<IDxcBlob>` for VS/PS; added `static ComPtr<IDxcUtils> s_DxcUtils`, `static ComPtr<IDxcCompiler3> s_DxcCompiler`; `EnsureDXCInitialized()` made **public** (needed by DX12RTPipeline) |
| 59 | `DX12Pipeline.cpp` | Replaced `D3DCompileFromFile` with `IDxcCompiler3::Compile()`; targets `vs_6_0` / `ps_6_0`; DXC args: `-E main`, `-T target`, `-HV 2021`, `-Zs -Od` in debug; `IDxcBlob::GetBufferPointer/Size` used for PSO (compatible interface) |

#### Phase 2C — DEFAULT Heap + D3D12MA (resolves P1-02)

| # | File(s) | What Was Implemented |
|---|---------|---------------------|
| 60 | `DX12Backend.h/cpp` | `D3D12MA::Allocator* _d3d12maAllocator`; `InitD3D12MA()` called after device creation |
| 61 | `DX12Backend.cpp` | `CreateVertexBuffer()` — allocates DEFAULT buffer via D3D12MA, UPLOAD staging buffer, `CopyBufferRegion`, barrier `COPY_DEST → VERTEX_AND_CONSTANT_BUFFER`, release staging; blocks once at startup |
| 62 | `premake5.lua` | Added `../vendor/d3d12ma/src/D3D12MemAlloc.cpp` to files; `NoPCH` filter for that file (no `D3D12MA_D3D12_HEADERS_ALREADY_INCLUDED` — file compiled without PCH needs its own d3d12.h) |

#### Phase 3A — Orbital Camera + GLFW Callbacks (resolves P3-07, P4-07)

| # | File(s) | What Was Implemented |
|---|---------|---------------------|
| 63 | `Renderer/Camera.h/cpp` | Orbital camera: yaw/pitch/radius spherical coords, `Orbit(dYaw, dPitch)`, `Zoom(delta)`, `GetViewMatrix()` (`XMMatrixLookAtLH`), `GetProjectionMatrix()` (`XMMatrixPerspectiveFovLH`), `SetAspect()` |
| 64 | `IRenderBackend.h` | Added `virtual void UpdateMVP(const XMFLOAT4X4& model, view, proj) {}` (default no-op so Vulkan stub compiles); added `#include <DirectXMath.h>` |
| 65 | `IRenderContext.h/cpp` | Added static `UpdateMVP()` delegating to backend |
| 66 | `Application.h/cpp` | Added `Camera _camera`, mouse drag state; registered framebuffer-resize, mouse-move/button, scroll callbacks via `glfwSetWindowUserPointer`; `Run()` computes MVP from camera before `BeginFrame()` |

#### Phase 3B — glTF Mesh Loader

| # | File(s) | What Was Implemented |
|---|---------|---------------------|
| 67 | `Renderer/Mesh.h/cpp` | `PBRVertex` (pos+normal+uv+tangent), `Mesh` struct with D3D12MA VB/IB allocations and `D3D12_VERTEX_BUFFER_VIEW` / `D3D12_INDEX_BUFFER_VIEW` |
| 68 | `Renderer/MeshLoader.h/cpp` | `LoadGLTF()` using cgltf (`CGLTF_IMPLEMENTATION` in MeshLoader.cpp); reads POSITION/NORMAL/TEXCOORD_0/TANGENT; uploads VB+IB to DEFAULT heap via staging |
| 69 | `premake5.lua` | Added `../vendor/cgltf` to includedirs |

#### Phase 3C — PBR Pipeline + Shaders

| # | File(s) | What Was Implemented |
|---|---------|---------------------|
| 70 | `Shaders/pbr.vert.hlsl` | SM 6.0 VS: transforms pos/normal/tangent to world space, computes TBN, outputs clip-space position |
| 71 | `Shaders/pbr.frag.hlsl` | SM 6.0 PS: Cook-Torrance BRDF (D_GGX, G_SmithSchlick, F_Schlick), normal mapping, shadow factor from shadowMap SRV at `t3`, Reinhard tone mapping + gamma |
| 72 | `DX12Backend.h/cpp` | SRV heap expanded to 1024 slots (`SRV_HEAP_SIZE=1024`); `AllocateSRVSlot()` public helper; `_srvAllocIndex` starts at 1 (slot 0 = ImGui font) |

#### Phase 3D — Texture Loading (resolves P4-05)

| # | File(s) | What Was Implemented |
|---|---------|---------------------|
| 73 | `Graphics/Texture.h/cpp` | `Load()` using stb_image (`STB_IMAGE_IMPLEMENTATION` in Texture.cpp); row-pitch aligned to `D3D12_TEXTURE_DATA_PITCH_ALIGNMENT`; Texture2D DEFAULT heap via D3D12MA + UPLOAD staging; `CreateSRV()` writes `DXGI_FORMAT_R8G8B8A8_UNORM` SRV descriptor |

#### Phase 4A — DXR Capability Check

| # | File(s) | What Was Implemented |
|---|---------|---------------------|
| 74 | `DX12Device.h/cpp` | `SupportsDXR()` — `QueryInterface` to `ID3D12Device5`, `CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5)`, returns `RaytracingTier >= D3D12_RAYTRACING_TIER_1_0` |

#### Phase 4B — BLAS/TLAS Acceleration Structures

| # | File(s) | What Was Implemented |
|---|---------|---------------------|
| 75 | `DX12AccelStructure.h/cpp` | `BuildBLAS()` — per-mesh BLAS with `D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES`, prebuild info, scratch+result DEFAULT-heap buffers, UAV barrier |
| 76 | `DX12AccelStructure.h/cpp` | `BuildTLAS()` — identity-transform instance descs in UPLOAD heap, builds TLAS from all BLASes, UAV barrier; `GetTLASAddress()` returns GPU VA |

#### Phase 4C — DXR Pipeline + Shader Table

| # | File(s) | What Was Implemented |
|---|---------|---------------------|
| 77 | `Shaders/shadows.hlsl` | SM 6.5 `lib_6_5`: RayGen (reconstructs world pos from depth+invViewProj, traces shadow ray with `RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH`), Miss (`shadowed=false`), ClosestHit (`shadowed=true`) |
| 78 | `DX12RTPipeline.h/cpp` | Global root sig (b0=ShadowCB inline CBV, t0=TLAS inline SRV, t1=depth table, u0=shadow UAV table); RTPSO subobjects: DXIL library, hit group, shader/pipeline config, global root sig; shader table: 3 records (RayGen/Miss/HitGroup); `DispatchShadows()` sets state + dispatches via `D3D12_DISPATCH_RAYS_DESC` |
| 79 | `DX12RTPipeline.h` | Uses `UINT64` instead of `D3D12_GPU_VIRTUAL_ADDRESS` in forward-decl header to avoid typedef redefinition conflict with d3d12.h |

#### Phase 4D — DrawFrame Integration

| # | File(s) | What Was Implemented |
|---|---------|---------------------|
| 80 | `DX12Backend.cpp` | `CreateShadowUAV()` — R32_FLOAT viewport-sized UAV on DEFAULT heap; slot reuse on resize (`_shadowUAVSRVIndex == 0` guard prevents heap slot leak) |
| 81 | `DX12Backend.cpp` | `DrawFrame()` — DXR path: depth barrier (`DEPTH_WRITE → NON_PIXEL_SHADER_RESOURCE`) → `cmdList.As<ID3D12GraphicsCommandList4>()` → `DispatchRays` → shadow UAV barrier (`UAV → PIXEL_SHADER_RESOURCE`) → restore depth → PBR rasterization → ImGui; graceful skip when `!_dxrSupported` |

---

### Session 6 — Scene Graph Wiring (resolves P4-02, P4-03)

| # | File(s) | What Was Implemented |
|---|---------|---------------------|
| 82 | `Shaders/mesh_preview.vert.hlsl` | New SM 6.0 VS: PBRVertex input (POSITION+NORMAL+TEXCOORD+TANGENT), same b0=MVP CBV root sig, outputs world-space normal |
| 83 | `Shaders/mesh_preview.frag.hlsl` | New SM 6.0 PS: hemisphere diffuse shading by world normal (silver-gray base, single directional light) |
| 84 | `Graphics/IPipeline.h` | Added `VertexLayout` enum (`Triangle`=POSITION+COLOR 28B, `PBR`=POSITION+NORMAL+TEXCOORD+TANGENT 48B) to `PipelineStateDesc`; fixed `~IPipelineState` → `~IPipeline` typo |
| 85 | `DX12Pipeline.cpp` | `CreatePipelineState()` switches input layout based on `_desc.vertexLayout`: Triangle layout (2 elements) or PBR layout (4 elements) |
| 86 | `DX12Backend.h` | Includes `Renderer/Mesh.h`; added `_meshPreviewPipeline`, `_sceneMeshes` (vector of shared_ptr<Mesh>), `_lastView`/`_lastProj` cache; added `DrawMesh()` override + `LoadMeshes()` public method |
| 87 | `DX12Backend.cpp` | `Init()`: initializes `_meshPreviewPipeline` (PBR layout, depth test on) after `_cbvPipeline` |
| 88 | `DX12Backend.cpp` | `UpdateMVP()`: caches `_lastView`/`_lastProj` for per-object MVP reconstruction in `DrawMesh()` |
| 89 | `DX12Backend.cpp` | `DrawFrame()`: wraps triangle draw in `if (_sceneMeshes.empty())` — triangle is the fallback when no glTF is loaded |
| 90 | `DX12Backend.cpp` | `Shutdown()`: calls `_sceneMeshes.clear()` before allocator release so `~Mesh()` → `Allocation::Release()` fires while allocator is alive |
| 91 | `DX12Backend.cpp` | `LoadMeshes(path)`: `WaitAllFrames()` → reset frame-0 allocator → `MeshLoader::LoadGLTF()` → close/execute/signal/wait → converts `unique_ptr<Mesh>` → `shared_ptr<Mesh>` into `_sceneMeshes`; returns shared_ptrs to caller |
| 92 | `DX12Backend.cpp` | `DrawMesh(mesh, model)`: updates MVP CB with per-mesh model + cached view/proj; binds `_meshPreviewPipeline`; `IASetVertexBuffers` + `IASetIndexBuffer`; `SetGraphicsRootConstantBufferView(0, ...)`; `DrawIndexedInstanced` |
| 93 | `IRenderBackend.h` | Forward-declares `struct Mesh`; adds `virtual void DrawMesh(const Mesh*, const XMFLOAT4X4&) {}` default no-op |
| 94 | `IRenderContext.h/cpp` | Forward-declares `struct Mesh`; adds static `DrawMesh()` delegating to `s_Backend->DrawMesh()` |
| 95 | `Components/MeshRenderer.h` | Fixed private inheritance (`MeshRenderer : Component` → `public Component`); removed nonexistent `class Material`; proper `struct Mesh` forward-decl in Luna namespace |
| 96 | `Components/MeshRenderer.cpp` | `Render()`: gets Transform world matrix → `XMStoreFloat4x4` → `IRenderContext::DrawMesh(_mesh.get(), model)` |
| 97 | `Manager/SceneManager.h` | Added `ResetActiveScene()` for safe pre-shutdown cleanup |
| 98 | `Manager/SceneManager.cpp` | Added out-of-line `_instance = nullptr` definition; implemented `LoadTestScene()` — casts to `DX12Backend*`, calls `LoadMeshes("Assets/DamagedHelmet.glb")`, creates `GameObject` + `MeshRenderer` per mesh, adds to scene |
| 99 | `Application/Application.cpp` | `Init()`: calls `SceneManager::GetInstance()->LoadScene(L"Test")` after backend init; `Run()`: calls `SceneManager::GetInstance()->Update()` after `DrawFrame()` (meshes draw into the open command list); `Shutdown()`: calls `ResetActiveScene()` first to free Mesh D3D12MA allocations cleanly |

---

## Remaining Work

Items are tagged: **[CRASH]** breaks at runtime, **[COMPILE]** won't build, **[PERF]** hits GPU performance, **[QUALITY]** code review concern, **[FEATURE]** missing capability.

---

### P0 — Correctness

These will either crash, produce wrong output, or fail validation.

#### P0-04 · `DX12Buffer::GetBackend()` uses `dynamic_cast` on a public API boundary `[QUALITY]`
**File:** `DX12Buffer.cpp:76-86`
The buffer calls `IRenderContext::GetBackend()` and `dynamic_cast`s it to `DX12Backend*` at runtime. `DX12Buffer` should receive the device and command list at construction time.

---

#### P0-05 · `IBuffer::CreateBuffer()` and `IShaderProgram::Create()` are DX12-only `[CRASH]`
**Files:** `IBuffer.h:26`, `IShader.h:25`
Both factory functions hard-code `DX12Buffer` / `DX12Shader` regardless of the active backend. Calling them under Vulkan crashes at the `assert(false && "IRenderBackend is not DX12Backend")`.

---

#### P0-06 · `IRenderDevice` has Vulkan types in a backend-agnostic interface `[COMPILE]`
**File:** `IRenderDevice.h:9`
`virtual bool Initialize(VkInstance instance, VkSurfaceKHR surface)` is declared in a header included by the DX12 backend, requiring `<vulkan/vulkan.h>` in DX12-only TUs.

---

### P1 — GPU Architecture / Performance

#### P1-04 · Vulkan: no dedicated transfer queue for uploads `[PERF]`
**File:** `VulkanDevice.cpp`
No dedicated transfer queue family (queue with `VK_QUEUE_TRANSFER_BIT` but not `VK_QUEUE_GRAPHICS_BIT`) for async texture/buffer uploads.

---

### P2 — API Best Practice

#### P2-04 · `Present(1, 0)` hardcoded vsync — no runtime toggle `[QUALITY]`
**File:** `DX12Backend.cpp:EndFrame()`
VSync should be configurable via `ApplicationSpecification` or a render settings struct.

---

#### P2-05 · `DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH` without ALT+ENTER handling `[QUALITY]`
**File:** `DX12Backend.cpp:CreateSwapChain()`
Without `IDXGIFactory::MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER)`, default DXGI behavior can interfere with GLFW window management.

---

### P3 — Code Quality / Architecture

#### P3-01 · `using namespace std/DirectX/WRL` in precompiled header `[QUALITY]`
**File:** `LunaPCH.h:21,54,55,56`
Every TU inherits these namespace injections. Remove from PCH; add `using namespace DirectX/WRL` only to `.cpp` files that use those types heavily.

---

#### P3-02 · `IRenderDevice` is nearly empty and adds no value `[QUALITY]`
**File:** `IRenderDevice.h`
Neither `DX12Device` nor `VulkanDevice` is used polymorphically anywhere. Either flesh it out (feature queries, device caps) or remove it.

---

#### P3-03 · `DX12Backend` includes `DX12Device.h` in its public header `[QUALITY]`
**File:** `DX12Backend.h:3`
Transitively pulls `ComPtr`, `IDXGIFactory6`, `ID3D12Device` into every includer. Forward-declare `DX12Device` and use `unique_ptr<DX12Device>` opaquely.

---

#### P3-04 · `DX12Device::GetDeviceName()` returns a literal, not the actual GPU name `[QUALITY]`
**File:** `DX12Device.h:11`, `VulkanDevice.h:16`
Adapter name is available from `DXGI_ADAPTER_DESC1::Description` (already queried during init). Should be stored and returned.

---

#### P3-05 · GLFW error callback still uses `fprintf` directly `[QUALITY]`
**File:** `Application.cpp:11-14`
Should use `LUNA_LOG_ERROR("GLFW error %d: %s", error, description)`.

---

#### P3-06 · `ImGui::ShowDemoWindow()` hardcoded in the main loop `[QUALITY]`
**File:** `Application.cpp`
Should be behind `#if defined(_DEBUG)` or a `bool _showDemoWindow` flag.

---

#### P3-08 · `DX12Buffer` couples itself to `IRenderContext` singleton `[QUALITY]`
**File:** `DX12Buffer.cpp:12-14`
Buffer constructor queries the global context. Device/state should be passed as constructor parameters.

---

#### P3-09 · `DX12Buffer::Bind()` conflates root parameter index with HLSL register index `[QUALITY]`
**File:** `DX12Buffer.cpp:101-106`
`slot` is treated as the root parameter index, not the HLSL `b#` register. Will silently bind to wrong slots as root signature grows.

---

### P4 — Feature Completeness

#### P4-06 · `IRenderSwapChain.h` is an empty interface `[FEATURE]`

#### P4-06 · `IRenderSwapChain.h` is an empty interface `[FEATURE]`
**File:** `HAL/Public/IRenderSwapChain.h`
Never implemented on either backend. Proper abstraction would expose `AcquireNextImage()`, `Present()`, `Resize()`, `GetCurrentImageIndex()`, `GetFormat()`.

---

## Architecture Roadmap

```
Phase 1 — Make it render ✅ DONE (Sessions 1–3)
  ✅ DX12Buffer include path (P0-03)
  ✅ constantbuffer.vert.hlsl MVP + return (P0-07)
  ✅ Depth buffer + DSV heap (P1-03)
  ✅ CBV bind in DrawFrame (P2-03)
  ✅ GetTransform() wired (P0-08)

Phase 2–4 — GPU architecture + PBR + DXR ✅ DONE (Session 4)
  ✅ Frames-in-flight ring buffer N=2 (P1-01)
  ✅ DXC / SM 6.x / DXIL (P2-01)
  ✅ DEFAULT heap VB + D3D12MA (P1-02)
  ✅ Orbital camera + resize callback (P3-07, P4-07)
  ✅ cgltf glTF mesh loader
  ✅ Cook-Torrance PBR shaders (SM 6.0)
  ✅ stb_image texture upload (P4-05)
  ✅ DXR BLAS/TLAS + shadow RTPSO (SM 6.5)
  ✅ DrawFrame DXR integration with graceful fallback

Vulkan parity ✅ DONE (Session 5)
  ✅ VkSwapchainKHR + VkRenderPass + VkFramebuffer[] (P0-02)
  ✅ Per-frame VkCommandBuffer + VkSemaphore + VkFence ring buffer (P0-01)
  ✅ BeginFrame / DrawFrame / EndFrame / Resize fully implemented
  ✅ ImGui_ImplVulkan_RenderDrawData wired into command buffer
  ✅ VkDebugUtilsMessenger active in debug builds (P1-05)

Scene graph wiring ✅ DONE (Session 6)
  ✅ mesh_preview.vert/frag.hlsl — PBR vertex, normal-diffuse shading
  ✅ VertexLayout enum + DX12Pipeline input layout switch
  ✅ DX12Backend::LoadMeshes() + DrawMesh() (P4-03, P4-02)
  ✅ MeshRenderer::Render() wired to IRenderContext::DrawMesh()
  ✅ SceneManager::LoadTestScene() loads DamagedHelmet.glb
  ✅ Application wires LoadScene/Update/ResetActiveScene

Next — Polish (P2, P3) + IRenderSwapChain (P4-06)
  ├── Place Assets/DamagedHelmet.glb in working directory
  └── IRenderSwapChain abstraction (P4-06)

Polish — (P2, P3)
  ├── Remove using namespace from PCH (P3-01)
  ├── VSync toggle (P2-04)
  ├── ALT+ENTER suppression (P2-05)
  └── GetDeviceName() returns real GPU name (P3-04)
```
