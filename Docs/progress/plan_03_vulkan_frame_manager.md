# VulkanFrameManager — Implementation Plan

**Date:** 2026-04-25  
**Status:** 🔄 Planning  
**Estimated Lines:** Header ~100, Impl ~350  

---

## 1. Analysis — Current VulkanBackend Frame Resources

### Members to Extract (lines 166-196)

```cpp
static constexpr uint32_t FRAMES_IN_FLIGHT = 3;

struct VkFrameResource {
    VkCommandPool   cmdPool    = VK_NULL_HANDLE;
    VkCommandBuffer cmdBuffer  = VK_NULL_HANDLE;
    VkFence         fence      = VK_NULL_HANDLE;
    VkSemaphore     imageReady = VK_NULL_HANDLE;
    VkSemaphore     renderDone = VK_NULL_HANDLE;

    // Per-frame MVP UBO
    static constexpr uint32_t MAX_DRAWS = 64;
    VkBuffer        mvpBuffer  = VK_NULL_HANDLE;
    VkDeviceMemory  mvpMemory  = VK_NULL_HANDLE;
    void*           mvpMapped  = nullptr;
    VkDescriptorSet mvpDescSet = VK_NULL_HANDLE;

    // Per-frame scene UBO
    VkBuffer       sceneBuffer = VK_NULL_HANDLE;
    VkDeviceMemory sceneMemory = VK_NULL_HANDLE;
    void*          sceneMapped = nullptr;
};

VkFrameResource _frames[FRAMES_IN_FLIGHT];
uint32_t _frameIndex  = 0;
uint32_t _imageIndex  = 0;
bool     _frameActive = false;
```

---

## 2. Design Decisions

### 2.1 FRAMES_IN_FLIGHT vs Swapchain Image Count

```
FRAMES_IN_FLIGHT = 3  (CPU recording ring buffer)
Swapchain images = 2-3 (GPU presentation queue)

These are INDEPENDENT:
- FRAMES_IN_FLIGHT: How many frames CPU can record ahead
- Swapchain images: How many images GPU can present from

Frame index advances every frame (0→1→2→0→...)
Image index comes from vkAcquireNextImageKHR (unpredictable)
```

### 2.2 Fence Responsibilities

| Fence Type | Owner | Purpose |
|------------|-------|---------|
| Per-frame fence | FrameManager | Wait for this frame's GPU work before reusing command buffer |
| Per-image fence | Swapchain | Wait for image's GPU work before reusing same swapchain image |

### 2.3 Descriptor Set Ownership

**Decision**: Keep MVP/Scene descriptor sets in FrameManager since they're per-frame resources.

**Alternative considered**: Move to separate DescriptorManager
- **Rejected**: Over-engineering for current scope. Per-frame UBOs are tightly coupled to frame lifecycle.

### 2.4 Command Buffer Strategy

**Current**: One command buffer per frame, reset each frame.

**Keep as-is**: Simple and correct. Multi-threaded recording would need per-thread pools, but that's Phase 26+.

---

## 3. Interface Design

```cpp
class VulkanFrameManager {
public:
    struct CreateInfo {
        VulkanCore* core;
        uint32_t    framesInFlight = 3;
    };

    bool Create(const CreateInfo& info);
    void Destroy();

    // Frame lifecycle
    bool BeginFrame();   // Wait fence, reset command buffer, begin recording
    void EndFrame(VkQueue queue, VkSemaphore waitSemaphore, VkSemaphore signalSemaphore);

    // Accessors
    uint32_t GetFrameIndex() const { return _frameIndex; }
    uint32_t GetFramesInFlight() const { return FRAMES_IN_FLIGHT; }
    
    VkCommandBuffer GetCurrentCommandBuffer() const;
    VkFence         GetCurrentFence() const;
    VkSemaphore     GetCurrentImageReadySemaphore() const;
    VkSemaphore     GetCurrentRenderDoneSemaphore() const;

    // Per-frame UBO access
    void* GetMVPMapped() const;
    void* GetSceneMapped() const;
    VkDescriptorSet GetMVPDescriptorSet() const;

    // Advance to next frame
    void AdvanceFrame();

private:
    static constexpr uint32_t FRAMES_IN_FLIGHT = 3;
    static constexpr uint32_t MAX_DRAWS = 64;

    struct FrameResource {
        VkCommandPool   cmdPool   = VK_NULL_HANDLE;
        VkCommandBuffer cmdBuffer = VK_NULL_HANDLE;
        VkFence         fence     = VK_NULL_HANDLE;
        VkSemaphore     imageReady = VK_NULL_HANDLE;
        VkSemaphore     renderDone = VK_NULL_HANDLE;

        // MVP UBO (dynamic, per-draw offset)
        VkBuffer        mvpBuffer = VK_NULL_HANDLE;
        VkDeviceMemory  mvpMemory = VK_NULL_HANDLE;
        void*           mvpMapped = nullptr;

        // Scene UBO (static per-frame)
        VkBuffer        sceneBuffer = VK_NULL_HANDLE;
        VkDeviceMemory  sceneMemory = VK_NULL_HANDLE;
        void*           sceneMapped = nullptr;

        // Descriptor set (MVP dynamic + Scene static)
        VkDescriptorSet descSet = VK_NULL_HANDLE;
    };

    VulkanCore* _core = nullptr;
    FrameResource _frames[FRAMES_IN_FLIGHT];
    uint32_t _frameIndex = 0;
    bool _frameActive = false;

    // Descriptor infrastructure
    VkDescriptorPool      _descPool   = VK_NULL_HANDLE;
    VkDescriptorSetLayout _descLayout = VK_NULL_HANDLE;
};
```

