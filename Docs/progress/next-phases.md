# LunaEngine — Next Phases Roadmap

**Written:** 2026-04-11
**Status of codebase entering this roadmap:** Phases 1–4 + Vulkan parity + scene graph wiring all complete.
DX12 renders a glTF mesh with orbital camera, PBR shading, and DXR hybrid shadows.

---

## Reading order

Each phase lists:
- **Goal** — what it adds to the portfolio
- **Why it matters** — the senior interview answer
- **Key design** — concrete structs/APIs to build
- **Files** — which files to create or change
- **Depends on** — what must be done first

Phases are ordered so each one builds on the previous. Phases 5A and 5B are small;
the real senior-level work begins at Phase 6 (Render Graph).

---

## Phase 5A — API Correctness & Polish

**Goal:** Fix the 9 remaining code-review items before building bigger features on top.
Prevents technical debt from compounding.

### Fixes in priority order

| Item | File | Fix |
|------|------|-----|
| P0-05 | `IBuffer.h`, `IShader.h` | Factory functions (`CreateBuffer`, `Create`) hardcode `DX12Buffer`/`DX12Shader` — crash under Vulkan. Gate behind `#ifdef _WIN32` or replace with a virtual factory on `IRenderContext`. |
| P0-06 | `HAL/Public/IRenderDevice.h` | `Initialize(VkInstance, VkSurfaceKHR)` is a Vulkan signature in a backend-agnostic header. Split into `DX12Device::Initialize(ID3D12Device5*)` and `VulkanDevice::Initialize(VkInstance, VkSurfaceKHR)` — no base class method. |
| P0-04 | `DX12Buffer.cpp` | `GetBackend()` calls `dynamic_cast` on every buffer operation. Pass `ID3D12Device*` and `D3D12MA::Allocator*` to the constructor instead. |
| P3-03 | `DX12Backend.h` | Includes `DX12Device.h` — transitively pulls D3D12 types into every includer. Forward-declare `class DX12Device` and hold `unique_ptr<DX12Device>` opaquely. |
| P3-01 | `LunaPCH.h` | Remove `using namespace std/DirectX/WRL` from PCH. Add `using namespace DirectX` to `.cpp` files that need it. This is a correctness issue — `std::byte` collision already suppressed with a hack. |
| P2-04 | `DX12Backend.cpp` | Add `bool vsync` to `ApplicationSpecification`. `EndFrame()`: `Present(vsync ? 1 : 0, 0)`. |
| P2-05 | `DX12Backend.cpp` | Add `_mdxgiFactory->MakeWindowAssociation(_mainWindow, DXGI_MWA_NO_ALT_ENTER)` after swapchain creation. |
| P3-04 | `DX12Device.h/cpp` | Store `DXGI_ADAPTER_DESC1::Description` as `std::wstring _adapterName`; `GetDeviceName()` returns it. |
| P4-06 | `IRenderSwapChain.h` | Implement `AcquireNextImage()`, `Present()`, `Resize()`, `GetCurrentImageIndex()`, `GetFormat()` — or delete the file if you decide to keep swapchain inside the backend. |

**Complexity:** 1–2 sessions. No new GPU work — all refactoring.

---

## Phase 5B — Full PBR Material Pipeline

**Goal:** Wire `_pbrPipeline` (already declared, never initialized) and `pbr.vert/frag.hlsl`
(already written) to real textured glTF materials. Right now the mesh renders with flat
normal-diffuse shading. This phase makes it look production-quality.

**Why it matters:** Cook-Torrance PBR is the entry-level expectation at any AAA or middleware
company. Showing textures + normal maps + metallic workflow is table stakes.

### Key design

```cpp
// Graphics/Material.h  (new)
struct MaterialConstants
{
    XMFLOAT4 albedoFactor    = {1,1,1,1};
    float     metallicFactor  = 1.0f;
    float     roughnessFactor = 1.0f;
    float     _pad[2]         = {};
};

struct Material
{
    shared_ptr<Texture> albedo;       // t0
    shared_ptr<Texture> normalMap;    // t1
    shared_ptr<Texture> metalRough;   // t2 — G=roughness, B=metallic (glTF spec)

    ComPtr<ID3D12Resource> constantBuffer;
    void*                  cbMapped    = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS cbGPUAddr = 0;
    UINT srvTableStart = 0;  // first of 3 consecutive SRV slots in _imGuiSrvHeap
};
```

The PBR root signature (already in `pbr.frag.hlsl`):

```
b0 — MVP transform (existing)
b1 — MaterialConstants CBV
t0 — albedoTex     } descriptor table (3 SRVs from srvTableStart)
t1 — normalTex     }
t2 — metalRoughTex }
s0 — static sampler (anisotropic wrap)
```

### Files

