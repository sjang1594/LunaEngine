# LunaEngine — Portfolio Progress Report

**Author:** Senior Graphics Engineer
**Platform:** Windows 11 · DirectX 12 · Vulkan
**Language:** C++17
**Build system:** Premake5 → MSBuild / Visual Studio 2022
**Last updated:** 2026-04-30

---

## Project Overview

LunaEngine is a dual-backend (DirectX 12 + Vulkan) real-time rendering engine built as a
senior-level portfolio project. The goal is to demonstrate fluency with modern low-level GPU
APIs, driver-level optimization patterns, and production-grade engine architecture — the kind of
work expected from a Senior Graphics / Engine programmer on a AAA or automotive-simulation team.

The project is built entirely from scratch: no engine middleware, no high-level graphics
framework. Every subsystem — swap chain, memory allocation, shader compilation, acceleration
structure management, and now the render graph — is implemented directly against the D3D12 /
Vulkan API surface.

---

## Technology Stack

```
Input layer:      GLFW 3.4  (window, mouse, keyboard callbacks)
UI overlay:       Dear ImGui 1.91+ (DX12 and Vulkan backends)
Math:             DirectXMath (XMMATRIX, XMFLOAT*) + GLM (supplementary)
Memory:           D3D12MA 2.x  — D3D12 Memory Allocator (DEFAULT / UPLOAD / READBACK heaps)
Shader compiler:  DXC 1.8+     — IDxcCompiler3 → SM 6.x DXIL (vs_6_0, ps_6_0, lib_6_5)
Asset loading:    cgltf 1.14   — single-header glTF/GLB parser
Texture loading:  stb_image 2.29
PBR shading:      Cook-Torrance BRDF (GGX NDF, Smith-Schlick G, Schlick F)
Ray tracing:      DXR (D3D12_RAYTRACING_TIER_1_0) — hybrid shadow pass
Render graph:     Phase 6 — data-driven barrier scheduling
```

**DX12 pipeline path (end-to-end):**
```
GLFW window → DX12Device (DXGI adapter selection) → ID3D12CommandQueue
→ IDXGISwapChain4 → D3D12MA allocator
→ DXC-compiled DXIL shaders → ID3D12PipelineState (PSO)
→ cgltf → PBRVertex[] → D3D12MA DEFAULT heap VB/IB
→ stb_image → RGBA8 Texture2D → SRV descriptor heap
→ DXR BLAS/TLAS → ID3D12StateObject (RTPSO) → shader table
→ RenderGraph: barrier scheduling → shadow DispatchRays → G-buffer fill
→ CompositeFrame: G-buffer read → Cook-Torrance deferred lighting → back buffer
→ ImGui overlay → IDXGISwapChain4::Present
```

---

## Phase Completion Table

