# VulkanBackend Subsystem Extraction Plan

**Current State**: 817-line header, ~100 member variables, ~6200-line .cpp
**Target**: 10 focused subsystems with clear ownership and interfaces

---

## Proposed Subsystem Architecture

```
VulkanBackend (coordinator)
├── VulkanCore              # Instance, Device, Surface, Debug
├── VulkanSwapchain         # Swapchain lifecycle + resize
├── VulkanFrameManager      # Per-frame resources, fences, semaphores
├── VulkanGBuffer           # G-buffer images, deferred pipeline
├── VulkanShadows           # CSM + RT shadows
├── VulkanSSAO              # SSAO + blur
├── VulkanGPUDriven         # Merged geometry, compute cull, indirect draw
├── VulkanHiZ               # Hi-Z pyramid + occlusion culling
├── VulkanIBL               # Environment lighting precompute
├── VulkanPostProcess       # TAA, SSR, Bloom, Motion Blur, Tonemap
└── VulkanRayTracing        # BLAS/TLAS, RT pipeline, SBT
```

---

## Subsystem Details

### 1. VulkanCore (~50 variables → ~10)
**Lines**: 126-137
**Owns**:
- `VkInstance`, `VkSurfaceKHR`, `VkDebugUtilsMessengerEXT`
- `VulkanDevice` (unique_ptr)
- `_deviceLost` flag
- `_transferCmdPool`

**Interface**:
```cpp
class VulkanCore {
public:
    bool Init(void* windowHandle);
    void Shutdown();
    VkDevice GetDevice() const;
    VkQueue GetGraphicsQueue() const;
    VkQueue GetComputeQueue() const;
    bool IsDeviceLost() const;
    VkCommandBuffer BeginSingleTimeCommands();
    void EndSingleTimeCommands(VkCommandBuffer cmd);
};
```

### 2. VulkanSwapchain (~15 variables)
**Lines**: 142-160
**Owns**:
- `VkSwapchainKHR`, format, extent
- `_swapchainImages`, `_swapchainImageViews`
- `_imagesInFlight`
- Depth buffer (`_depthImage/Memory/View`)
- `_renderPass`, `_framebuffers`

**Interface**:
```cpp
class VulkanSwapchain {
public:
    bool Create(VulkanCore& core, uint32_t w, uint32_t h);
    void Destroy();
    void Recreate(uint32_t w, uint32_t h);
    VkResult AcquireNextImage(VkSemaphore signal, uint32_t* outIndex);
    VkExtent2D GetExtent() const;
    VkRenderPass GetRenderPass() const;
    VkFramebuffer GetFramebuffer(uint32_t index) const;
    VkImageView GetDepthView() const;
};
```

### 3. VulkanFrameManager (~30 variables)
**Lines**: 166-196
**Owns**:
- `VkFrameResource[FRAMES_IN_FLIGHT]`
- `_frameIndex`, `_imageIndex`, `_frameActive`
- Per-frame MVP/Scene UBOs

**Interface**:
```cpp
class VulkanFrameManager {
public:
    bool Create(VulkanCore& core);
    void Destroy();
    bool BeginFrame(VulkanSwapchain& swapchain);
    void EndFrame(VulkanSwapchain& swapchain, VkQueue queue);
    VkCommandBuffer GetCurrentCmd() const;
    uint32_t GetFrameIndex() const;
    void* GetMVPMapped() const;
    void* GetSceneMapped() const;
};
```

### 4. VulkanGBuffer (~30 variables)
**Lines**: 211-262
**Owns**:
- Albedo/Normal/MetalRough images
- `_gbRenderPass`, `_gbFramebuffer`
- Deferred pipeline + layouts
- Per-frame deferred scene CBs

**Interface**:
```cpp
class VulkanGBuffer {
public:
    bool Create(VulkanCore& core, VkExtent2D extent);
    void Destroy();
    void Resize(VkExtent2D extent);
    VkRenderPass GetRenderPass() const;
    VkFramebuffer GetFramebuffer() const;
    void UpdateDeferredUBO(const DeferredSceneUBO& ubo);
    void DrawDeferredLighting(VkCommandBuffer cmd);
};
```

### 5. VulkanShadows (~25 variables)
**Lines**: 266-281 (CSM) + 758-810 (RT)
**Owns**:
- CSM image, layer views, render pass, pipeline
- `_csmLightVP[4]`, `_csmSplits[4]`

**Interface**:
```cpp
class VulkanShadows {
public:
    bool CreateCSM(VulkanCore& core);
    void DestroyCSM();
    void UpdateCSMMatrices(const XMFLOAT4X4& view, const XMFLOAT4X4& proj);
    void DrawCSMPass(VkCommandBuffer cmd, const std::vector<XMFLOAT4X4>& models);
    VkImageView GetCSMArrayView() const;
    const XMFLOAT4X4* GetLightVPs() const;
    const float* GetCascadeSplits() const;
};
```

### 6. VulkanSSAO (~35 variables)
**Lines**: 284-342
**Owns**:
- SSAO/blur images, noise image
- SSAO kernel constant buffer
- SSAO/blur pipelines and descriptors

**Interface**:
```cpp
class VulkanSSAO {
public:
    bool Create(VulkanCore& core, VkExtent2D extent);
    void Destroy();
    void Resize(VkExtent2D extent);
    void Draw(VkCommandBuffer cmd, VkImageView depth, VkImageView normal);
    void DrawBlur(VkCommandBuffer cmd);
    VkImageView GetBlurredView() const;
};
```

### 7. VulkanGPUDriven (~50 variables)
**Lines**: 346-421
**Owns**:
- Merged VB/IB
- Object data SSBO
- Indirect arg/count buffers
- Compute cull pipeline
- Indirect G-buffer pipeline