| Action | File |
|--------|------|
| Create | `Graphics/Material.h/cpp` |
| Modify | `Renderer/Mesh.h` — add `Material* material` pointer |
| Modify | `Renderer/MeshLoader.cpp` — parse `cgltf_material`, call `Texture::Load()` for each map, populate `Material` |
| Modify | `DX12Backend.h/cpp` — initialize `_pbrPipeline` in `Init()`; `DrawMesh()` checks if mesh has material: if yes use `_pbrPipeline`, else fall back to `_meshPreviewPipeline` |
| Modify | `DX12Pipeline.cpp` — add new `CreateRootSignature()` overload for PBR (2 CBVs + 1 SRV table + 1 static sampler) |

### DX12Pipeline change

Add `RootSignatureLayout` enum to `PipelineStateDesc`:
```cpp
enum class RootSignatureLayout { MVP, PBR };
```

`CreateRootSignature()` switches between the two layouts. The PBR layout:
```cpp
CD3DX12_ROOT_PARAMETER params[3];
params[0].InitAsConstantBufferView(0);        // b0: MVP
params[1].InitAsConstantBufferView(1);        // b1: MaterialConstants
CD3DX12_DESCRIPTOR_RANGE srvRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0); // t0-t2
params[2].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);

D3D12_STATIC_SAMPLER_DESC sampler = {};  // s0: anisotropic wrap
```

**Complexity:** 1–2 sessions. Mostly wiring — the shaders are already written.

---

## Phase 6 — Render Graph

**Goal:** Replace the hardcoded `BeginFrame → DrawFrame → SceneUpdate → RenderImGui → EndFrame`
pipeline with a data-driven render graph that compiles render passes into a DAG, automatically
inserts resource barriers, and culls unused passes.

**Why it matters:** Every modern engine (Frostbite, Unreal, id Tech 7) uses a frame graph.
It is the architectural pattern that separates entry-level from senior graphics engineers.
Explaining it in an interview demonstrates systems design, not just API knowledge.

### Concept

```
Producer pass:  declares output resources
Consumer pass:  declares input resources (reads from a previous pass's output)
Compile():      topological sort → barrier schedule → culling of unreferenced passes
Execute():      iterate sorted passes, insert barriers, call execute lambda
```

### Key structs

```cpp
// Renderer/RenderGraph.h  (new)

// A handle to a transient resource — an opaque index
using RGResourceHandle = uint32_t;
constexpr RGResourceHandle RG_NULL_HANDLE = UINT32_MAX;

struct RGTextureDesc
{
    uint32_t        width, height;
    DXGI_FORMAT     format;
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
    float           clearColor[4] = {0,0,0,1};
};

struct RGPassDesc
{
    std::string                     name;
    std::vector<RGResourceHandle>   reads;   // SRV inputs
    std::vector<RGResourceHandle>   writes;  // RTV / UAV outputs
    std::function<void(ID3D12GraphicsCommandList*)> execute;
};

class RenderGraph
{
public:
    // Called during frame setup (no GPU work yet)
    RGResourceHandle CreateTexture(const RGTextureDesc& desc);
    void             AddPass(const RGPassDesc& desc);

    // Called once per frame: sort, allocate transients, schedule barriers
    void Compile();

    // Called once per frame: execute all passes in order
    void Execute(ID3D12GraphicsCommandList* cmd);

    // Retrieve the backing resource for a handle (after Compile)
    ID3D12Resource* GetResource(RGResourceHandle handle) const;

private:
    struct PassNode  { RGPassDesc desc; bool culled = false; };
    struct ResNode   { RGTextureDesc desc; ComPtr<ID3D12Resource> resource; D3D12_RESOURCE_STATES currentState; };

    std::vector<PassNode>  _passes;
    std::vector<ResNode>   _resources;

    void TopologicalSort(std::vector<uint32_t>& order);
    void ScheduleBarriers(const std::vector<uint32_t>& order);
    void AllocateTransients();
};
```

### Transient resource cache

Transient resources (shadowMap, GBuffer RTs, SSAO output) live for one frame only.
A cache keyed on `RGTextureDesc` reuses `D3D12MA::Allocation` objects across frames
without reallocating:

```cpp
class TransientResourceCache
{
    struct Entry { RGTextureDesc desc; ComPtr<ID3D12Resource> resource;
                   D3D12MA::Allocation* alloc; bool inUse; };
    std::vector<Entry> _pool;
public:
    ComPtr<ID3D12Resource> Acquire(const RGTextureDesc&, D3D12MA::Allocator*);
    void Release(ID3D12Resource*);
    void EndFrame();  // mark all as available
};
```

### How DrawFrame becomes