| Phase | Date       | Description |
|-------|------------|-------------|
| 1     | 2026-04-08 | First DX12 triangle — device wiring, depth buffer, MVP CB, DXC/SM6 |
| 2A    | 2026-04-09 | Frames-in-flight ring buffer — eliminates per-frame GPU stall |
| 2B    | 2026-04-09 | DXC / SM 6.x — replaced FXC with IDxcCompiler3; SM 6.0 shaders |
| 2C    | 2026-04-09 | D3D12MA + DEFAULT heap — vertex buffer moved off UPLOAD heap |
| 3A    | 2026-04-10 | Orbital camera — left-drag yaw/pitch, scroll zoom, GLFW callbacks |
| 3B    | 2026-04-10 | cgltf glTF loader — PBRVertex interleave, D3D12MA staging upload |
| 3C    | 2026-04-10 | Cook-Torrance PBR — GGX D, Smith-Schlick G, Schlick F; normal map TBN |
| 3D    | 2026-04-10 | Texture loading — stb_image → RGBA8 Texture2D → SRV heap |
| 4A    | 2026-04-11 | DXR capability check — graceful degradation to raster-only |
| 4B    | 2026-04-11 | BLAS / TLAS — per-mesh BLASes, single scene TLAS |
| 4C    | 2026-04-11 | DXR pipeline — RTPSO, shader table, shadows.hlsl (SM 6.5) |
| 4D    | 2026-04-11 | DXR hybrid shadow integration — depth SRV → DispatchRays → shadow UAV |
| 5     | 2026-04-12 | Vulkan parity — production frame loop replacing ImGui demo helper |
| 5A    | 2026-04-13 | API correctness — VSync, ALT+ENTER, adapter name, namespace cleanup |
| 5B    | 2026-04-13 | Full PBR material pipeline — RootSignatureLayout::PBR, Material GPU struct |
| 6     | 2026-04-13 | Render graph — data-driven barrier scheduling replaces hardcoded DrawFrame |
| 7     | 2026-04-13 | Deferred rendering — G-buffer (albedo/normal/metalRough) + deferred lighting pass; DXR shadows now affect final pixel colour |
| 8     | 2026-04-14 | Cascaded Shadow Maps (CSM) — 4-cascade 2048×2048 directional shadow; replaces DXR shadow mask at t4; 5-tap PCF; practical split scheme |
| 5C    | 2026-04-15 | Vulkan per-frame SceneUBO — eyePos/lightDir/lightColor split from static MatUBO into per-frame set=0 binding=1; camera-driven specular now correct |
| 9     | 2026-04-16 | SSAO — half-res R8_UNORM raw pass (16-tap hemisphere kernel, view-space TBN, 4×4 noise jitter) + 3×3 box-blur pass; blurred result wired to t5 in deferred lighting (bilinear upscale); DX12 root signatures SSAO + SSAOBlur; resize-safe; ambient term multiplied by AO factor |
| 10    | 2026-04-16 | Post-process stack — R16G16B16A16_FLOAT HDR buffer; TAA (Halton(2,3) jitter on P._31/P._32, depth-based reprojection, YCoCg 3×3 neighbourhood clamp, 64-frame Halton sequence, 8-frame warm-up); bloom (bright-pass soft-knee threshold, separable 5-tap Gaussian H+V, R11G11B10_FLOAT half-res, ping-pong); ACES filmic tone mapping + gamma 2.2; resize-safe; graceful fallback to Phase 9 LDR path on init failure |
| 12    | 2026-04-16 | GPU-driven rendering — merged VB+IB; GPUObjectData SSBO (model, bounding sphere, mesh/material index); GPU frustum cull compute (64 threads/group, 6-plane sphere test, atomic append); ExecuteIndirect with ID3D12CommandSignature (DrawIndexed + 2 root constants: materialIndex + objectIndex); pbr_indirect.vert.hlsl reads model from StructuredBuffer; DX12-only; graceful fallback to Phase 11 per-draw path |
| 13    | 2026-04-16 | Async compute — GPU frustum cull dispatch on dedicated D3D12_COMMAND_LIST_TYPE_COMPUTE queue; per-frame compute allocators + command list; cross-queue ID3D12Fence sync (compute Signal → graphics Wait before ExecuteIndirect); overlaps with CSM/DXR shadow passes; graceful fallback to graphics-queue dispatch |
| 14    | 2026-04-17 | Render graph: DAG cull + transient resource aliasing — reference-count flood-fill backwards from side-effect passes; culled passes skipped at Execute(); CreateTransientTexture() declares graph-owned placed resources; interval-graph colouring assigns overlapping lifetimes to distinct D3D12 aliasing heap slots; greedy sort-by-firstPass algorithm; placed resources created via ID3D12Device::CreatePlacedResource on D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES heaps; aliasing barriers emitted between slot transitions; SideEffect() marker prevents cull of externally-observable passes (GBuffer Fill, SSAO, Deferred Lighting); DX12Backend updated to pass ID3D12Device* to RenderGraph constructor |
| 15B   | 2026-04-17 | Vulkan GPU-driven rendering — merged VB+IB; GPUObjectDataVK SSBO (model, bounding sphere, mesh/material index); GPU frustum cull compute (vkCmdDispatch, 64 threads/group, 6-plane sphere test, atomic append to indirect draw count); vkCmdDrawIndexedIndirectCount; bindless descriptor set (set=1: 3 texture arrays + material SSBO + sampler); pbr_indirect_vk.vert.hlsl reads model from StructuredBuffer[[vk::binding]]; graceful fallback to per-draw path on init failure |
| 15C   | 2026-04-17 | IBL environment lighting (both backends) — equirect→cubemap compute (512², R16G16B16A16_SFLOAT); irradiance convolution (32² × 6-face); prefiltered env map (128² × 5 mip roughness levels); BRDF integration LUT (512×512, RG16F); DX12: GPU compute precompute pipeline (equirect→cube, irr, prefilter, brdfLUT all dispatched on graphics queue); DeferredLightingIBL pipeline + root sig with 3 extra descriptor tables (t6 irr, t7 prefilter, t8 brdfLUT); Vulkan: DeferredLightingIBL VK variant replaces _deferredPipeline; LoadHDREnvironment() exposed on IRenderBackend (both backends); graceful disable on stb_image failure |
| 16A   | 2026-04-18 | Vulkan SSAO — half-res R8_UNORM raw (16-tap hemisphere kernel, view-space TBN, 4×4 noise jitter) + 3×3 box-blur; blurred SSAO wired to deferred lighting binding=5; per-frame scene UBO (set=0) carries projection+invProjection+view; two separate renderpass/framebuffer pairs (raw + blur); resize-safe via DestroySSAOResources/CreateSSAOResources cycle |
| 16B   | 2026-04-18 | DX12 SSR — ray-march compute (ssr.comp.hlsl, 64×1 threads; binary search refinement; 64-step max; metal/roughness mask); SSRConstants CBV (screenSize, nearFar, stepSize, maxSteps, metalCutoff, roughCutoff); result stored in R16G16B16A16_FLOAT UAV; additive blend pass (ssr_blend.frag.hlsl) onto _hdrRT before TAA; SSRCompute + SSRBlend root signatures added to IPipeline.h + DX12Pipeline.cpp; optional (failure falls back to non-SSR path) |
| 16C   | 2026-04-18 | Vulkan SSR — ssr_vk.comp.hlsl identical algorithm; SSR constants UBO per frame; deferred lighting retargeted to _ppRenderPass → _hdrImage (RGBA16F intermediate, full-res); tonemapping_vk_full.frag.hlsl reads hdrImage + ssrImage + bloomBright for combined composite; _deferredHDRFramebuffer + _ppRenderPass pipeline wiring; _vkSSRTonemapPipeline (Phase 16C fallback tonemap until Phase 17 TAA is ready) |
| 17    | 2026-04-18 | Vulkan PP stack parity — TAA (taa.frag.hlsl reused; [[vk::binding]] annotations; Halton jitter; YCoCg neighbourhood clamp; per-frame UBO); bloom (bloom_bright.frag.hlsl + bloom_blur.frag.hlsl; half-res RGBA16F ping-pong; _ppRenderPass); full tonemap (tonemapping_vk_full.frag.hlsl; swapchain _tonemapRenderPass); _vkTAALayout (UBO+3SAMPLED_IMAGE+2SAMPLER); _vkPP1SRVLayout (1SAMPLED_IMAGE+1SAMPLER); _vkPP2SRVLayout (2SAMPLED_IMAGE+1SAMPLER+1SAMPLER); UpdatePPDescriptors() updates TAA history ping-pong each frame; both backends now have full feature parity |
| 18A   | 2026-04-20 | Build verification + progress documentation — environment.hdr placement instructions; progress.md extended with Phases 15B–17 |
| 18B   | 2026-04-20 | Screen-space motion blur (both backends) — motion_blur.frag.hlsl (8-tap exponential weight, velocity from depth+invViewProj/prevViewProj reconstruction); DX12: MotionBlur RSL + root sig; _motionBlurRT (RGBA16F, dedicated 1-slot RTV heap); per-frame MotionBlurConstants CB (256 B, UPLOAD); DrawMotionBlurPass() after SSR, before TAA; DrawTAAPass() reads _motionBlurSRVIndex when MB active; Vulkan: _mbImage (RGBA16F) + _mbFB via _ppRenderPass; per-frame UBOs + descriptor sets; DrawVKMotionBlurPass() before DrawVKTAAPass; TAA binding=1 (currentFrame) wired to _mbView when available |
| 18C   | 2026-04-23 | Vulkan render graph wired into frame loop — VulkanRenderGraph.h/cpp; ImportImage() tracks VkImage state (layout+stage+access); PassBuilder Read/Write/SideEffect/Execute API mirrors DX12 RenderGraph; Compile() backward flood-fill from SideEffect passes culls dead passes; Execute() emits vkCmdPipelineBarrier per transition before each pass; Reset() restores initial image states; **Now wired into VulkanBackend::CompositeFrame()** — replaces 10 manual vkCmdPipelineBarrier calls (RT shadow mask, HDR/SSR compute, TAA/bloom/blur inter-pass execution dependencies); PP helper functions (DrawVKTAAPass, DrawVKBloomBrightPass, DrawVKBloomBlurPass) stripped of trailing barriers; FlushDraws() retains manual buffer barriers (graph is image-only) |
| 18D   | 2026-04-20 | Vulkan ray tracing — VK_KHR_acceleration_structure + VK_KHR_ray_tracing_pipeline + VK_KHR_deferred_host_operations + VK_KHR_buffer_device_address extensions conditionally enabled in VulkanDevice.cpp; VkPhysicalDeviceFeatures2 probe chain (asFeatures+rtFeatures+bdaFeatures) before device creation; graceful retry without RT on failure; _rtSupported flag propagated to VulkanBackend; rt_shadows_vk.rgen/rmiss/rchit.glsl (GLSL 4.60 + GL_EXT_ray_tracing); full BLAS build (per-mesh, VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR) + single-instance TLAS; SBT buffer (host-visible, SHADER_BINDING_TABLE_BIT + SHADER_DEVICE_ADDRESS_BIT) with 3 entries (rgen/miss/chit, handleSizeAligned stride); vkCmdTraceRaysKHR dispatched per frame after SSAO, before deferred lighting; shadow mask (R8_UNORM, GENERAL layout) written by raygen, transitioned to SHADER_READ_ONLY_OPTIMAL for fragment reads at deferred lighting binding=13; rtEnabled UBO flag selects RT shadow vs CSM fallback in deferred_lighting_ibl_vk.frag.glsl |
| 19    | 2026-04-22 | DX12/Vulkan rendering parity bug-fix — 5 sessions of systematic diagnosis and 3 root-cause fixes achieving visual parity between backends (see Bug-Fix Sessions below) |
| 21    | 2026-04-23 | Multi-mesh scene support — glTF node tree traversal (cgltf_node_transform_local) for per-mesh world transforms; texture decode dedup cache (std::unordered_map<cgltf_image*>); Transform::SetWorldMatrix() raw matrix override; SceneManager::SetSceneAsset() configurable asset path; --scene CLI argument; both DX12 and Vulkan backends walk the node hierarchy instead of iterating meshes directly |
| 22    | 2026-04-24 | GPU profiler overlay — per-pass GPU timestamp queries (DX12: ID3D12QueryHeap + ResolveQueryData; Vulkan: vkCmdWriteTimestamp + vkGetQueryPoolResults); named Begin/End regions (GBuffer Fill, GPU Cull, Indirect Draw, CSM Shadows, Hi-Z Build, SSAO, Deferred Lighting, SSR, TAA, Bloom, Tonemap); ImGui overlay window with per-pass ms timings and bar chart; both backends |
| 23    | 2026-04-25 | Hi-Z occlusion culling (both backends) — Hi-Z pyramid generation: blit depth (D32_SFLOAT) → R32_SFLOAT mip 0, then iterative min-downsample via compute (hiz_generate.comp.hlsl / hiz_generate_vk.comp.glsl, 8×8 workgroups, min of 2×2 parent texels); ~11 mip levels for 1080p; per-mip descriptor sets + GENERAL layout storage images; DX12: CreateHiZResources() (committed R32_FLOAT, UAV+SRV per mip, HiZGenerate root sig + PSO), BuildHiZPyramid() (resource barriers + Dispatch per mip), called at end of FlushDraws(); Vulkan: CreateHiZResources() (VkImage R32_SFLOAT, per-mip VkImageView, compute pipeline + descriptor pool), BuildHiZPyramid(cmd) (vkCmdBlitImage depth→mip0, then vkCmdDispatch per mip with inter-mip COMPUTE→COMPUTE barriers), called in FlushDraws() pre-cull AND CompositeFrame() post-draw with depth layout restore barrier (READ_ONLY→ATTACHMENT→TRANSFER_SRC→ATTACHMENT→READ_ONLY); gpu_cull.comp.hlsl / gpu_cull_vk.comp.glsl extended: HiZTestSphere() projects bounding sphere to screen AABB, picks mip where 1 texel covers AABB, samples 4 corners, rejects if object.z > min(occluder depths); enableHiZ push constant / CullConstants flag; _hizReady bool gates activation (first frame = frustum-only); Bug #009: depth layout mismatch fix (see Docs/bug-fix/009) |
| 20    | 2026-04-25 | Vulkan async compute — dedicated compute queue discovery (VK_QUEUE_COMPUTE_BIT without VK_QUEUE_GRAPHICS_BIT, fallback: second queue from graphics family); per-frame VkCommandPool + VkCommandBuffer + VkSemaphore + VkFence for compute-only submissions; DispatchCullAsync() records cull dispatch on compute queue with queue ownership release barriers (srcQueueFamilyIndex=compute, dstQueueFamilyIndex=graphics) for _indirectArgBuffer and _drawCountBuffer; FlushDraws() restructured: async path dispatches cull on compute queue + records acquire barriers on graphics cmd, fallback path keeps single-queue dispatch; EndFrame() chains computeDoneSemaphore into graphics VkSubmitInfo (DRAW_INDIRECT stage wait); same-family handling uses VK_QUEUE_FAMILY_IGNORED; Hi-Z pyramid stays on graphics queue; graceful fallback when async not available |
| 24    | 2026-04-26 | Clustered lighting (both backends) — 16×9×24 view-frustum clusters with logarithmic depth slicing; cluster_assign compute shader (Vulkan GLSL + DX12 HLSL, sphere-AABB light test per cluster, dispatch 16×9×24); GPUPointLight SSBO (32B per light, max 1024, host-visible); ClusterParams UBO (invProj, near/far, screen dims); cluster counts + indices SSBOs (device-local, ~1.7 MB total); lights transformed to view space per frame; render graph compute pass before deferred lighting with COMPUTE→FRAGMENT buffer barriers; deferred_lighting_ibl shaders extended with cluster data bindings for per-pixel cluster index + Cook-Torrance BRDF per point light; SceneConstants/DeferredSceneUBO extended with numPointLights; pipeline layouts expanded; IRenderBackend::SetPointLights() API; ImGui Point Lights editor panel |
| 26    | 2026-04-27 | Vulkan render graph: transient resource aliasing — VulkanRenderGraph extended with CreateTransientImage() API; graph-owned VkImages backed by aliased VkDeviceMemory; Compile() pipeline: DAG reference-count flood-fill cull → transient lifetime analysis (firstPass/lastPass per image) → greedy interval-graph colouring assigns alias slots → per-slot VkAllocateMemory (DEVICE_LOCAL) + VkCreateImage + VkBindImageMemory at offset 0; Execute() emits aliasing barriers (VkImageMemoryBarrier with oldLayout=UNDEFINED) when a new transient takes over a memory slot; FindMemoryType helper queries VkPhysicalDeviceMemoryProperties; memory savings logged at Compile() time; Shutdown() destroys owned VkImages + VkFreeMemory; constructor accepts VkDevice + VkPhysicalDevice (optional, backward-compatible default ctor preserved); CompositeFrame() passes device info; all existing images remain imported (persistent) — aliasing infrastructure ready for migration of short-lived intermediates |
| 28    | 2026-04-27 | Physically-based atmosphere rendering (Hillaire 2020, Vulkan) — 3-LUT pipeline: transmittance (256×64, 64-step ray march, one-time), multi-scattering (32×32, 64 hemisphere dirs × 20 steps + ground bounce, one-time), sky-view (192×108, 32 steps single+multi scatter, per-frame); VulkanAtmosphere subsystem (Create/Update/DrawComposite lifecycle); 6 GLSL shaders (atmosphere_common.glsl shared include, transmittance/multiscatter/skyview compute, composite fragment, fullscreen vertex); Hillaire parameterization: non-linear V (square-root zenith mapping), azimuth [0, 2π]; sun disk with limb-darkening (Schlick approximation); Earth defaults (Rayleigh 5.8/13.6/33.1 × 10⁻⁶ /m, Mie 4.0 × 10⁻⁶ /m, ozone layer, ground radius 6360 km); render graph: Atmo SkyView LUT compute pass (.SideEffect) + Sky Composite graphics pass (.SideEffect, writes hHDR); Bug #015 post-ship fix: replaced _ppRenderPass (DONT_CARE) with owned _atmosphereRenderPass (LOAD_OP_LOAD, initialLayout=SHADER_READ_ONLY_OPTIMAL) — eliminates sceneTex feedback loop; fragment shader now discards scene pixels; fixed first-frame skyView barrier (_precomputed → _skyViewReady flag) |
| 30    | 2026-04-30 | Global Illumination — SSGI + irradiance probes (both backends) — DX12: SSGI compute (ssgi.comp.hlsl, half-res RGBA16F ping-pong, 8 cosine-weighted rays, Hi-Z ray march, temporal accumulation via prevViewProj reprojection); irradiance probe atlas (8×4×8 grid, octahedral encoding, 16×16 per probe, 128×64×8 R16G16B16A16_FLOAT); probe_update.comp.hlsl accumulates cosine-weighted sky samples; deferred_lighting_gi.frag.hlsl (t12=ssgiTex, t13=probeIrrArray, ProbeGridConstants CB) adds ssgiRadiance + probeRadiance × 0.3 to IBL ambient; DeferredLightingGI root sig (12 params); Vulkan: VulkanGI subsystem (Create/Dispatch/Destroy lifecycle, SSGI + probe atlas with USAGE_SAMPLED_BIT, all images stay VK_IMAGE_LAYOUT_GENERAL); CreateGIDeferredResources() (set=3 descriptor layout: binding 0/1 SAMPLED_IMAGE + binding 2 SAMPLER + binding 3 ProbeGridData UBO; 4-set pipeline layout; deferred_lighting_gi_vk.frag.glsl compiled at runtime; per-frame ProbeGridUBO host-mapped); UpdateGIDescriptorSet() writes ssgiReadView + probeIrrView with GENERAL layout; CompositeFrame() GI pipeline selected when _gi.IsReady() + _clusterLightDescSet valid; 4-descriptor-set bind (scene UBO + G-buffer + cluster + GI); DestroyGIDeferredResources() in DestroyDeferredPipeline(); no intra-frame race (ping-pong slots, deferred reads 1-frame-old SSGI slot) |
| 32    | 2026-05-27 | Visibility buffer rendering (both backends) — R32_UINT render target packs (objectIndex<<23|primitiveID) per pixel. DX12: VisibilityBuffer PSO (compatible with Phase 12 ExecuteIndirect command sig; params[0]=ViewProj CBV, params[1]=materialCB, params[2]=materialIndex const, params[3]=objectIndex const, params[4]=GPUObjectData root SRV); visibility.vert.hlsl reads model from GPUObjectData[b3], outputs objectIdx flat; visibility.frag.hlsl writes packed uint via SV_PrimitiveID; ClearUnorderedAccessViewUint with 0xFFFFFFFF sentinel for sky pixels; VisibilityShade compute: params[0]=CBV, params[1]=visBuf SRV table, params[2..5]=mergedVB/IB/objects/meshInfos root SRVs (GPU VAs), params[6]=G-buffer UAV table (u0-u2 consecutive), params[7]=bindless SRV heap; visibility_shade.comp.hlsl reconstructs barycentrics from screen position (perspective-correct via 1/w weighting), interpolates UV/normal/tangent, samples bindless textures, writes G-buffer UAVs; toggle: _visBufferMode flag selects between vis and traditional G-buffer fill in FlushDraws(). Vulkan: visibility_vk.vert.glsl uses gl_InstanceIndex (= firstInstance=objectIndex in VkDrawIndexedIndirectCommand) — reuses _indirectVSLayout and _indirectVSDescSet; _visRenderPass (R32_UINT + depth LOAD_OP_CLEAR); DrawVKVisibilityPass uses vkCmdDrawIndexedIndirectCount with existing cull output (_indirectArgBuffer[_frameIndex]); DispatchVKVisibilityShade writes G-buffer images (GENERAL layout during compute, restored to SHADER_READ_ONLY_OPTIMAL); 4-set shade pipeline: {VisShadeSet0, VisShadeSet1, VisShadeSet2, _indirectMaterialSet}; vis pass inserts in CompositeFrame before render graph (overwrites G-buffer with reconstructed attributes). Build: 0 errors. |
| 31    | 2026-05-26 | Order-independent transparency (WBOIT, both backends) — McGuire & Bavoil 2013 weighted blended OIT. Two render targets: accum (R16G16B16A16_FLOAT, ONE+ONE additive blend) + revealage (R8_UNORM, ZERO+SRC_COLOR multiplicative blend). OIT forward pass: depth read-only (LOAD_OP_LOAD, DEPTH_STENCIL_READ_ONLY_OPTIMAL), fragment writes w(z)·color to accum + w(z) to revealage; weight function w(z) = clamp(0.03/(1e-5+z^4), 1e-2, 3e3) balances near/far objects. OIT composite pass: fullscreen resolve — solved_color = accum.rgb / clamp(accum.a, 1e-4, 5e4), alpha = revealage.r (1-∏(1-αᵢ) accumulated), blended onto HDR RT with SRC_ALPHA/ONE_MINUS_SRC_ALPHA. Vulkan: independentBlend device feature required (VK_TRUE in VulkanDevice.cpp); CreateVKOITResources() full lifecycle; render graph integration Pass 4.9 ("OIT Forward" + "OIT Composite") after vol fog, before SSR; render graph auto-transitions depth to DEPTH_STENCIL_READ_ONLY_OPTIMAL. IRenderBackend::DrawMeshOIT() API; _vkOitMeshes list cleared after composite. Bug fixed (2026-05-26): VkPhysicalDeviceFeatures::independentBlend not enabled caused vkCreateGraphicsPipelines failure → device lost cascade. |
| 29    | 2026-04-29 | Volumetric lighting / fog (froxel-based, both backends) — froxel grid 160×90×64, exponential depth, RGBA16F 3D textures (~7 MB each); material injection compute (vol_inject.comp.hlsl / vol_inject_vk.comp.glsl, 8×8×4 threads, height-based exponential fog density × scattering coefficient per froxel); scattering accumulation compute (vol_scatter.comp.hlsl / vol_scatter_vk.comp.glsl, 8×8×1, Beer-Lambert transmittance + Henyey-Greenstein phase function, CSM shadow lookup per froxel via SampleCmpLevelZero); apply pass (vol_apply.frag.hlsl / vol_apply_vk.frag.glsl, fullscreen additive blend onto HDR RT, froxel UVW from log-depth mapping); DX12: CreateVolumetricFogResources() (3D R16G16B16A16_FLOAT UAV+SRV per volume, non-vis UAV heap for ClearUnorderedAccessViewFloat, per-frame 512B params CB, VolInject/VolScatter/VolApply root signatures + PSOs); DispatchVolumetricFog() (clear inject → inject dispatch → UAV barrier → inject UAV→SRV transition → clear accum → CSM→PSR → scatter dispatch); DrawVolumetricFogApply() (depth→PSR, bind apply PSO, fullscreen tri, restore states); Vulkan: VulkanVolumetricFog subsystem (Create/Destroy/Dispatch/DrawApply lifecycle, per-frame UBOs, 3 descriptor set layouts, per-pool allocation, VkFramebuffer on ppRenderPass, additive blend VkPipeline); render graph: "Vol Fog Compute" + "Vol Fog Apply" passes (SideEffect) after atmosphere, before SSR; IRenderBackend::SetVolumetricFogParams() virtual API; ImGui "Volumetric Fog" panel (density, height falloff, base height, scattering, extinction, phase G sliders) |