---

## 4. UBO Layout Analysis

### MVP UBO (per-draw, dynamic offset)

```cpp
// Layout: MAX_DRAWS × 256 bytes (256-byte aligned for dynamic UBO)
struct MVPData {
    float model[16];  // 64 bytes
    float view[16];   // 64 bytes
    float proj[16];   // 64 bytes
    float _pad[16];   // 64 bytes (padding to 256)
};
// Total: MAX_DRAWS × 256 = 64 × 256 = 16KB per frame
```

### Scene UBO (per-frame, static offset)

```cpp
struct SceneUBO {
    float eyePosition[3]; float _p0;   // 16 bytes
    float lightDir[3];    float _p1;   // 16 bytes
    float lightColor[3];  float _p2;   // 16 bytes
};
// Total: 256 bytes (aligned)
```

### Descriptor Set Layout

```
binding 0: MVP UBO (UNIFORM_BUFFER_DYNAMIC, VERTEX | FRAGMENT)
binding 1: Scene UBO (UNIFORM_BUFFER, FRAGMENT only)
```

---

## 5. Synchronization Flow

```
Frame N-2: GPU executing
Frame N-1: GPU executing  
Frame N:   CPU recording  ← current

┌──────────────────────────────────────────────────────────────┐
│ BeginFrame(N)                                                │
├──────────────────────────────────────────────────────────────┤
│ 1. vkWaitForFences(_frames[N].fence)  // wait for N-2 done   │
│ 2. vkResetFences(_frames[N].fence)                           │
│ 3. vkResetCommandPool(_frames[N].cmdPool)                    │
│ 4. vkBeginCommandBuffer(_frames[N].cmdBuffer)                │
└──────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────┐
│ EndFrame(N)                                                  │
├──────────────────────────────────────────────────────────────┤
│ 1. vkEndCommandBuffer(_frames[N].cmdBuffer)                  │
│ 2. vkQueueSubmit(..., _frames[N].fence)  // signal fence     │
│    - wait: imageReady semaphore                              │
│    - signal: renderDone semaphore                            │
└──────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────┐
│ AdvanceFrame()                                               │
├──────────────────────────────────────────────────────────────┤
│ _frameIndex = (_frameIndex + 1) % FRAMES_IN_FLIGHT           │
└──────────────────────────────────────────────────────────────┘
```

---

## 6. Error Handling

### VK_ERROR_DEVICE_LOST

```cpp
bool BeginFrame() {
    VkResult waitResult = vkWaitForFences(...);
    if (waitResult == VK_ERROR_DEVICE_LOST) {
        _core->SetDeviceLost();
        return false;
    }
    // ...
}
```

### Fence Timeout

```cpp
// Use UINT64_MAX for infinite wait — in practice, device lost or driver hang
// would trigger TDR before timeout
vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
```

---

## 7. Migration Strategy

### Phase 1: Create standalone VulkanFrameManager
- Implement all methods
- Build verification

### Phase 2: Integrate into VulkanBackend
```cpp
// In VulkanBackend::Init()
_frameManager = std::make_unique<VulkanFrameManager>();
VulkanFrameManager::CreateInfo fmci;
fmci.core = _core.get();
if (!_frameManager->Create(fmci)) return false;

// In BeginFrame()
if (!_frameManager->BeginFrame()) {
    _frameActive = false;
    return;
}
VkCommandBuffer cmd = _frameManager->GetCurrentCommandBuffer();
```

---

## 8. Testing Checklist

- [ ] Fence wait/signal sequence (no validation errors)
- [ ] Command buffer reset/record cycle
- [ ] UBO mapping persistence
- [ ] 3-frame pipeline (no stalls)
- [ ] Device lost handling

---

## 9. Files to Create

| File | Path |
|------|------|
| Header | `Vulkan/Public/VulkanFrameManager.h` |
| Implementation | `Vulkan/Private/VulkanFrameManager.cpp` |
| Documentation | `Docs/progress/subsystem_03_vulkan_frame_manager.md` |

