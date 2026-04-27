# VulkanSwapchain — Implementation Document

**Date:** 2026-04-25  
**Status:** ✅ Complete  
**Lines:** Header 140, Impl 470  

---

## Overview

VulkanSwapchain은 swapchain lifecycle, depth buffer, present render pass를 관리한다. VulkanCore에 의존하며, 화면 크기 변경 시 안전한 재생성을 제공한다.

---

## Responsibilities

| Responsibility | API |
|----------------|-----|
| Swapchain lifecycle | `Create()`, `Destroy()`, `RecreateIfNeeded()` |
| Image acquisition | `AcquireNextImage()` |
| Present | `Present()` |
| Depth buffer | `GetDepthView()`, `GetDepthFormat()` |
| Render pass | `GetRenderPass()` |
| Framebuffers | `GetFramebuffer(index)` |
| Resize handling | `RequestResize()`, deferred recreate |
| VSync control | `SetVSync()` |

---

## Design Decisions

### 1. Depth Format Fallback Chain

**Problem**: 모든 GPU가 `VK_FORMAT_D32_SFLOAT`을 지원하지 않음 (Intel iGPU 등).

**Solution**: Format candidate chain 검색:
```cpp
const VkFormat candidates[] = {
    VK_FORMAT_D32_SFLOAT,        // 32-bit depth, best precision
    VK_FORMAT_D32_SFLOAT_S8_UINT, // 32-bit depth + 8-bit stencil
    VK_FORMAT_D24_UNORM_S8_UINT   // 24-bit depth + 8-bit stencil (widely supported)
};
```

### 2. Present Mode Selection

| Mode | Behavior | When Used |
|------|----------|-----------|
| `FIFO_KHR` | VSync on, guaranteed | `_vsync = true` |
| `MAILBOX_KHR` | Triple-buffer, no tear | `_vsync = false`, preferred |
| `IMMEDIATE_KHR` | May tear, lowest latency | `_vsync = false`, fallback |

```cpp
VkPresentModeKHR SelectPresentMode() {
    if (_vsync) return VK_PRESENT_MODE_FIFO_KHR;
    // Try MAILBOX first, then IMMEDIATE
}
```

### 3. Deferred Resize Pattern

**Problem**: `glfwSetFramebufferSizeCallback`은 `glfwPollEvents()` 내에서 호출됨. 즉시 `RecreateSwapchain()` 호출 시 command recording 중에 framebuffer 파괴 → crash.

**Solution**: 
```cpp
void RequestResize(w, h) {
    _pendingWidth = w;
    _pendingHeight = h;
    _needsRecreate = true;  // flag만 설정
}

// BeginFrame() 시작점에서:
if (!_swapchain->RecreateIfNeeded()) return;  // 안전하게 재생성
```

### 4. Image-in-Flight Fence Tracking

**Problem**: Swapchain image가 이전 프레임의 GPU 작업이 완료되기 전에 재사용될 수 있음.

**Solution**: Per-image fence tracking:
```cpp
// AcquireNextImage 후:
VkFence inFlight = _swapchain->GetImageFence(imageIndex);
if (inFlight != VK_NULL_HANDLE) {
    vkWaitForFences(dev, 1, &inFlight, VK_TRUE, UINT64_MAX);
}
// 이 프레임의 fence 기록:
_swapchain->SetImageFence(imageIndex, _frames[frameIndex].fence);
```

### 5. OUT_OF_DATE / SUBOPTIMAL Handling

```
┌─────────────────────────────────────────────────────────┐
│ AcquireNextImage                                         │
├─────────────────────────────────────────────────────────┤
│ VK_SUCCESS           → continue                          │
│ VK_SUBOPTIMAL_KHR    → continue + schedule recreate      │
│ VK_ERROR_OUT_OF_DATE → skip frame + schedule recreate    │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│ Present                                                  │
├─────────────────────────────────────────────────────────┤
│ VK_SUCCESS           → done                              │
│ VK_SUBOPTIMAL_KHR    → schedule recreate for next frame  │
│ VK_ERROR_OUT_OF_DATE → schedule recreate for next frame  │
└─────────────────────────────────────────────────────────┘
```

---

## Memory Layout

```
VulkanSwapchain (estimated ~200 bytes + vectors)
├── VulkanCore*          _core               (8B pointer)
├── VkSurfaceKHR         _surface            (8B handle)
├── VkSwapchainKHR       _swapchain          (8B handle)
├── VkFormat             _format             (4B enum)
├── VkExtent2D           _extent             (8B)
├── bool                 _vsync              (1B)
├── vector<VkImage>      _images             (24B + N*8B)
├── vector<VkImageView>  _imageViews         (24B + N*8B)
├── vector<VkFence>      _imagesInFlight     (24B + N*8B)
├── VkImage              _depthImage         (8B)
├── VkDeviceMemory       _depthMemory        (8B)
├── VkImageView          _depthView          (8B)
├── VkFormat             _depthFormat        (4B)
├── VkRenderPass         _renderPass         (8B)
├── vector<VkFramebuffer> _framebuffers      (24B + N*8B)
├── bool                 _needsRecreate      (1B)
├── uint32_t             _pendingWidth       (4B)
└── uint32_t             _pendingHeight      (4B)
```