---

## Architecture Diagram

```
LunaApp/
└── LunaApp.cpp            CreateApplication() → ExampleLayer
                           ExampleLayer::OnUIRender() — ImGui windows

LunaEngine/
├── Application/
│   └── Application.cpp    Run() loop:
│                            UpdateMVP → BeginFrame → DrawFrame [G-buffer fill]
│                            → SceneManager::Update [DrawMesh → gbufferPipeline]
│                            → CompositeFrame [deferred lighting → back buffer]
│                            → StartImGui → OnUIRender → RenderImGui
│                            → EndFrame
│
├── Components/
│   ├── Transform           GetWorldMatrix() — SRT via XMMatrix
│   ├── MeshRenderer        Render() → IRenderContext::DrawMesh()
│   └── GameObject          component array, Update() dispatch
│
├── Manager/
│   └── SceneManager        LoadScene → LoadTestScene → LoadMeshes
│                           Update() → Scene → GameObject → MeshRenderer
│
├── Renderer/
│   ├── Camera              orbital yaw/pitch/radius, GLFW callbacks
│   ├── Mesh                PBRVertex + D3D12MA VB/IB handles
│   ├── MeshLoader          cgltf → PBRVertex[] → D3D12MA DEFAULT heap
│   ├── RenderGraph  ★NEW   ImportTexture / AddPass / Compile / Execute
│   │
│   ├── HAL/
│   │   ├── IRenderBackend  BeginFrame / DrawFrame / FlushDraws / EndFrame / DrawMesh
│   │   ├── IRenderContext  singleton wrapper; static dispatch to backend
│   │   └── IRenderDevice   Init / Shutdown
│   │
│   ├── DX12/
│   │   ├── DX12Backend     full DX12 implementation; uses RenderGraph + GPU-driven indirect
│   │   ├── DX12Device      adapter selection, DXR capability query
│   │   ├── DX12Pipeline    PSO creation via DXC; vertex layout switch
│   │   ├── DX12Buffer      D3D12MA-backed IBuffer
│   │   ├── DX12AccelStructure  BLAS / TLAS build
│   │   └── DX12RTPipeline  RTPSO, shader table, DispatchShadows()
│   │
│   └── Vulkan/
│       ├── VulkanBackend   production frame loop (BeginFrame/EndFrame)
│       └── VulkanDevice    instance + device creation
│
├── Graphics/
│   ├── IPipeline / IBuffer abstraction interfaces
│   ├── Material            PBR material GPU struct + load helpers
│   └── Texture             stb_image → DEFAULT heap Texture2D + SRV
│
└── Shaders/
    ├── constantbuffer.vert/frag.hlsl     triangle fallback (SM 6.0)
    ├── pbr.vert.hlsl                     PBR vertex shader (used by G-buffer pipeline)
    ├── gbuffer.frag.hlsl          ★NEW   G-buffer fill — albedo/normal/metalRough 3 MRTs
    ├── fullscreen.vert.hlsl       ★NEW   SV_VertexID covering triangle (no VB)
    ├── deferred_lighting.frag.hlsl★NEW   Cook-Torrance BRDF + CSM shadow (Texture2DArray + PCF)
    ├── csm_depth.vert.hlsl        ★NEW   depth-only VS for CSM shadow map renders
    ├── gpu_cull.comp.hlsl         ★NEW   GPU frustum cull compute (Phase 12)
    ├── pbr_indirect.vert.hlsl     ★NEW   indirect PBR VS — model from SSBO (Phase 12)
    ├── shadows.hlsl                      DXR RayGen/Miss/HitGroup (SM 6.5)
    └── mesh_preview.vert/frag.hlsl       normal-diffuse preview (SM 6.0)
```