**Interface**:
```cpp
class VulkanGPUDriven {
public:
    bool Create(VulkanCore& core);
    void Destroy();
    void BuildMergedGeometry(/* ... */);
    void RecordInstance(const GPUObjectDataVK& data);
    void FlushDraws(VkCommandBuffer cmd, VkRenderPass gbPass, VkFramebuffer gbFB);
    bool IsReady() const;
};
```

### 8. VulkanHiZ (~20 variables)
**Lines**: 449-469
**Owns**:
- Hi-Z image + per-mip views
- Hi-Z generation pipeline
- Hi-Z params UBO

**Interface**:
```cpp
class VulkanHiZ {
public:
    bool Create(VulkanCore& core, VkExtent2D extent);
    void Destroy();
    void Resize(VkExtent2D extent);
    void BuildPyramid(VkCommandBuffer cmd, VkImageView depth);
    VkImageView GetFullView() const;
    uint32_t GetMipCount() const;
    bool IsReady() const;
};
```

### 9. VulkanIBL (~40 variables)
**Lines**: 474-523
**Owns**:
- EnvCubemap, IrrCubemap, PrefilterCubemap, BRDF LUT
- IBL compute pipelines (equirect, irr, prefilter, brdf)
- IBL samplers

**Interface**:
```cpp
class VulkanIBL {
public:
    bool Create(VulkanCore& core);
    void Destroy();
    bool LoadHDREnvironment(const std::string& hdrPath);
    bool IsReady() const;
    VkImageView GetIrradianceView() const;
    VkImageView GetPrefilterView() const;
    VkImageView GetBRDFLUTView() const;
    VkSampler GetIBLSampler() const;
};
```

### 10. VulkanPostProcess (~70 variables)
**Lines**: 593-748
**Owns**:
- HDR image
- TAA history images
- Bloom images
- SSR image
- Motion blur image
- All PP pipelines and descriptors

**Interface**:
```cpp
class VulkanPostProcess {
public:
    bool Create(VulkanCore& core, VkExtent2D extent);
    void Destroy();
    void Resize(VkExtent2D extent);
    void DrawSSR(VkCommandBuffer cmd, /* inputs */);
    void DrawMotionBlur(VkCommandBuffer cmd, /* inputs */);
    void DrawTAA(VkCommandBuffer cmd, /* inputs */);
    void DrawBloom(VkCommandBuffer cmd, /* inputs */);
    void DrawTonemap(VkCommandBuffer cmd, VkFramebuffer target);
};
```

---

## Extraction Order (by dependency)

| Order | Subsystem | Dependencies | Estimated LOC |
|-------|-----------|--------------|---------------|
| 1 | VulkanCore | None | ~200 |
| 2 | VulkanSwapchain | Core | ~300 |
| 3 | VulkanFrameManager | Core | ~250 |
| 4 | VulkanHiZ | Core | ~350 |
| 5 | VulkanSSAO | Core | ~400 |
| 6 | VulkanShadows | Core | ~500 |
| 7 | VulkanIBL | Core | ~600 |
| 8 | VulkanGBuffer | Core, Swapchain | ~450 |
| 9 | VulkanGPUDriven | Core, HiZ | ~700 |
| 10 | VulkanPostProcess | Core, Swapchain | ~900 |

---

## Refactored VulkanBackend (~200 lines)

```cpp
class VulkanBackend : public IRenderBackend {
public:
    // IRenderBackend interface (unchanged)
    
private:
    // Subsystems (owned)
    std::unique_ptr<VulkanCore>        _core;
    std::unique_ptr<VulkanSwapchain>   _swapchain;
    std::unique_ptr<VulkanFrameManager> _frameManager;
    std::unique_ptr<VulkanGBuffer>     _gbuffer;
    std::unique_ptr<VulkanShadows>     _shadows;
    std::unique_ptr<VulkanSSAO>        _ssao;
    std::unique_ptr<VulkanGPUDriven>   _gpuDriven;
    std::unique_ptr<VulkanHiZ>         _hiz;
    std::unique_ptr<VulkanIBL>         _ibl;
    std::unique_ptr<VulkanPostProcess> _postProcess;
    
    // Minimal coordinator state
    std::vector<std::shared_ptr<VkSceneMesh>> _sceneMeshes;
    VulkanGPUProfiler _gpuProfiler;
};
```

---

## Benefits

| Metric | Before | After |
|--------|--------|-------|
| Header lines | 817 | ~200 |
| Member variables | ~100 | ~15 |
| Compilation coupling | All features | Per-subsystem |
| Testability | Monolithic | Per-subsystem |
| Code navigation | Complex | Focused |

---

## Implementation Notes

1. **Start with VulkanCore** — lowest dependency, enables incremental refactoring
2. **Keep VkDevice accessible** — all subsystems need it
3. **Pass VulkanCore& to subsystem Create()** — avoids storing duplicate device pointers
4. **Subsystems should NOT know about each other** — coordinator (VulkanBackend) handles cross-subsystem data flow
5. **Use forward declarations** — avoid circular includes

---

## Status

- [x] VulkanCore ✅ (2026-04-25)
- [x] VulkanSwapchain ✅ (2026-04-25)
- [x] VulkanFrameManager ✅ (2026-04-25)
- [x] VulkanHiZ ✅ (2026-04-25)
- [x] VulkanSSAO ✅ (2026-04-25)
- [x] VulkanShadows ✅ (2026-04-25)
- [x] VulkanIBL ✅ (2026-04-25)
- [x] VulkanGBuffer ✅ (2026-04-25)
- [x] VulkanGPUDriven ✅ (2026-04-25)
- [x] VulkanPostProcess ✅ (2026-04-25)