```cpp
void DX12Backend::DrawFrame()
{
    RenderGraph rg;

    auto shadowHandle = rg.CreateTexture({_w, _h, DXGI_FORMAT_R32_FLOAT,
                                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS});
    auto depthHandle  = rg.CreateTexture({_w, _h, DXGI_FORMAT_D32_FLOAT,
                                          D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL});
    auto hdrHandle    = rg.CreateTexture({_w, _h, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                          D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET});

    rg.AddPass({ "DXR Shadows",
        .reads  = {},
        .writes = {depthHandle, shadowHandle},
        .execute = [&](auto* cmd){ DispatchShadowsInternal(cmd, ...); } });

    rg.AddPass({ "PBR Forward",
        .reads  = {shadowHandle},
        .writes = {hdrHandle, depthHandle},
        .execute = [&](auto* cmd){ DrawSceneInternal(cmd, ...); } });

    rg.AddPass({ "Tonemapping",
        .reads  = {hdrHandle},
        .writes = {backBufferHandle},
        .execute = [&](auto* cmd){ TonemapInternal(cmd, ...); } });

    rg.Compile();
    rg.Execute(_commandList.Get());
}
```

**Complexity:** 2–3 sessions. This is the hardest phase. Do it before deferred/post-process
so those passes slot in cleanly.

**Files:**
- Create: `Renderer/RenderGraph.h/cpp`
- Create: `Renderer/TransientResourceCache.h/cpp`
- Modify: `DX12Backend.cpp::DrawFrame()` — replaced with graph construction

---

## Phase 7 — Deferred Rendering + G-Buffer

**Goal:** Move from forward rendering (one pass per object) to deferred rendering
(geometry pass into G-buffer, then a single lighting pass reading all G-buffer RTs).

**Why it matters:** Deferred rendering is the standard in open-world games (Frostbite,
Decima, UE4). It decouples geometry complexity from light count. For a portfolio it
demonstrates understanding of bandwidth, memory layout, and multi-RT writing.

**Depends on:** Phase 6 (Render Graph) — G-buffer passes are natural render graph nodes.

### G-Buffer layout

```
RT0: DXGI_FORMAT_R8G8B8A8_UNORM      — albedo.rgb | roughness
RT1: DXGI_FORMAT_R16G16B16A16_FLOAT  — worldNormal.xyz | metallic
RT2: DXGI_FORMAT_D32_FLOAT           — depth (already exists)
```

R16G16B16A16 for normals avoids the artefacts from oct-encoding on a portfolio scale.

### Passes

```
GeometryPass:
  Input:  scene mesh VBs/IBs, material textures
  Output: RT0 (albedo+roughness), RT1 (normal+metallic), Depth

LightingPass (fullscreen quad / fullscreen triangle):
  Input:  RT0, RT1, Depth (reconstruct world pos from depth + inv VP)
  Input:  shadowMap (from DXR or CSM)
  Output: HDR colour buffer
  Shader: reads G-buffer, runs Cook-Torrance BRDF for all lights
```

### Shaders to write

```
Shaders/gbuffer.vert.hlsl    — identical to pbr.vert but outputs to 2 RTs
Shaders/gbuffer.frag.hlsl    — writes albedo/roughness to RT0, normal/metallic to RT1
Shaders/deferred_light.vert.hlsl  — fullscreen triangle (no VB: SV_VertexID trick)
Shaders/deferred_light.frag.hlsl  — read G-buffer, run BRDF, output HDR
```

Fullscreen triangle without a vertex buffer:

```hlsl
// deferred_light.vert.hlsl — 3-vertex screen-covering triangle, no VB needed
void main(uint id : SV_VertexID, out float4 pos : SV_POSITION, out float2 uv : TEXCOORD)
{
    uv  = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2,-2) + float2(-1,1), 0, 1);
}
```

### Root signature for deferred lighting pass

```
t0 — albedoRoughTex (G-buffer RT0)
t1 — normalMetalTex (G-buffer RT1)
t2 — depthTex
t3 — shadowMap
b0 — CameraConstants (inv VP, eye position, viewport size)
b1 — LightConstants  (direction, colour, intensity)
s0 — point sampler
```

**Complexity:** 2 sessions. The BRDF is already written in `pbr.frag.hlsl` — copy it into
the lighting pass. The hard part is the G-buffer output format and normal reconstruction.

---

## Phase 8 — Cascaded Shadow Maps (CSM)

**Goal:** Add raster shadow maps as the fallback path when DXR is unavailable.
Both systems should be switchable at runtime.

**Why it matters:** CSM is the industry standard for directional light shadows.
It demonstrates understanding of view frustum splitting, depth bias, and percentage
closer filtering (PCF). Required knowledge for any rendering engineer role.

**Depends on:** Phase 6 (Render Graph). Phase 7 optional but useful.

### Design