---

## Render Graph Design (Phase 6)

### Motivation

Before Phase 6, `DX12Backend::DrawFrame()` contained ~50 lines of hardcoded
`ID3D12Resource_Barrier` calls interspersed with pass logic. Every time a new pass was
added, the developer had to manually reason about all preceding and following resource states.
This is the pattern that caused the shadow-UAV restore bug: the state after the DXR pass had
to match the state expected at the start of the next frame.

Phase 6 replaces the hardcoded sequence with a data-driven `RenderGraph` that:
- Tracks each resource's current GPU state
- Emits transition barriers automatically before each pass
- Restores imported resources to their declared `finalState` after the last pass

### API

```cpp
RenderGraph rg(commandList);

// Import persistent resources with current and desired final states
auto bbHandle  = rg.ImportTexture("BackBuffer", resource,
                                   D3D12_RESOURCE_STATE_PRESENT,
                                   D3D12_RESOURCE_STATE_RENDER_TARGET);  // leave in RT

auto depthHdl  = rg.ImportTexture("Depth", resource,
                                   D3D12_RESOURCE_STATE_DEPTH_WRITE);    // auto-restore

auto shadowHdl = rg.ImportTexture("ShadowMap", resource,
                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS); // auto-restore

// Declare passes with their resource accesses
rg.AddPass("DXR Shadows")
  .Read (depthHdl,  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
  .Write(shadowHdl, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
  .Execute([](ID3D12GraphicsCommandList* cmd) { /* DispatchRays */ });

rg.AddPass("PBR Forward")
  .Write(bbHandle,  D3D12_RESOURCE_STATE_RENDER_TARGET)
  .Write(depthHdl,  D3D12_RESOURCE_STATE_DEPTH_WRITE)
  .Read (shadowHdl, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
  .Execute([](ID3D12GraphicsCommandList* cmd) { /* raster */ });

rg.Compile();  // barrier analysis
rg.Execute();  // inject barriers + run lambdas
```

### Compile() Algorithm

```
For each PassNode P in declaration order:
  For each access A in (P.reads ∪ P.writes):
    if resources[A.handle].currentState != A.state:
      P.preBarriers += Transition(resource, currentState → A.state)
      resources[A.handle].currentState = A.state

For each ResourceNode R:
  if R.currentState != R.finalState:
    _finalBarriers += Transition(resource, currentState → finalState)
```

### Barrier trace for Phase 6 DrawFrame

| Resource   | Import state | After DXR pass               | After PBR pass     | finalBarriers      |
|------------|-------------|------------------------------|--------------------|--------------------|
| BackBuffer | PRESENT      | PRESENT (not accessed)       | RENDER_TARGET      | none (RT = final)  |
| Depth      | DEPTH_WRITE  | NON_PIXEL_SHADER_RESOURCE    | DEPTH_WRITE        | none (DW = final)  |
| ShadowMap  | UAV          | UAV (write = no-op barrier)  | PIXEL_SHADER_RES   | UAV restored ✓     |

After `Execute()` the command list is left open with the backbuffer in `RENDER_TARGET` state
and the RTV+DSV bound — exactly the state that `DrawMesh()` and ImGui need before `EndFrame()`
issues the final `RENDER_TARGET → PRESENT` barrier.

### Scope (Phase 6 vs future)

| Feature                            | Phase 6 | Phase 7+ |
|------------------------------------|---------|----------|
| Imported (persistent) resources    | ✓       | ✓        |
| Transient resource allocation      | –       | ✓ (G-buffer, SSAO) |
| Linear execution order             | ✓       | ✓        |
| DAG culling (skip unused passes)   | –       | ✓        |
| Automatic pass reordering          | –       | ✓        |
| Barrier batching across passes     | ✓       | ✓        |

---

## Key Design Decisions

### Frames-in-flight ring buffer (Phase 2A)
The single command allocator design causes the CPU to stall until the GPU finishes the previous
frame before it can record the next one. Moving to two allocator slots (one per in-flight frame)
lets the CPU record frame N+1 while the GPU is executing frame N — eliminating the stall entirely
at the cost of doubling the per-frame GPU resource footprint (constant buffers, etc.).

### D3D12MA over raw CreateCommittedResource (Phase 2C)
`ID3D12Device::CreateCommittedResource` allocates one VkDeviceMemory / D3D12 heap per resource.
On scenes with thousands of meshes this hits OS limits. D3D12MA pools allocations into large
heaps and sub-allocates, matching what Unreal Engine and Unity do internally. It also provides
`ALLOCATION_DESC` for fine-grained placement (DEFAULT / UPLOAD / READBACK) and a leak-detection
report in debug builds.

### Render graph before G-buffer and deferred (Phase 6 before Phase 7)
Introducing the render graph while the engine still has a simple two-pass frame (DXR shadows +
PBR forward) keeps the verification surface small — it is easy to reason about which barriers
the graph should emit and confirm they match the previous hardcoded sequence. Adding G-buffer
passes, SSAO, and screen-space reflections on top of a solid barrier-scheduling foundation is
far safer than retrofitting the graph onto an already-complex frame.

### DXR hybrid shadows vs full path tracing
A full path tracer is bandwidth- and compute-constrained in ways that obscure graphics
engineering skill behind GPU power. A hybrid approach — rasterize the G-buffer, use DXR only
for shadows — demonstrates the architectural knowledge (BLAS/TLAS, RTPSO, shader tables, state
object compilation) while keeping the overall frame time predictable on mid-range hardware.

---

## Phase 7 — Deferred Rendering (G-buffer + Deferred Lighting)

### G-buffer Layout

| Slot  | Format              | Contents                           |
|-------|---------------------|------------------------------------|
| RT0   | R8G8B8A8_UNORM      | albedo.rgb (sRGB)                  |
| RT1   | R16G16B16A16_FLOAT  | world-space normal.xyz (encoded)   |
| RT2   | R8G8B8A8_UNORM      | metallic.r + roughness.g           |
| DSV   | R32_TYPELESS        | depth (DSV=D32_FLOAT, SRV=R32_FLOAT) |
| UAV   | R32_FLOAT           | DXR shadow mask (unchanged)        |

### Frame Execution Order (Phase 7)

```
UpdateMVP()          ← fills MVPConstants + SceneConstants (invVP, eyePos, lightDir)
BeginFrame()         ← reset + open command list
DrawFrame()          ← RG1: DXR Shadows + GBuffer Fill (clears GB0/1/2, binds 3 RTVs+depth)
SceneManager::Update ← DrawMesh() records geometry to the 3 G-buffer RTVs via _gbufferPipeline
CompositeFrame()     ← RG2: GB0/1/2→PSR; fullscreen Cook-Torrance → back buffer
RenderImGui()        ← unchanged; back buffer stays in RENDER_TARGET
EndFrame()           ← RENDER_TARGET→PRESENT + submit + present
```

### New Files

| File | Role |
|------|------|
| `Shaders/gbuffer.frag.hlsl` | G-buffer fill — samples material textures, writes albedo/normal/metalRough to 3 MRTs |
| `Shaders/fullscreen.vert.hlsl` | SV_VertexID covering triangle — no vertex buffer |
| `Shaders/deferred_lighting.frag.hlsl` | Cook-Torrance BRDF + DXR shadow read — first time shadows affect output |

### Key Technical Points

