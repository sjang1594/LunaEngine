# VulkanSwapchain — Implementation Plan

**Date:** 2026-04-25  
**Status:** 🔄 Planning  
**Estimated Lines:** Header ~80, Impl ~400  

---

## 1. Analysis — Current VulkanBackend Swapchain Code

### Members to Extract (lines 142-160)

```cpp
VkSwapchainKHR            _swapchain       = VK_NULL_HANDLE;
VkFormat                  _swapchainFormat = VK_FORMAT_UNDEFINED;
VkExtent2D                _swapchainExtent = {};
std::vector<VkImage>      _swapchainImages;
std::vector<VkImageView>  _swapchainImageViews;
std::vector<VkFence>      _imagesInFlight;  // Per swapchain-image fence tracking

// Depth buffer
VkImage        _depthImage  = VK_NULL_HANDLE;
VkDeviceMemory _depthMemory = VK_NULL_HANDLE;
VkImageView    _depthView   = VK_NULL_HANDLE;

// Render pass + framebuffers
VkRenderPass               _renderPass = VK_NULL_HANDLE;
std::vector<VkFramebuffer> _framebuffers;
```

### Methods to Extract

| Method | Lines | Complexity |
|--------|-------|------------|
| `CreateSwapchain()` | ~80 | Surface capabilities query, format selection, present mode |
| `DestroySwapchain()` | ~15 | Cleanup framebuffers, image views |
| `RecreateSwapchain()` | ~50 | Full destroy-create cycle with PP resources |
| `CreateDepthResources()` | ~10 | D32_SFLOAT image |
| `DestroyDepthResources()` | ~5 | Cleanup |
| `CreateRenderPass()` | ~30 | Color + depth attachments |
| `CreateFramebuffers()` | ~20 | Per-swapchain-image framebuffer |

---

## 2. Design Decisions

### 2.1 Swapchain Image Count vs FRAMES_IN_FLIGHT

**Issue**: Swapchain can return 2 or 3 images depending on driver. `FRAMES_IN_FLIGHT=3` in backend.

**Solution**: 
- `_imagesInFlight` sized to swapchain image count
- Frame resources (command pools, fences) sized to `FRAMES_IN_FLIGHT`
- These are orthogonal — swapchain manages image acquisition, FrameManager manages recording

```
FRAMES_IN_FLIGHT = 3 (recording ring buffer)
Swapchain images = 2-3 (presentation queue)

Frame 0: records to image 0
Frame 1: records to image 1
Frame 2: records to image 2 (or wraps to 0)
```

### 2.2 VSync Mode Selection

**Current**:
```cpp
VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;  // vsync on (always supported)
if (!_vsync) {
    // Try MAILBOX first (triple-buffer), then IMMEDIATE
}
```

**Keep as-is**: Good pattern. FIFO guaranteed, MAILBOX preferred for no-vsync.

### 2.3 Depth Format Selection

**Current**: Hardcoded `VK_FORMAT_D32_SFLOAT`

**Improvement**: Add format fallback chain:
```cpp
const VkFormat candidates[] = {
    VK_FORMAT_D32_SFLOAT,
    VK_FORMAT_D32_SFLOAT_S8_UINT,
    VK_FORMAT_D24_UNORM_S8_UINT
};
for (auto fmt : candidates) {
    VkFormatProperties props;
    vkGetPhysicalDeviceFormatProperties(gpu, fmt, &props);
    if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
        return fmt;
}
```

### 2.4 Render Pass Ownership

**Question**: Should VulkanSwapchain own the final render pass?

**Answer**: Yes. The "present render pass" (swapchain → PRESENT_SRC_KHR) is tightly coupled to swapchain format. G-buffer and PP render passes belong to their respective subsystems.

### 2.5 Framebuffer Dependencies

**Problem**: Framebuffers depend on:
- Swapchain image views (owned by VulkanSwapchain)
- Depth view (owned by VulkanSwapchain)
- Render pass (owned by VulkanSwapchain)

**Solution**: All framebuffer creation stays in VulkanSwapchain.

---

## 3. Interface Design