```cpp
// Renderer/DX12/public/DX12CSM.h  (new)
class DX12CSM
{
public:
    static constexpr int CASCADE_COUNT = 4;

    bool Init(ID3D12Device*, D3D12MA::Allocator*, uint32_t shadowMapSize = 2048);
    void Update(const Camera& cam, const XMFLOAT3& lightDir);
    void RenderCascades(ID3D12GraphicsCommandList*, const std::vector<Mesh*>& meshes);
    // Returns SRV handle for use in lighting pass
    D3D12_GPU_DESCRIPTOR_HANDLE GetShadowAtlasSRV() const;

private:
    // Shadow atlas: shadowMapSize × (shadowMapSize × CASCADE_COUNT) — all 4 cascades in one texture
    ComPtr<ID3D12Resource>   _shadowAtlas;
    D3D12MA::Allocation*     _shadowAtlasAlloc = nullptr;
    ComPtr<ID3D12DescriptorHeap> _dsvHeap;      // 4 DSVs, one per cascade

    // Per-cascade light-space view-projection matrices
    XMFLOAT4X4 _lightSpaceMatrices[CASCADE_COUNT];
    float       _splitDepths[CASCADE_COUNT + 1];  // view-space split depths

    void ComputeSplitDepths(float nearZ, float farZ, float lambda = 0.5f);
    void ComputeLightSpaceMatrices(const Camera& cam, const XMFLOAT3& lightDir);
};
```

### Cascade split formula

```cpp
// Practical Split Scheme (PSS) — blend between log and uniform splits
for (int i = 1; i < CASCADE_COUNT; ++i)
{
    float ratio    = (float)i / CASCADE_COUNT;
    float logSplit = nearZ * pow(farZ / nearZ, ratio);
    float linSplit = nearZ + (farZ - nearZ) * ratio;
    _splitDepths[i] = lambda * logSplit + (1 - lambda) * linSplit;
}
```

### Shaders

```
Shaders/shadow_depth.vert.hlsl  — minimal VS: just transforms position by lightSpaceMatrix
Shaders/shadow_depth.frag.hlsl  — empty (depth-only pass)
```

PCF filtering in the lighting shader (4×4 Poisson disk):
```hlsl
float SampleShadow(Texture2DArray shadowAtlas, SamplerComparisonState cmpSampler,
                   float3 projCoords, int cascadeIndex)
{
    float shadow = 0.0;
    float2 texelSize = 1.0 / float2(2048, 2048);
    [unroll] for (int x = -1; x <= 1; ++x)
        [unroll] for (int y = -1; y <= 1; ++y)
            shadow += shadowAtlas.SampleCmpLevelZero(cmpSampler,
                float3(projCoords.xy + float2(x,y)*texelSize, cascadeIndex),
                projCoords.z - 0.005);  // bias
    return shadow / 9.0;
}
```

**Complexity:** 2 sessions. The shadow pass is straightforward; getting the split
depths and bias right for all cascade transitions is the tricky part.

---

## Phase 9 — Post-Processing Stack

**Goal:** Add SSAO, bloom, TAA, and filmic tone mapping as composable post-process passes
sitting on top of the HDR colour buffer.

**Why it matters:** Post-processing is visible to everyone and demonstrates mastery of
screen-space techniques, compute shaders, and temporal methods.

**Depends on:** Phase 6 (Render Graph), Phase 7 (deferred depth + normals available).

### SSAO

```
Input:  Depth, WorldNormal (from G-buffer RT1)
Output: OcclusionBuffer (R8_UNORM)

Algorithm:
1. Reconstruct view-space position from depth
2. Sample 32 hemisphere kernel points in view space (precomputed, stored in CBV)
3. Compare sample depth against depth buffer → occlusion contribution
4. Blur with 4×4 box filter (separable: H then V)
5. Multiply into lighting pass ambient term
```

```hlsl
// Shaders/ssao.hlsl (compute shader, SM 6.0)
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) { ... }
```

### Bloom (Dual Kawase)

More efficient than Gaussian at large radii. Two compute passes: downsample and upsample.

```
Downsample:  half-res → quarter-res → eighth-res (4 mip levels)
Upsample:    eighth → quarter → half → full-res (sum with weight)
```

```hlsl
// Shaders/bloom_downsample.hlsl
// Shaders/bloom_upsample.hlsl
// Both are [numthreads(8,8,1)] compute shaders reading/writing RWTexture2D
```

### TAA (Temporal Anti-Aliasing)

```
State added to DX12Backend:
  - ComPtr<ID3D12Resource> _historyBuffer  (R16G16B16A16_FLOAT, viewport-sized)
  - XMFLOAT4X4 _prevViewProj               (previous frame VP for reprojection)
  - uint32_t _frameCount                   (cycles 0-7 for Halton jitter sequence)

Per frame:
  1. Jitter projection matrix: add sub-pixel offset from Halton(2,3) sequence ×÷ viewport
  2. Reproject current pixel to previous frame UV using _prevViewProj + current depth
  3. Sample history buffer at reprojected UV (bilinear, clamped to AABB of 3×3 neighbours)
  4. Blend: colour = lerp(history, current, 0.1)  [10% current, 90% history]
  5. Copy current output → _historyBuffer for next frame
```