- **DXR shadows live**: `shadow = shadowMap.Sample(...)` in `deferred_lighting.frag.hlsl` — previously hardcoded to 1.0
- **Consecutive SRV slots**: GB0, GB1, GB2, Depth, Shadow allocated sequentially in `CreateGBuffer()` enabling a single 5-element descriptor table
- **R32_TYPELESS depth buffer**: same resource bound as DSV (D32_FLOAT) for rasterisation and as SRV (R32_FLOAT) for depth reconstruction
- **SceneConstants CB**: per-frame invVP + eye position + light dir/colour for deferred lighting pass
- **DeferredLighting root sig**: no input assembler layout flag; s0=point-clamp (no blurring of G-buffer reads)

---

## Phase 8 — Cascaded Shadow Maps (CSM)

### Shadow Map Layout

| Resource | Format | Size | Description |
|----------|--------|------|-------------|
| `_csmShadowMap` | R32_TYPELESS (Texture2DArray) | 2048×2048 × 4 | DSV=D32_FLOAT, SRV=R32_FLOAT |
| `_csmDsvHeap` | DSV heap | 4 slots | One per-slice DSV for depth writes |
| `_csmSRVIndex` | SRV slot | 1 slot | Texture2DArray SRV for debugging |
| `_shadowSRVIndex` (repurposed) | SRV slot | slot t4 | Texture2DArray SRV for lighting pass |

### Frame Execution Order (Phase 8)

```
UpdateMVP()          ← fills SceneConstants: viewMatrix + lightVP[4] + cascadeSplits
BeginFrame()         ← reset command list; _lastMeshModels.clear()
DrawFrame()          ← RG1: CSM Shadow (depth pre-pass) → DXR Shadows → GBuffer Fill
SceneManager::Update ← DrawMesh() → _gbufferPipeline + caches model in _lastMeshModels
CompositeFrame()     ← RG2: CSMShadow→PSR; GB0/1/2→PSR; lighting → back buffer
RenderImGui()        ← unchanged
EndFrame()           ← RENDER_TARGET→PRESENT + submit + present
```

### Cascade Split Scheme

Practical split: λ=0.5 blend of logarithmic and uniform distributions.

```
farZ = 100m, nearZ = 0.1m, 4 cascades:
  C0: ~0.1–0.74m   (inner, highest resolution)
  C1: ~0.74–5.4m
  C2: ~5.4–31.6m
  C3: ~31.6–100m   (outer, lowest resolution)
```

### Key Technical Points

- **Inline root constants**: CSM pipeline uses `InitAsConstants(16, b0)` — 16 DWORDs per draw call (one 4×4 MVP matrix). Avoids sharing conflict with the regular MVP CB written in the same command list
- **4 cascade draw calls**: one `OMSetRenderTargets` + clear + all-mesh draw loop per cascade slice
- **Front-face culling + depth bias**: `CullMode=FRONT`, `DepthBias=100`, `SlopeScaledDepthBias=2.0` in CSM PSO eliminates self-shadowing acne
- **One-frame model lag**: `_lastMeshModels` caches model matrices from the previous frame. CSM depth pre-pass runs before DrawMesh(), so it uses the previous frame's transforms. Acceptable for directional shadows
- **Slot t4 repurposed**: `_shadowSRVIndex` (the 5th slot in the lighting descriptor table) is overwritten by `CreateCSMResources()` to point at the CSM Texture2DArray SRV instead of the DXR shadow UAV. No root signature changes needed
- **5-tap PCF**: deferred lighting shader samples 4 corners + centre of each shadow texel, averages → soft shadow edge
- **View-space Z cascade selection**: `viewMatrix` added to SceneConstants; shader reads `posVS.z` for cascade selection (positive Z = depth into scene, LH convention)

### New Files

| File | Role |
|------|------|
| `Shaders/csm_depth.vert.hlsl` | Depth-only VS — reads `b0` as 16 inline root constants (lightMVP), outputs clip-space position |
| `Shaders/deferred_lighting.frag.hlsl` (updated) | CSM shadow read via `SampleCSMShadow()` — 5-tap PCF on `Texture2DArray` |

---

---

## Phase 11 — Bindless Textures

### Motivation

Before Phase 11, every `DrawMesh()` call issued a `SetGraphicsRootDescriptorTable` to re-point
the GPU at the 3-slot descriptor range for that mesh's material (albedo, normal, metalRough).
With hundreds or thousands of meshes this creates a per-draw root-parameter write and forces the
driver to validate that the described range is within the currently-bound heap.

Bindless eliminates the per-draw table switch: a single **unbounded** descriptor range (spanning
the entire `_imGuiSrvHeap`) is bound once, and the pixel shader indexes into it dynamically using
a **material index** delivered as a 1-DWORD root constant.

### Root Signature Change (PBR layout)

| Param   | Before (Phase 5B)            | After (Phase 11)                                   |
|---------|------------------------------|----------------------------------------------------|
| params[0] | CBV b0: MVP               | CBV b0: MVP (unchanged)                            |
| params[1] | CBV b1: MaterialConstants | CBV b1: MaterialConstants (unchanged)              |
| params[2] | DescTable: 3 SRVs t0-t2   | **Root constant b2: materialIndex (1 DWORD)**      |
| params[3] | —                         | **DescTable: unbounded SRVs t0+, space1**          |
| sampler   | s0 anisotropic wrap        | s0 anisotropic wrap (unchanged)                    |

`space1` is chosen for the bindless range to avoid any register clash with the deferred lighting
pass that binds `t0-t5` in `space0` on the same heap.

### CPU-side (DrawMesh)

```cpp
// Old (Phase 5B):
srvBase.ptr = heap_start + material->srvTableStart * stride;
cmd->SetGraphicsRootDescriptorTable(2, srvBase);   // per-draw table pointer

// New (Phase 11):
cmd->SetGraphicsRoot32BitConstant(2, material->srvTableStart, 0);  // push index
cmd->SetGraphicsRootDescriptorTable(3, heap_start);                 // bind once (heap base)
```

In practice `SetGraphicsRootDescriptorTable(3, heap_start)` is called every draw for correctness,
but the GPU only re-validates it when the value actually changes — the driver batches identical
root bindings. A future optimisation can hoist it to once-per-frame via a root sig flag or a
pipeline re-bind check.

### Shader-side (gbuffer.frag.hlsl)

```hlsl
// Old (Phase 7):
Texture2D albedoTex     : register(t0);
Texture2D normalTex     : register(t1);
Texture2D metalRoughTex : register(t2);

// New (Phase 11):
Texture2D<float4> gAllTextures[] : register(t0, space1);   // unbounded
cbuffer MaterialIndex : register(b2) { uint gMaterialIndex; }

// In main():
float3 albedo     = gAllTextures[gMaterialIndex + 0].Sample(s0, uv).rgb;
float3 normal     = gAllTextures[gMaterialIndex + 1].Sample(s0, uv).xyz;
float2 metalRough = gAllTextures[gMaterialIndex + 2].Sample(s0, uv).bg;
```

### Key Technical Points

- **No resource layout changes**: `LoadMeshes()` still allocates 3 consecutive SRV slots per material
  and writes albedo/normal/metalRough descriptors at `srvTableStart`, `srvTableStart+1`,
  `srvTableStart+2`. Only the bind-time mechanism changes.
- **NumDescriptors = UINT_MAX**: declares the unbounded range in the V1.0 root signature.
  V1.1 `DESCRIPTORS_VOLATILE` flag is not needed because descriptors are never mutated
  after root sig binding within a frame.
- **space1 isolation**: the deferred lighting and post-process passes use `t0-t5` in `space0`.
  Placing the bindless array in `space1` means no descriptor range overlap even though both
  root signatures share the same physical `_imGuiSrvHeap`.
- **SM 6.0 compatible**: unbounded arrays with dynamic indexing are supported since SM 5.1.
  No SM 6.6 `ResourceDescriptorHeap` keyword is required, preserving broad hardware compatibility.

---

## Phase 12 — GPU-Driven Rendering (Indirect Draw + GPU Frustum Culling)

### Motivation

Before Phase 12, every `DrawMesh()` call issued per-draw root parameter writes
(`SetGraphicsRootConstantBufferView`, `SetGraphicsRoot32BitConstant`,
`SetGraphicsRootDescriptorTable`) and a `DrawIndexedInstanced`. With N meshes this means
N×5 root-parameter changes and N draw calls recorded on the CPU timeline. On scenes with
hundreds or thousands of objects, CPU command recording becomes the bottleneck — not the GPU.

Phase 12 replaces the per-draw CPU loop with a GPU-driven pipeline:
1. CPU records instances into a flat `GPUObjectData[]` staging list
2. A compute shader frustum-culls all instances on the GPU
3. A single `ExecuteIndirect` replaces all per-mesh draw calls

### Data Structures

| Struct | Size | Purpose |
|--------|------|---------|
| `GPUObjectData` | 96 B | model matrix + bounding sphere (XYZR) + meshIndex + materialIndex |
| `MeshDrawInfo` | 16 B | indexCount + firstIndex + vertexOffset (into merged VB/IB) |
| `IndirectDrawCommand` | 32 B | D3D12_DRAW_INDEXED_ARGUMENTS + materialIndex + objectIndex + pad |

### Merged Geometry

At `LoadMeshes()` time, all mesh vertices and indices are concatenated into a single merged
VB+IB pair. Each mesh's offsets are stored in a `MeshDrawInfo` SSBO uploaded to the GPU.
This eliminates per-draw `IASetVertexBuffers`/`IASetIndexBuffer` calls.

### GPU Frustum Cull Compute Shader (`gpu_cull.comp.hlsl`)

- **Input**: `StructuredBuffer<GPUObjectData>` (all instances) + 6 frustum planes as root constants
- **Output**: `RWStructuredBuffer<IndirectDrawCommand>` + `RWByteAddressBuffer` (atomic draw count)
- **Thread group**: 64 threads, one per instance
- **Test**: 6-plane sphere test — transform bounding sphere centre to world space, scale radius by max model-matrix axis scale, reject if outside any half-space

### Command Signature