---

## Synchronization

### Swapchain Image Lifecycle

```
       ┌──────────────────────────────────────────────┐
       │                   GPU                        │
       └──────────────────────────────────────────────┘
                    ↑                    ↓
              imageReady            renderDone
              (semaphore)           (semaphore)
                    │                    │
    ┌───────────┐   │   ┌───────────┐   │   ┌───────────┐
    │  Acquire  │───┘   │  Render   │───┘   │  Present  │
    │           │       │           │       │           │
    │ imgIdx=1  │       │ draw to   │       │ show      │
    │           │       │ image[1]  │       │ image[1]  │
    └───────────┘       └───────────┘       └───────────┘
```

### Fence Wait Sequence

```cpp
// 1. Wait for frame's previous GPU work
vkWaitForFences(dev, 1, &_frames[fi].fence, VK_TRUE, UINT64_MAX);

// 2. Acquire image
_swapchain->AcquireNextImage(imageReady, &imageIndex);

// 3. Wait if this image was used by another frame
VkFence imageFence = _swapchain->GetImageFence(imageIndex);
if (imageFence) vkWaitForFences(dev, 1, &imageFence, VK_TRUE, UINT64_MAX);

// 4. Associate this frame's fence with this image
_swapchain->SetImageFence(imageIndex, _frames[fi].fence);

// 5. Record and submit
vkResetFences(dev, 1, &_frames[fi].fence);
```

---

## Render Pass Details

### Attachments

| Index | Format | Load Op | Store Op | Initial Layout | Final Layout |
|-------|--------|---------|----------|----------------|--------------|
| 0 (Color) | B8G8R8A8_UNORM | CLEAR | STORE | UNDEFINED | PRESENT_SRC_KHR |
| 1 (Depth) | D32_SFLOAT | CLEAR | DONT_CARE | UNDEFINED | DEPTH_STENCIL_ATTACHMENT_OPTIMAL |

### Subpass Dependency

```cpp
dependency.srcSubpass    = VK_SUBPASS_EXTERNAL;
dependency.dstSubpass    = 0;
dependency.srcStageMask  = COLOR_ATTACHMENT_OUTPUT | EARLY_FRAGMENT_TESTS;
dependency.dstStageMask  = COLOR_ATTACHMENT_OUTPUT | EARLY_FRAGMENT_TESTS;
dependency.dstAccessMask = COLOR_ATTACHMENT_WRITE | DEPTH_STENCIL_ATTACHMENT_WRITE;
```

**Purpose**: 이전 프레임의 present 완료 → 이번 프레임의 render pass 시작 동기화.

---

## Integration Examples

### Creating VulkanSwapchain

```cpp
_swapchain = std::make_unique<VulkanSwapchain>();

VulkanSwapchain::CreateInfo info;
info.core    = _core.get();
info.surface = _core->GetSurface();
info.width   = width;
info.height  = height;
info.vsync   = _vsync;

if (!_swapchain->Create(info)) return false;
```

### Frame Loop Integration

```cpp
// BeginFrame
if (!_swapchain->RecreateIfNeeded()) {
    _frameActive = false;
    return;
}

VkResult acq = _swapchain->AcquireNextImage(
    _frames[_frameIndex].imageReady, &_imageIndex);
if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
    _frameActive = false;
    return;
}

// ... render ...

// EndFrame
_swapchain->Present(_device->GetGraphicsQueue(),
                   _frames[_frameIndex].renderDone,
                   _imageIndex);
```

---

## Testing Checklist

- [x] Build verification (0 errors)
- [ ] Runtime: initial swapchain creation
- [ ] Runtime: window resize → deferred recreate
- [ ] Runtime: minimize → OUT_OF_DATE handling
- [ ] Runtime: VSync toggle
- [ ] Depth format fallback (test on Intel iGPU)

---

## Files

| File | Path |
|------|------|
| Header | `Vulkan/Public/VulkanSwapchain.h` |
| Implementation | `Vulkan/Private/VulkanSwapchain.cpp` |
| Plan | `Docs/progress/plan_02_vulkan_swapchain.md` |
| This doc | `Docs/progress/subsystem_02_vulkan_swapchain.md` |

---

## Build Verification

```
Build succeeded.
0 Warning(s)
0 Error(s)
```