```hlsl
// Shaders/taa.hlsl
cbuffer TAAConstants : register(b0) {
    row_major float4x4 currentVP, prevVP, invVP;
    float2 jitter;
    float2 viewportSize;
};
```

### Tonemapping (ACES Filmic)

```hlsl
// Shaders/tonemap.hlsl
float3 ACESFilmic(float3 x)
{
    float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
    return saturate((x*(a*x+b)) / (x*(c*x+d)+e));
}
float4 main(PSInput i) : SV_TARGET {
    float3 hdr = hdrTex.Sample(s0, i.uv).rgb;
    float3 ldr = ACESFilmic(hdr * exposure);
    return float4(pow(ldr, 1.0/2.2), 1.0);  // gamma correction
}
```

### Pass order in Render Graph

```
GeometryPass → DXR/CSM Shadows → LightingPass (PBR+shadows) →
SSAO → BloomDownsample → BloomUpsample → TAA → Tonemap → UI (ImGui)
```

**Complexity:** 3–4 sessions. TAA is the hardest sub-problem (ghosting artefacts,
disocclusion, fast-moving objects). SSAO and bloom can ship in one session each.

---

## Phase 10 — Bindless Resources

**Goal:** Replace per-draw `SetDescriptorHeaps` + `SetGraphicsRootDescriptorTable` with a
persistent bindless descriptor heap. Shaders index resources by handle, eliminating CPU-side
descriptor binding overhead.

**Why it matters:** All modern AAA renderers are bindless (D3D12 and Vulkan). The technique
unlocks GPU-driven rendering (Phase 11) because draw calls no longer need CPU-side
descriptor setup. It demonstrates deep D3D12 knowledge.

**Depends on:** Phase 5A (clean buffer API). Can overlap with Phase 9.

### Design

```cpp
// Renderer/BindlessHeap.h  (new)
class BindlessHeap
{
public:
    static constexpr uint32_t MAX_DESCRIPTORS = 65536;

    bool Init(ID3D12Device* device);

    // Allocate a permanent slot and write an SRV/CBV/UAV descriptor into it.
    // Returns the uint32_t index used in shaders.
    uint32_t AllocateSRV(ID3D12Device*, ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC&);
    uint32_t AllocateCBV(ID3D12Device*, D3D12_GPU_VIRTUAL_ADDRESS, uint32_t sizeInBytes);
    uint32_t AllocateUAV(ID3D12Device*, ID3D12Resource*, const D3D12_UNORDERED_ACCESS_VIEW_DESC&);

    // Set heap at start of command list
    void Bind(ID3D12GraphicsCommandList* cmd) const;

    ID3D12DescriptorHeap* GetHeap() const { return _heap.Get(); }

private:
    ComPtr<ID3D12DescriptorHeap> _heap;
    std::atomic<uint32_t>        _nextSlot{0};
    uint32_t                     _descriptorSize = 0;
};
```

Bindless root signature (replaces all per-pipeline root signatures):

```cpp
// One global root signature shared by all pipelines
CD3DX12_ROOT_PARAMETER params[2];

// Root constants — material index, draw index, etc.
params[0].InitAsConstants(4, 0);  // b0: 4 × uint32_t push constants

// Single large descriptor table covering the whole heap
CD3DX12_DESCRIPTOR_RANGE range(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 0);
params[1].InitAsDescriptorTable(1, &range);
```

Shader side:

```hlsl
// Bindless access pattern in any shader
struct DrawConstants { uint materialIdx; uint objectIdx; uint2 pad; };
ConstantBuffer<DrawConstants> gConstants : register(b0);

// All resources live in one array — index via the handle
Texture2D gTextures[] : register(t0, space0);

float4 main(PSInput i) : SV_TARGET {
    MaterialData mat = gMaterials[gConstants.materialIdx];
    float4 albedo = gTextures[mat.albedoHandle].Sample(gSamplers[0], i.uv);
    ...
}
```

**Files:**
- Create: `Renderer/BindlessHeap.h/cpp`
- Modify: `DX12Backend.h/cpp` — remove per-pipeline SRV heap, replace with `BindlessHeap`
- Modify: `DX12Pipeline.cpp` — single global root signature for all pipelines
- Modify: `Graphics/Material.h` — replace SRV slot index with `uint32_t albedoHandle`, etc.

**Complexity:** 2–3 sessions. The heap management is straightforward; the tricky
part is migrating all existing pipelines to the new root signature without regressions.

---

## Phase 11 — GPU-Driven Rendering

**Goal:** Move draw call submission from CPU to GPU. The CPU populates a draw-call buffer
once per frame; a compute shader culls it; `ExecuteIndirect` processes the visible subset.