```cpp
D3D12_INDIRECT_ARGUMENT_DESC argDescs[3] = {
    { D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT, .Constant = { 2, 0, 1 } }, // b2: materialIndex
    { D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT, .Constant = { 3, 0, 1 } }, // b3: objectIndex
    { D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED },
};
// ByteStride = sizeof(IndirectDrawCommand) = 32
```

Each indirect command sets two root constants (materialIndex for bindless texture lookup,
objectIndex for model-matrix SSBO fetch) then issues `DrawIndexedInstanced`.

### Vertex Shader (`pbr_indirect.vert.hlsl`)

Model matrix is no longer in a per-frame CBV at b0. Instead:
- `b0` = ViewProj CBV (view + proj only, 128 B)
- `b3` = objectIndex (1 DWORD root constant, set by command signature)
- `t0 space0` = `StructuredBuffer<GPUObjectData>` — indexed by objectIndex

### Frame Execution Order (Phase 12)

```
UpdateMVP()           ← cache view/proj
BeginFrame()          ← reset
DrawFrame()           ← RG1: CSM → DXR → GBuffer Fill (clears + binds RTVs)
SceneManager::Update  ← DrawMesh() records GPUObjectData into _cpuInstances[]
FlushDraws()     ★NEW ← upload instances → dispatch gpu_cull.comp → ExecuteIndirect
CompositeFrame()      ← deferred lighting → post-process
RenderImGui()         ← unchanged
EndFrame()            ← present
```

### Key Technical Points

- **Graceful fallback**: if `CreateIndirectResources()` fails, `_gpuDrivenReady` stays false and `DrawMesh()` falls back to the Phase 11 per-draw path — no visual regression
- **Bounding sphere at load time**: computed in `MeshLoader::LoadGLTF()` as centroid + max vertex radius; stored in `Mesh::boundingSphere`
- **Uniform scale assumption**: compute shader extracts max axis scale from model matrix columns for radius scaling; non-uniform scale may over-estimate but never under-cull
- **Readback-based geometry merge**: `BuildMergedGeometry()` reads DEFAULT-heap per-mesh VB/IB back to CPU, concatenates, then re-uploads as a single merged pair. One-time cost at load.
- **SRV heap slot for ClearUAV**: `FlushDraws()` allocates a temporary UAV descriptor in the shader-visible heap + a non-shader-visible copy for `ClearUnorderedAccessViewUint` (required by DX12 API)
- **DX12-only**: Vulkan backend continues to use per-draw path; Vulkan indirect support is a future phase

### New Files

| File | Role |
|------|------|
| `Shaders/gpu_cull.comp.hlsl` | GPU frustum cull compute shader — sphere test, atomic append |
| `Shaders/pbr_indirect.vert.hlsl` | Indirect PBR vertex shader — model from SSBO, ViewProj from CBV |

---

## Phase 14 — Render Graph DAG Cull + Transient Resource Aliasing

### Motivation

Before Phase 14, `RenderGraph::Compile()` performed only barrier scheduling in linear
declaration order: every pass was always executed and every barrier was always emitted.
Two categories of waste existed:

1. **Unnecessary passes** — if a pass wrote to a resource that was never consumed (e.g.
   an optional shadow pass when DXR is unavailable), the pass still ran, wasting command-list
   recording time even though the result was discarded.

2. **Unnecessary memory** — every G-buffer, SSAO, and post-process buffer was a separately
   committed D3D12 resource even though many of these buffers have non-overlapping lifetimes
   and could share the same GPU memory.

Phase 14 addresses both with a proper DAG cull phase and a transient resource aliasing system.

### DAG Cull Algorithm

```
1. For each resource R, count refCount = number of pass.reads that reference R
2. Build lastWriter[R] = the last pass (by declaration index) that writes R
3. Seed live set: all passes with sideEffect = true
4. Flood-fill backward (iterate until stable):
     for each live pass P:
       for each R in P.reads:
         if lastWriter[R] exists and is not live → mark live, changed = true
5. Passes not marked live are culled (skipped in Execute())
```

**SideEffect() passes** are the roots of the live set.  In the current frame:

| Pass               | Why SideEffect                                                         |
|--------------------|------------------------------------------------------------------------|
| GBuffer Fill       | Leaves G-buffer RTVs bound for external `DrawMesh()` calls             |
| SSAO               | Writes `_ssaoRT`/`_ssaoBlurRT` referenced by descriptor table outside graph |
| Deferred Lighting  | Writes final colour to back buffer / HDR RT consumed by post-process   |

CSM and DXR passes are kept because the Deferred Lighting pass reads their outputs — the
flood-fill traces the dependency chain automatically.

### Transient Resource Aliasing

```
CreateTransientTexture(name, desc, initialState) → RGResourceHandle
  │
  ▼
After Compile() / _ComputeTransientLifetimes():
  resource.firstPass = earliest live pass that reads/writes it
  resource.lastPass  = latest  live pass that reads/writes it
  │
  ▼
_AssignAliasingSlots() — greedy interval graph colouring:
  Sort transients by firstPass
  For each transient T:
    if any existing slot S has S.lastPass < T.firstPass → reuse S
    else → open new slot
    T.aliasSlot = S
    S.sizeBytes = max(S.sizeBytes, T.sizeBytes)
  │
  ▼
_CreateTransientResources():
  For each alias slot → CreateHeap(sizeBytes, DEFAULT, ALLOW_ALL_BUFFERS_AND_TEXTURES)
  For each transient  → CreatePlacedResource(heap, offset=0, desc, initialState)
  │
  ▼
Execute():
  Track slotCurrentResident[slot] per pass
  When a pass writes to transient T and slotCurrentResident[T.slot] ≠ T:
    emit ResourceBarrier(Aliasing(oldResident, T))
  slotCurrentResident[T.slot] = T
```

### Memory Savings Example (G-buffer + SSAO + Bloom)

| Resource        | Format                  | Size (1920×1080) | Lifetime  | Alias Slot |
|-----------------|-------------------------|------------------|-----------|------------|
| GBuf0 (albedo)  | R8G8B8A8_UNORM          | 8 MB             | pass 2–5  | 0          |
| GBuf1 (normal)  | R16G16B16A16_FLOAT      | 16 MB            | pass 2–5  | 1          |
| GBuf2 (metalRough)| R8G8B8A8_UNORM        | 8 MB             | pass 2–5  | 2          |
| SSAO raw        | R8_UNORM                | 2 MB (half-res)  | pass 6–7  | 0 ✓ shared |
| Bloom bright    | R11G11B10_FLOAT         | 4 MB (half-res)  | pass 8–10 | 2 ✓ shared |

Slots 0 and 2 each serve two resources with non-overlapping lifetimes → **two heap allocations
eliminated** (committed → placed, same D3D12 heap re-used). With the current persistent-resource
path the G-buffer stays as committed resources; the aliasing infrastructure is ready to be
wired in when those resources are migrated to transient declarations.

### API Changes

```cpp
// RenderGraph constructor — new primary form
RenderGraph(ID3D12Device* device, ID3D12GraphicsCommandList* cmd);

// Legacy form retained for zero-change call sites
RenderGraph(ID3D12GraphicsCommandList* cmd);

// New methods
RGResourceHandle CreateTransientTexture(name, desc, initialState, clearValue = nullptr);
ID3D12Resource*  GetTransientResource(handle) const;   // valid after Compile()
PassBuilder&     SideEffect();                          // fluent; marks pass as uncullable
```

### Key Technical Points

- **`D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES`**: chosen over
  `ALLOW_ONLY_RT_DS_TEXTURES` so a single heap can hold render targets, depth-stencil,
  and shader-visible textures (e.g. a UAV-capable transient compute output) without needing
  separate heap types per slot.
- **Aliasing barrier placement**: emitted immediately before the first `preBarrier` batch of
  the pass that transitions to the new resident, which is the latest valid point per the
  D3D12 spec.
- **Placed resource offset = 0**: all resources in a slot are assigned offset 0.  Because
  they are guaranteed non-overlapping in time (by the interval colouring), they are never
  simultaneously valid — `CreatePlacedResource` allows reuse of the same memory range.
- **`D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS` stripped**: this flag is incompatible
  with `CreatePlacedResource` on DEFAULT heaps without CROSS_ADAPTER; it is removed from
  the desc before placement.
- **Backward compatibility**: existing `RenderGraph(cmd)` call sites continue to work
  unchanged; `_device = nullptr` disables transient creation only.

---

## Phase 22 — GPU Profiler Overlay

**Date:** 2026-04-24

Per-pass GPU timestamp profiler with ImGui overlay for both backends.

### Implementation

- **DX12**: `DX12GPUProfiler` — `ID3D12QueryHeap` (TIMESTAMP type) + `ResolveQueryData` into readback buffer; `InsertBeginTimestamp`/`InsertEndTimestamp` wrap named regions; `CollectTimings` reads resolved timestamps via GPU frequency
- **Vulkan**: `VulkanGPUProfiler` — `VkQueryPool` (TIMESTAMP type) + `vkGetQueryPoolResults`; `WriteBeginTimestamp(cmd, name)`/`WriteEndTimestamp(cmd)` insert `vkCmdWriteTimestamp` commands; per-frame query pool reset via `vkCmdResetQueryPool`
- **ImGui overlay**: `GPUProfilerOverlay` renders a translucent window with per-pass ms timings and horizontal bar chart; colour-coded pass names; total frame GPU time

### Named Regions

GBuffer Fill, GPU Cull, Indirect Draw, CSM Shadows, Hi-Z Build, SSAO, SSAO Blur, Deferred Lighting, RT Shadows, SSR, Motion Blur, TAA, Bloom Bright, Bloom Blur, Tonemap

---

## Phase 23 — Hi-Z Occlusion Culling

**Date:** 2026-04-25

Two-stage GPU culling: frustum rejection (Phase 12) + Hi-Z depth-based occlusion rejection. Objects fully behind existing geometry are eliminated before any draw calls, reducing vertex and fragment workload proportional to scene depth complexity.

### Architecture