```cpp
class VulkanSwapchain {
public:
    struct CreateInfo {
        VulkanCore* core;           // required
        VkSurfaceKHR surface;       // required
        uint32_t width, height;     // initial size
        bool vsync = false;
    };

    bool Create(const CreateInfo& info);
    void Destroy();
    
    // Resize handling
    void Resize(uint32_t width, uint32_t height);  // deferred
    bool RecreateIfNeeded();  // called at frame start
    
    // Acquisition
    VkResult AcquireNextImage(VkSemaphore imageReady, uint32_t* outIndex);
    VkResult Present(VkQueue queue, VkSemaphore waitSemaphore);
    
    // Accessors
    VkExtent2D GetExtent() const { return _extent; }
    VkFormat GetFormat() const { return _format; }
    VkRenderPass GetRenderPass() const { return _renderPass; }
    VkFramebuffer GetFramebuffer(uint32_t index) const;
    VkImageView GetDepthView() const { return _depthView; }
    uint32_t GetImageCount() const { return (uint32_t)_images.size(); }
    
    // Image-in-flight fence tracking
    void SetImageFence(uint32_t imageIndex, VkFence fence);
    VkFence GetImageFence(uint32_t imageIndex) const;

private:
    VulkanCore* _core = nullptr;
    VkSurfaceKHR _surface = VK_NULL_HANDLE;
    
    VkSwapchainKHR _swapchain = VK_NULL_HANDLE;
    VkFormat _format = VK_FORMAT_UNDEFINED;
    VkExtent2D _extent = {};
    bool _vsync = false;
    
    std::vector<VkImage> _images;
    std::vector<VkImageView> _imageViews;
    std::vector<VkFence> _imagesInFlight;
    
    // Depth
    VkImage _depthImage = VK_NULL_HANDLE;
    VkDeviceMemory _depthMemory = VK_NULL_HANDLE;
    VkImageView _depthView = VK_NULL_HANDLE;
    
    // Render pass + framebuffers
    VkRenderPass _renderPass = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> _framebuffers;
    
    // Deferred resize
    bool _needsRecreate = false;
    uint32_t _pendingWidth = 0, _pendingHeight = 0;
    
    VkFormat SelectDepthFormat();
    VkPresentModeKHR SelectPresentMode();
    bool CreateDepthResources();
    void DestroyDepthResources();
    bool CreateRenderPass();
    bool CreateFramebuffers();
};
```

---

## 4. Synchronization Analysis

### Swapchain Acquire/Present Flow

```
┌──────────────┐     ┌──────────────┐     ┌──────────────┐
│ AcquireNext  │────►│ GPU Render   │────►│   Present    │
│  (Semaphore) │     │ (wait acq)   │     │ (wait render)│
└──────────────┘     └──────────────┘     └──────────────┘
       │                    │                    │
   imageReady          renderDone             Queue
   (signaled)          (signaled)
```

### Image-in-Flight Fence Pattern

```cpp
// In BeginFrame:
VkFence imageFence = _swapchain->GetImageFence(imageIndex);
if (imageFence != VK_NULL_HANDLE) {
    vkWaitForFences(dev, 1, &imageFence, VK_TRUE, UINT64_MAX);
}
_swapchain->SetImageFence(imageIndex, _frames[frameIndex].fence);
```

**Purpose**: If same swapchain image acquired before previous frame's GPU work completes, must wait.

---

## 5. Error Handling

### VK_ERROR_OUT_OF_DATE_KHR / VK_SUBOPTIMAL_KHR

```cpp
VkResult VulkanSwapchain::AcquireNextImage(...) {
    VkResult result = vkAcquireNextImageKHR(...);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        _needsRecreate = true;
        return result;  // caller should skip frame
    }
    if (result == VK_SUBOPTIMAL_KHR) {
        // Continue but schedule recreate
        _needsRecreate = true;
    }
    return result;
}
```

### Present Errors

```cpp
VkResult VulkanSwapchain::Present(...) {
    VkResult result = vkQueuePresentKHR(...);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        _needsRecreate = true;
    }
    return result;
}
```

---

## 6. Migration Strategy

### Phase 1: Create VulkanSwapchain (standalone, no integration)
- Implement all methods
- Build verification

### Phase 2: Integrate into VulkanBackend
```cpp
// In VulkanBackend::Init()
_swapchain = std::make_unique<VulkanSwapchain>();
VulkanSwapchain::CreateInfo sci;
sci.core = _core.get();
sci.surface = _core->GetSurface();
sci.width = width;
sci.height = height;
sci.vsync = _vsync;
if (!_swapchain->Create(sci)) return false;
```

### Phase 3: Remove deprecated code
- Delete `VulkanBackend::CreateSwapchain()` etc.
- Update all `_swapchainExtent` → `_swapchain->GetExtent()`

---

## 7. Testing Checklist

- [ ] Initial creation
- [ ] VK_PRESENT_MODE_MAILBOX_KHR selection (vsync off)
- [ ] VK_PRESENT_MODE_FIFO_KHR fallback (vsync on)
- [ ] Window resize → deferred recreate
- [ ] Minimize → OUT_OF_DATE handling
- [ ] Image-in-flight fence wait
- [ ] Depth format fallback (test on AMD/Intel)

---

## 8. Files to Create

| File | Path |
|------|------|
| Header | `Vulkan/Public/VulkanSwapchain.h` |
| Implementation | `Vulkan/Private/VulkanSwapchain.cpp` |
| Documentation | `Docs/progress/subsystem_02_vulkan_swapchain.md` |