**Why it matters:** GPU-driven rendering is the state of the art for large scenes
(COD, Fortnite, UE5 Nanite's predecessor concept). It demonstrates mastery of indirect
drawing, GPU compute, and the transition away from CPU bottlenecks.

**Depends on:** Phase 10 (Bindless) — GPU shaders need bindless access to all resources.

### Data layout

```cpp
// Renderer/DrawIndirect.h  (new)
struct DrawCall
{
    D3D12_DRAW_INDEXED_ARGUMENTS args;  // IndexCountPerInstance, InstanceCount, ...
    XMFLOAT4X4 worldMatrix;
    uint32_t   materialIdx;
    uint32_t   meshIdx;            // index into vertex/index buffer table
    uint32_t   _pad[2];
};

struct DrawCallBuffer
{
    static constexpr uint32_t MAX_DRAWS = 65536;

    // GPU-side buffer of all potential draw calls (written by CPU each frame)
    ComPtr<ID3D12Resource> drawCallBuffer;     // UAV — compute writes visible subset here
    ComPtr<ID3D12Resource> visibleDrawBuffer;  // UAV — ExecuteIndirect reads from here
    ComPtr<ID3D12Resource> countBuffer;        // UINT — visible count (UAV)
    D3D12MA::Allocation*   allocs[3] = {};

    uint32_t totalDraws = 0;
};
```

### Culling compute shader

```hlsl
// Shaders/culling.hlsl  — frustum cull per draw call
RWStructuredBuffer<DrawCall> gDrawCalls   : register(u0);
RWStructuredBuffer<DrawCall> gVisible     : register(u1);
RWByteAddressBuffer          gCount       : register(u2);

cbuffer CullConstants : register(b0) {
    float4 frustumPlanes[6];
    uint   totalDraws;
};

[numthreads(64, 1, 1)]
void main(uint id : SV_DispatchThreadID)
{
    if (id >= totalDraws) return;
    DrawCall dc = gDrawCalls[id];
    if (FrustumCull(dc.worldMatrix, frustumPlanes))
    {
        uint insertIdx;
        gCount.InterlockedAdd(0, 1, insertIdx);
        gVisible[insertIdx] = dc;
    }
}
```

### ExecuteIndirect call

```cpp
// Per frame:
// 1. Reset count buffer to 0
// 2. Map CPU draw call buffer, write all draw calls
// 3. Dispatch culling compute (totalDraws / 64 groups)
// 4. Barrier: UAV → INDIRECT_ARGUMENT
// 5. ExecuteIndirect(_commandSignature, MAX_DRAWS, visibleDrawBuffer, 0, countBuffer, 0)
```

`ID3D12CommandSignature` is created once:
```cpp
D3D12_INDIRECT_ARGUMENT_DESC argDesc[2] = {};
argDesc[0].Type                    = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;  // push world matrix
argDesc[0].Constant.RootParameterIndex = 0;
argDesc[0].Constant.Num32BitValuesToSet = sizeof(XMFLOAT4X4) / 4;
argDesc[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
```

**Files:**
- Create: `Renderer/DrawIndirect.h/cpp`
- Create: `Shaders/culling.hlsl`
- Modify: `DX12Backend.cpp::DrawFrame()` — replace `DrawIndexedInstanced` loop with dispatch+ExecuteIndirect

**Complexity:** 3 sessions. The conceptually simple — hard to debug because GPU-side errors
are silent. Use PIX GPU captures heavily during development.

---

## Phase 12 — Multi-threaded Command Recording

**Goal:** Record draw commands on multiple CPU threads simultaneously, then submit
all command lists to the GPU in one batch. Eliminates the single-threaded CPU bottleneck.

**Why it matters:** Every production engine records command lists in parallel.
It requires understanding of thread synchronisation, D3D12 command list lifecycle,
and the distinction between command allocators and command lists.

**Depends on:** Phase 11 (GPU-driven) completes most CPU work; threading is the final push.

### Thread pool

```cpp
// Utils/JobSystem.h  (new)
class JobSystem
{
public:
    explicit JobSystem(uint32_t threadCount = std::thread::hardware_concurrency() - 1);
    ~JobSystem();

    // Submit a job; returns a future to wait on
    std::future<void> Submit(std::function<void()> job);

private:
    std::vector<std::thread>          _workers;
    std::queue<std::function<void()>> _queue;
    std::mutex                        _mutex;
    std::condition_variable           _cv;
    std::atomic<bool>                 _stop{false};
};
```

### Parallel command recording

D3D12 requirement: each thread needs its own `ID3D12CommandAllocator`. A bundle of
allocators is pre-created per frame slot, one per worker thread:

```cpp
// In FrameResource (DX12Backend.h):
static constexpr uint32_t MAX_THREADS = 8;
ComPtr<ID3D12CommandAllocator> workerAllocators[MAX_THREADS];
ComPtr<ID3D12GraphicsCommandList> workerCmdLists[MAX_THREADS];
```

Recording pattern:

```cpp
// BeginFrame() resets all worker allocators

// DrawFrame() — dispatch one job per scene partition
std::vector<std::future<void>> jobs;
for (uint32_t t = 0; t < threadCount; ++t)
{
    jobs.push_back(_jobSystem.Submit([this, t, &partition = _partitions[t]] {
        auto* cmd = _frames[_frameIndex].workerCmdLists[t].Get();
        cmd->Reset(_frames[_frameIndex].workerAllocators[t].Get(), nullptr);
        // ... record draws for this partition ...
        cmd->Close();
    }));
}
for (auto& f : jobs) f.wait();

// EndFrame() — submit all closed command lists in one ExecuteCommandLists call
ID3D12CommandList* lists[MAX_THREADS + 1]; // +1 for main list (ImGui)
for (uint32_t t = 0; t < threadCount; ++t) lists[t] = _workerCmdLists[t].Get();
lists[threadCount] = _commandList.Get();
_commandQueue->ExecuteCommandLists(threadCount + 1, lists);
```

**Files:**
- Create: `Utils/JobSystem.h/cpp`
- Modify: `DX12Backend.h/cpp` — `FrameResource` gets `workerAllocators[]`, `DrawFrame()` dispatches jobs

**Complexity:** 2–3 sessions. Most complexity is in debugging race conditions and
D3D12 validation errors from misused allocators.

---

## Phase 13 — Offline Asset Pipeline

**Goal:** Add an offline tool that converts raw assets (glTF, PNG) into an engine-native
binary format. Runtime loading becomes a memcpy into GPU memory, not a parse.

**Why it matters:** Production renderers never load glTF at runtime. An asset pipeline
is expected knowledge for senior engine roles.

### Binary format (.luna_mesh)

```cpp
// LunaEngine/AssetPipeline/MeshAsset.h  (new)
#pragma pack(push, 1)
struct LunaMeshHeader
{
    uint32_t magic         = 0x4C4D5348; // 'LMSH'
    uint32_t version       = 1;
    uint32_t primitiveCount;
    uint32_t materialCount;
    uint64_t vbOffset;      // byte offset from file start
    uint64_t ibOffset;
    uint64_t materialOffset;
};
#pragma pack(pop)
// Followed by: PBRVertex[] vertices, uint32_t[] indices, LunaMaterial[] materials
```

### Texture compression

Use DirectXTex (already linked) to compress textures to BCn at cook time:

```cpp
// AssetPipeline/TextureCooker.h
// Albedo / roughness maps → BC7   (high quality, variable ratio)
// Normal maps             → BC5   (RG only, lossless for normals)
// Single-channel masks    → BC4   (metallic, AO)
```

Cook tool (standalone exe, separate premake project `LunaCook`):
```
LunaCook.exe input.gltf output_dir/
  → output_dir/helmet.luna_mesh
  → output_dir/albedo_2048.dds
  → output_dir/normal_2048.dds
  → output_dir/metalRough_2048.dds
```

### Hot-reload

Add a file watcher to reload changed assets without restarting:

```cpp
// Utils/FileWatcher.h  (new)
class FileWatcher
{
public:
    using Callback = std::function<void(const std::filesystem::path&)>;
    void Watch(const std::filesystem::path& dir, Callback onChanged);
    void Poll();  // call once per frame from Application::Run()
private:
    HANDLE _hDir = INVALID_HANDLE_VALUE;
    ...
};
```

**Complexity:** 2–3 sessions. The binary format is simple; getting BC compression right
(choosing correct DXGI_FORMAT for sampling, mip generation) is the fiddly part.

---

## Phase 14 — GPU Profiling & Debug Infrastructure

**Goal:** Add per-pass GPU timing, PIX markers, and a real-time performance overlay.
This rounds out the engine to production quality.

**Why it matters:** Profiling is part of the daily workflow at every graphics team.
Showing you instrumented your own engine is a strong portfolio signal.

### GPU timestamp queries

```cpp
// Renderer/GPUProfiler.h  (new)
class GPUProfiler
{
public:
    static constexpr uint32_t MAX_SCOPES = 64;

    bool Init(ID3D12Device*, uint32_t framesInFlight);
    void BeginScope(ID3D12GraphicsCommandList*, const char* name);
    void EndScope(ID3D12GraphicsCommandList*);
    void ResolveTimestamps(ID3D12GraphicsCommandList*);  // end of frame
    void Readback(uint32_t frameIndex);  // after GPU is done with that frame slot

    // Returns ms for a named scope in the previous resolved frame
    float GetScopeMs(const char* name) const;

private:
    // D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2 queries per scope (begin + end)
    ComPtr<ID3D12QueryHeap>  _queryHeap;
    ComPtr<ID3D12Resource>   _readbackBuffer;
    D3D12MA::Allocation*     _readbackAlloc = nullptr;
    uint64_t                 _gpuFrequency  = 0;
    uint32_t                 _nextQuery     = 0;
    struct Scope { const char* name; uint32_t startQuery; float ms; };
    std::vector<Scope>       _scopes;
};
```

PIX markers (zero-cost in retail builds):

```cpp
#ifdef _DEBUG
#include <pix3.h>
#define LUNA_GPU_SCOPE(cmd, name) PIXScopedEvent(cmd, PIX_COLOR_DEFAULT, name)
#else
#define LUNA_GPU_SCOPE(cmd, name)
#endif
```

### ImGui performance overlay

In `ExampleLayer::OnUIRender()`:

```cpp
ImGui::Begin("GPU Profiler");
ImGui::Text("DXR Shadows:   %.2f ms", profiler.GetScopeMs("DXR Shadows"));
ImGui::Text("Geometry Pass: %.2f ms", profiler.GetScopeMs("Geometry Pass"));
ImGui::Text("Lighting Pass: %.2f ms", profiler.GetScopeMs("Lighting Pass"));
ImGui::Text("SSAO:          %.2f ms", profiler.GetScopeMs("SSAO"));
ImGui::Text("Bloom:         %.2f ms", profiler.GetScopeMs("Bloom"));
ImGui::Text("TAA:           %.2f ms", profiler.GetScopeMs("TAA"));
ImGui::Text("Tonemap + UI:  %.2f ms", profiler.GetScopeMs("Tonemap+UI"));
ImGui::Separator();
ImGui::Text("Total GPU:     %.2f ms", profiler.GetScopeMs("Frame"));
ImGui::Text("Frame budget:  %.2f ms (%.0f FPS)",
            1000.f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);
ImGui::End();
```

**Complexity:** 1–2 sessions. The query heap API is straightforward; the readback
timing (two frames delayed) requires careful frame indexing.

---

## Summary Table

| Phase | Topic | Sessions | Depends on | Portfolio Signal |
|-------|-------|----------|-----------|-----------------|
| 5A | API Correctness | 1–2 | — | code hygiene |
| 5B | Full PBR Materials | 1–2 | — | textured scene |
| **6** | **Render Graph** | **3** | 5A | ⭐⭐⭐ architecture |
| 7 | Deferred Rendering | 2 | 6 | ⭐⭐ industry standard |
| 8 | Cascaded Shadow Maps | 2 | 6 | ⭐⭐ lighting quality |
| 9 | Post-Processing | 3–4 | 6,7 | ⭐⭐ visual quality |
| **10** | **Bindless Resources** | **2–3** | 5A | ⭐⭐⭐ D3D12 mastery |
| **11** | **GPU-Driven Rendering** | **3** | 10 | ⭐⭐⭐ modern GPU usage |
| 12 | Multi-threaded Recording | 2–3 | 11 | ⭐⭐ CPU efficiency |
| 13 | Asset Pipeline | 2–3 | — | ⭐ production process |
| 14 | GPU Profiling | 1–2 | 6 | ⭐⭐ engineering discipline |

### Recommended sequencing for a job search portfolio

If time is limited, prioritise in this order:

```
5A (clean API) → 5B (PBR textures — looks good in screenshots) →
6  (Render Graph — the most important architectural piece) →
10 (Bindless — shows D3D12 depth) →
11 (GPU-Driven — the WOW factor item) →
14 (Profiling — shows engineering maturity)
```

Phases 7, 8, 9 are valuable for visual quality but 6 + 10 + 11 are the ones that
differentiate a senior candidate from a mid-level one in interview discussions.

---

## What the engine looks like when all phases are complete

```
Application::Run()
  ├─ FileWatcher::Poll()          — hot-reload changed assets
  ├─ JobSystem: parallel scene update (transforms, animations)
  ├─ DrawCallBuffer: CPU writes all draw calls (N=thousands)
  │
  ├─ BeginFrame()                 — ring buffer slot, wait fence
  │
  ├─ RenderGraph::Build()
  │   ├─ GeometryPass             — parallel command recording (N threads)
  │   ├─ DXR/CSM ShadowPass       — DXR (Tier 1.0+) or CSM (fallback)
  │   ├─ LightingPass             — deferred PBR (compute fullscreen)
  │   ├─ SSAOPass                 — compute, blur
  │   ├─ BloomPass                — dual Kawase downsample/upsample
  │   ├─ TAAPass                  — temporal resolve + history blend
  │   └─ TonemapPass              — ACES + gamma
  ├─ RenderGraph::Execute()       — auto barriers, transient resources
  │
  ├─ ImGui overlay                — GPU timings, camera, scene stats
  └─ EndFrame()                   — submit N+1 command lists, Present
```

GPU passes per frame: 8–12
Draw calls (GPU-driven): ExecuteIndirect over ~10,000 visible instances
CPU draw call submission time: <0.1 ms (all on GPU)