```
Per frame:
  1. BuildHiZPyramid (prev-frame depth → mip chain)
  2. GPU Cull Dispatch:
       for each object:
         a. Frustum test (6-plane sphere test) → reject if outside
         b. Hi-Z test (project sphere → AABB → sample pyramid) → reject if occluded
         c. Survivors → atomic append indirect draw command
  3. vkCmdDrawIndexedIndirectCount / ExecuteIndirect
  4. BuildHiZPyramid (current-frame depth → ready for next frame)
```

### Hi-Z Pyramid Generation

- **Input**: Full-res depth buffer (D32_SFLOAT)
- **Mip 0**: Blit/copy depth → R32_SFLOAT (DX12: `CopyTextureRegion`; Vulkan: `vkCmdBlitImage`)
- **Mip 1..N**: Compute shader `hiz_generate.comp.hlsl` / `hiz_generate_vk.comp.glsl`
  - 8×8 workgroups, each thread reads 2×2 texels from parent mip, outputs `min()` (conservative closest surface)
  - Push constants: `srcW, srcH, dstW, dstH`
  - Per-mip descriptor set: source = combined image sampler (mip N-1), dest = storage image (mip N)
  - Inter-mip barrier: `COMPUTE_WRITE → COMPUTE_READ`
- **Mip count**: `ceil(log2(max(width,height))) + 1`, capped at 16 (HIZ_MAX_MIPS)
- **Layout**: `VK_IMAGE_LAYOUT_GENERAL` for storage image writes; DX12 uses `UAV` state

### HiZTestSphere (Cull Shader)

```hlsl
bool HiZTestSphere(float3 centreWS, float radius)
{
    float4 clip = mul(float4(centreWS, 1), viewProj);
    if (clip.w <= 0) return true;  // behind near plane → visible
    float3 ndc = clip.xyz / clip.w;

    // Project sphere to screen AABB
    float2 screenRadius = abs(radius * float2(viewProj[0][0], viewProj[1][1]) / clip.w) * 0.5;
    float2 uv = ndc.xy * 0.5 + 0.5;  uv.y = 1 - uv.y;
    float2 uvMin = saturate(uv - screenRadius);
    float2 uvMax = saturate(uv + screenRadius);

    // Pick mip where 1 texel covers the AABB
    float maxDim = max((uvMax - uvMin) * screenSize);
    uint mip = min(uint(ceil(log2(maxDim))), hizMipCount - 1);

    // Sample 4 AABB corners → take min (closest occluder)
    float occluder = min(min(
        HiZ.SampleLevel(samp, uvMin, mip),
        HiZ.SampleLevel(samp, uvMax, mip)),
      min(
        HiZ.SampleLevel(samp, float2(uvMax.x, uvMin.y), mip),
        HiZ.SampleLevel(samp, float2(uvMin.x, uvMax.y), mip)));

    return ndc.z <= occluder;  // visible if in front of occluder
}
```

### Vulkan Depth Layout Management

`BuildHiZPyramid` requires depth in `ATTACHMENT_OPTIMAL` and leaves it there. Two call sites have different postcondition requirements:

| Call Site | Pre-call Layout | Post-call Need | Extra Barrier |
|-----------|----------------|---------------|---------------|
| `FlushDraws()` pre-cull | `ATTACHMENT_OPTIMAL` (already transitioned) | `ATTACHMENT_OPTIMAL` (re-open G-buffer pass) | None |
| `CompositeFrame()` post-draw | `READ_ONLY_OPTIMAL` (render pass finalLayout) | `READ_ONLY_OPTIMAL` (render graph expects it) | `READ_ONLY→ATTACHMENT` before, `ATTACHMENT→READ_ONLY` after |

See **Bug #009** (`Docs/bug-fix/009_hiz_depth_layout_mismatch.md`) for the crash this caused and the fix.

### Files Added/Modified

| File | Type | Description |
|------|------|-------------|
| `Shaders/hiz_generate.comp.hlsl` | New | DX12 Hi-Z mip downsample compute (SM 6.0) |
| `Shaders/hiz_generate_vk.comp.glsl` | New | Vulkan Hi-Z mip downsample compute (GLSL 4.50) |
| `Shaders/gpu_cull.comp.hlsl` | Modified | Added `HiZTestSphere()`, `enableHiZ` flag, Hi-Z SRV binding |
| `Shaders/gpu_cull_vk.comp.hlsl` | Modified | Same for Vulkan variant |
| `Shaders/gpu_cull_vk.comp.glsl` | Modified | Same for GLSL variant |
| `DX12Backend.cpp` | Modified | `CreateHiZResources()`, `DestroyHiZResources()`, `BuildHiZPyramid()` |
| `DX12Backend.h` | Modified | Hi-Z member variables |
| `VulkanBackend.cpp` | Modified | `CreateHiZResources()`, `DestroyHiZResources()`, `BuildHiZPyramid(cmd)`, CompositeFrame depth restore |
| `VulkanBackend.h` | Modified | Hi-Z member variables |

---

## Bug-Fix Sessions (Phase 19 — Rendering Parity)

Five systematic debugging sessions (documented in `bug-fix/001–005`) resolved all visual
discrepancies between the DX12 and Vulkan backends rendering the DamagedHelmet.glb asset.

### Session Summary

| # | File | Root Cause | Impact |
|---|------|-----------|--------|
| 001 | `taa.frag.hlsl` | `YCoCgToRGB()` had Co/Cg channels swapped — every TAA frame scrambled colour slightly, saturating to wrong palette over ~10 frames | DX12: gold→green, purple tint accumulation |
| 002–003 | Multiple shaders | Systematic elimination of 18+ suspects (SPIR-V decorations, UBO layout, UV reconstruction, front-face culling, struct padding, etc.) — all verified correct | Diagnostic toggles added to 6 shaders |
| 004 | `VulkanBackend.cpp` | `_vkCurJitter` was `{0,0}` and **never assigned** — Vulkan TAA ran without sub-pixel jitter, so it couldn't smooth UV seam discontinuities | Vulkan: diagonal metallic stripes at triangle edges |
| 005 | 3 files | **Three parity bugs** (see below) | DX12 vs Vulkan looked completely different |

### Session 005 — Three Parity Fixes

#### 1. DX12 IBL Shader Never Sampled IBL Textures (CRITICAL)

`deferred_lighting_ibl.frag.hlsl` declared `irrMap` (t6), `prefilterMap` (t7), `brdfLUT` (t8)
and the C++ backend bound them — but the shader never sampled them. It used
`ambient = 0.05 * albedo * ao`. The Vulkan IBL shader did full split-sum IBL.

**Fix**: Added irradiance diffuse + prefiltered specular + BRDF LUT split-sum matching Vulkan.

#### 2. Legacy Vulkan G-Buffer Emissive Leak (HIGH)

`gbuffer_vk.frag.glsl` output `outAlbedo.a = 1.0`. The IBL lighting shader reads emissive from
`gb0.a`, so this produced a **bright red emissive (1,0,0)** on every pixel when IBL was active.

**Fix**: Changed `outAlbedo.a` from `1.0` to `0.0`.

#### 3. Inconsistent `_linearSampler` Creation (MEDIUM)

Three `_linearSampler` creation points in `VulkanBackend.cpp` with different settings — only one
had anisotropic 8x. Whichever ran first (depending on init order) determined texture quality.
Without anisotropic filtering, UV seam artifacts were amplified.

**Fix**: All three creation sites now use `anisotropyEnable=VK_TRUE, maxAnisotropy=8.0, maxLod=0.0`.

### Files Changed (All Bug-Fix Sessions)

| File | Sessions | Changes |
|------|----------|---------|
| `Shaders/taa.frag.hlsl` | 001 | Fixed `YCoCgToRGB()` Co/Cg swap |
| `Shaders/tonemapping.frag.hlsl` | 001 | Hue-preserving ACES (luminance-based) |
| `Shaders/deferred_lighting_ibl.frag.hlsl` | 005 | Added split-sum IBL sampling |
| `Shaders/gbuffer_vk.frag.glsl` | 005 | Fixed emissive alpha leak |
| `Renderer/Vulkan/Private/VulkanBackend.cpp` | 004, 005 | Halton jitter + unjittered VP + consistent sampler |
| `Renderer/Vulkan/Public/VulkanBackend.h` | 004 | Added `_vkUnjitteredVP`, `_vkPrevUnjitteredVP` |
| 6 shader files | 002–003 | Diagnostic `#define DEBUG_*` toggles (all set to 0) |

---

## Phase 25 — Mesh Shaders (DX12 SM 6.5)

**Date:** 2026-04-27

Replace the Phase 12 vertex/index `ExecuteIndirect` pipeline with DX12 mesh shaders. An amplification shader performs per-meshlet frustum culling on the GPU; visible meshlets are dispatched to a mesh shader that reads vertex/index data from structured buffers and outputs triangles directly to the existing G-buffer MRT layout. No input assembler is involved.

### Why Mesh Shaders Matter

The traditional GPU-driven pipeline (Phase 12) still relies on the fixed-function input assembler:

```
CPU: record GPUObjectData[] → GPU compute: frustum cull → ExecuteIndirect
     ↓                                                          ↓
     Upload N objects                                   IA fetches vertices
                                                        from merged VB/IB
```

**Mesh shaders eliminate the input assembler entirely.** The key differences:

| Aspect | Phase 12 (ExecuteIndirect) | Phase 25 (Mesh Shaders) |
|--------|---------------------------|------------------------|
| **Cull granularity** | Per-object (whole mesh) | Per-meshlet (~124 tris) — finer rejection |
| **Vertex fetch** | Fixed-function IA reads VB/IB | Programmable: mesh shader reads `StructuredBuffer<PBRVertex>` |
| **Index format** | 32-bit per index (4 B) | Packed uint8×3 per triangle (3 B) — ~25% index memory savings |
| **Draw call** | `ExecuteIndirect` (CPU-initiated) | `DispatchMesh` (GPU-initiated from AS) |
| **Pipeline** | VS → Rasterizer → PS | AS → MS → Rasterizer → PS |
| **Culling location** | Separate compute dispatch + barrier + indirect | Inline in amplification shader — zero barrier overhead |
| **Thread model** | 1 VS invocation per vertex (IA-driven) | Cooperative threadgroups (128 threads share meshlet work) |

The main win is **meshlet-level culling**: a 15K-triangle mesh (~125 meshlets) can reject individual 124-tri clusters behind walls, instead of the all-or-nothing per-object test. On Sponza, this means rooms behind the camera are rejected at meshlet granularity rather than only at mesh granularity.

### Implementation

1. **Meshlet generation** (CPU, at load time in `BuildMergedGeometry`)
   - Greedy algorithm: walk triangles in order, add to current meshlet until 64-vertex or 124-triangle limit hit
   - Per-meshlet bounding sphere computed from vertex positions (AABB centre + max radius)
   - Triangle indices packed as `uint32`: `idx0 | (idx1 << 8) | (idx2 << 16)` — 3 bytes per tri vs 12 bytes for 3×uint32
   - Output: `Meshlet[]`, `MeshletBounds[]`, `meshletVertices[]` (global VB indices), `meshletTriangles[]` (packed local indices)

2. **Amplification shader** (`meshlet_cull.as.hlsl`, SM 6.5)
   - `[numthreads(32,1,1)]` — one thread per meshlet, one group per 32-meshlet batch
   - Per-meshlet: transform bounding sphere to world space → 6-plane frustum test
   - Wave intrinsics (`WavePrefixCountBits`, `WaveActiveCountBits`) compact visible meshlets
   - `DispatchMesh(visibleCount, 1, 1)` — only visible meshlets proceed

3. **Mesh shader** (`gbuffer_mesh.ms.hlsl`, SM 6.5)
   - `[numthreads(128,1,1)]` with `[outputtopology("triangle")]`
   - Reads `StructuredBuffer<PBRVertex>` via meshlet vertex indices (no IA, no VBV/IBV)
   - Transforms position/normal/tangent identically to `pbr_indirect.vert.hlsl`
   - Outputs match `PSInput` in `gbuffer.frag.hlsl` — pixel shader is unchanged

4. **Pixel shader** (`gbuffer_mesh.frag.hlsl`, SM 6.5)
   - Same G-buffer output as `gbuffer.frag.hlsl`
   - Bindless textures via `gAllTextures[] : register(t0, space1)` + `materialIndex` root constant

5. **Root signature** (`MeshShaderGBuffer`)
   - `b0` = MeshShaderConstants CBV (view/proj + frustum planes + object/meshlet info)
   - `b1` = MaterialConstants CBV, `b2` = materialIndex root constant
   - `t0-t5 space0` = object data, meshlets, bounds, merged vertices, meshlet vertices, meshlet triangles
   - `t0+ space1` = unbounded bindless SRV heap
   - `s0` = anisotropic sampler

6. **PSO creation** via `D3D12_PIPELINE_STATE_STREAM_DESC` + `ID3D12Device2::CreatePipelineState`
   - Pipeline state stream with `CD3DX12_PIPELINE_STATE_STREAM_AS`, `_MS`, `_PS` subobjects
   - No input layout, no VS — mesh shader pipeline is a fundamentally different PSO type

7. **Feature detection** — `D3D12_FEATURE_DATA_D3D12_OPTIONS7::MeshShaderTier >= D3D12_MESH_SHADER_TIER_1`
   - Graceful fallback: if unsupported, `_meshShaderReady = false` → Phase 12 indirect draw path used

### Frame Execution (Mesh Shader Path)

```
FlushDraws():
  for each object in _cpuInstances[]:
    fill MeshShaderConstants (viewProj + frustum + objectIdx + meshletRange)
    SetGraphicsRootCBV(0, meshShaderCB)
    SetGraphicsRootCBV(1, materialCB)
    SetGraphicsRoot32BitConstant(2, materialIndex)
    DispatchMesh(ceil(meshletCount / 32), 1, 1)
      ↓
    AS: 32 threads cull 32 meshlets → compact → DispatchMesh(visibleCount)
      ↓
    MS: 128 threads process 1 meshlet → SetMeshOutputCounts → emit verts + tris
      ↓
    PS: gbuffer_mesh.frag.hlsl → G-buffer MRT (same as Phase 12)
```

### Files Added/Modified

| File | Type | Description |
|------|------|-------------|
| `Renderer/Meshlet.h` | New | `Meshlet`, `MeshletBounds`, `MeshletMeshInfo` structs + `BuildMeshlets()` |
| `Renderer/Meshlet.cpp` | New | Greedy meshlet builder with bounding sphere computation |
| `Shaders/meshlet_cull.as.hlsl` | New | Amplification shader — per-meshlet frustum cull + wave compact |
| `Shaders/gbuffer_mesh.ms.hlsl` | New | Mesh shader — structured buffer vertex fetch + G-buffer output |
| `Shaders/gbuffer_mesh.frag.hlsl` | New | Pixel shader for mesh shader pipeline (bindless textures) |
| `Graphics/IPipeline.h` | Modified | Added `MeshShaderGBuffer` root sig layout, `meshShaderPipeline` flag |
| `DX12/Public/DX12Pipeline.h` | Modified | Added `InitializeMeshShader()`, `CreateMeshShaderPSO()`, AS/MS blobs |
| `DX12/Private/DX12Pipeline.cpp` | Modified | Mesh shader root sig + PSO via pipeline state stream API |
| `DX12/Public/DX12Device.h` | Modified | Added `SupportsMeshShaders()` |
| `DX12/Private/DX12Device.cpp` | Modified | `D3D12_FEATURE_DATA_D3D12_OPTIONS7` mesh shader tier check |
| `DX12/Public/DX12Backend.h` | Modified | Mesh shader members: pipeline, meshlet buffers, per-frame CB |
| `DX12/Private/DX12Backend.cpp` | Modified | Meshlet generation in `BuildMergedGeometry()`, `CreateMeshShaderResources()`, `DispatchMesh` path in `FlushDraws()` |

---

## Remaining Roadmap

| Phase | Priority | Feature | Status |
|-------|----------|---------|--------|
| 9     | ★★★      | SSAO (DX12) | ✅ Done |
| 10    | ★★☆      | Post-process stack (DX12) — TAA, bloom, tonemapping | ✅ Done |
| 11    | ★★☆      | Bindless textures (DX12) | ✅ Done |
| 12    | ★★☆      | GPU-driven rendering (DX12) | ✅ Done |
| 13    | ★★☆      | Async compute (DX12) | ✅ Done |
| 14    | ★★☆      | Render graph: DAG cull + transient aliasing (DX12) | ✅ Done |
| 15B   | ★★★      | Vulkan GPU-driven rendering | ✅ Done |
| 15C   | ★★★      | IBL environment lighting (both backends) | ✅ Done |
| 16A   | ★★☆      | Vulkan SSAO | ✅ Done |
| 16B   | ★★☆      | DX12 SSR | ✅ Done |
| 16C   | ★★☆      | Vulkan SSR | ✅ Done |
| 17    | ★★★      | Vulkan PP stack parity (TAA + bloom + tonemap) | ✅ Done |
| 18A   | ★☆☆      | Build verification + progress docs | ✅ Done |
| 18B   | ★★☆      | Screen-space motion blur (both backends) | ✅ Done |
| 18C   | ★★☆      | Vulkan render graph | ✅ Done (wired into frame loop) |
| 18D   | ★★★      | Vulkan ray tracing (full BLAS/TLAS/SBT + dispatch) | ✅ Done |
| 19    | ★★★      | DX12/Vulkan rendering parity bug-fix | ✅ Done |
| 19B   | ★☆☆      | FPS / frame-time overlay in custom title bar | ✅ Done |
| 21    | ★★★      | Multi-mesh scene (Sponza, Bistro) | ✅ Done |
| 22    | ★★★      | GPU profiler overlay (timestamp queries + ImGui bar chart) | ✅ Done |
| 23    | ★★★      | Hi-Z occlusion culling (2-pass GPU cull) | ✅ Done |
| 20    | ★★☆      | Vulkan async compute | ✅ Done |
| 24    | ★★☆      | Clustered lighting (16×9×24 clusters, compute assign, deferred accumulation) | ✅ Done (both backends) |
| 25    | ★★☆      | Mesh shaders (DX12 SM 6.5 amplification + mesh shader) | ✅ Done |
| 26    | ★☆☆      | Vulkan render graph: transient resource aliasing | ✅ Done |
| 27    | ★★★      | Vulkan mesh shaders (VK_EXT_mesh_shader) | ✅ Done |
| 28    | ★★★      | Atmosphere / sky rendering (Hillaire 2020) | ✅ Done |
| 29    | ★★☆      | Volumetric lighting / fog (froxel-based) | ✅ Done |
| 30    | ★★☆      | Global illumination (SSGI + irradiance probes) | ✅ Done (both backends) |
| 31    | ★★☆      | Order-independent transparency (OIT) | ✅ Done (both backends) |
| 32    | ★★☆      | Visibility buffer rendering | ✅ Done (both backends) |
| 33    | ★☆☆      | Variable rate shading (VRS) | Planned |

### Simulation Track (deferred)

Sensor simulation phases use separate S-series numbering. Work begins after the rendering track is complete.

| Phase | Priority | Feature | Status |
|-------|----------|---------|--------|
| S1    | ★☆☆      | Sensor simulation foundation (data structures, ImGui panels, BEV) | ✅ Done |
| S2    | ★★☆      | Camera sensor offscreen rendering | Deferred |
| S3    | ★★☆      | LiDAR GPU raycasting (DXR RayQuery compute) | Deferred |
| S4    | ★★☆      | Radar scene query + CPU FFT → range-Doppler | Deferred |
| S5    | ★☆☆      | Sensor data export (PNG, PLY/PCD, CSV) | Deferred |

