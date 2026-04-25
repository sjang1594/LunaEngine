# Bug #008: Custom Title Bar Init Order Causes Device Lost on First Frame

**Date:** 2026-04-24  
**Status:** Fixed  
**Severity:** Critical  
**Component:** Application init order, VulkanBackend swapchain image layout

---

## Symptoms

```
[ERROR] VK: Device lost during image-in-flight fence wait
[ERROR] VK: Device lost during queue submit
```

Device lost occurred reliably on the **first frame** when `customTitleBar = true`. Disabling the custom title bar eliminated the crash entirely.

- With custom title bar: window resizes from 1600×900 → 1616×938 during init
- Without custom title bar: window stays at 1600×900, no crash

---

## Root Cause Analysis

### Primary Issue: Init Order — Title Bar Triggers Resize After Vulkan Init

`CustomTitleBar::Init()` calls:
```cpp
SetWindowLong(hwnd, GWL_STYLE, style & ~WS_CAPTION | WS_THICKFRAME);
SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
    SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
```

Removing `WS_CAPTION` and calling `SWP_FRAMECHANGED` changes the client area size (non-client area shrinks, client area grows). This fires `WM_SIZE` → GLFW framebuffer callback → `_pendingResize = true`.

**The problem:** In the original init order, the Vulkan backend was already initialized at 1600×900, and GLFW callbacks were already registered. The title bar init triggered a full swapchain destroy-recreate cycle on the very first frame, producing stale descriptor set references and device lost.

```
Original init order (BROKEN):
  1. glfwCreateWindow(1600, 900)
  2. Register GLFW callbacks          ← callbacks active
  3. IRenderContext::Initialize()      ← swapchain created at 1600×900
  4. CustomTitleBar::Init()            ← WS_CAPTION removal fires WM_SIZE
     → callback sets _pendingResize = true (1616×938)
  5. First BeginFrame() → RecreateSwapchain() → stale refs → DEVICE LOST
```

### Secondary Issue: Swapchain Image Layout in ppReady=false Path

When post-processing resources aren't ready (first few frames, or after resize), `CompositeFrame()` skips the tonemap pass and jumps directly to the ImGui render pass. The tonemap pass normally transitions the swapchain image from `UNDEFINED` → `COLOR_ATTACHMENT_OPTIMAL`. Without it, the swapchain image remains in `VK_IMAGE_LAYOUT_UNDEFINED`, but the ImGui render pass (`_renderPass`) declares `initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR`.

This layout mismatch is undefined behavior per the Vulkan spec.

---

## Fixes Applied

### 1. Reorder Initialization — Title Bar Before Callbacks and Backend

Move `CustomTitleBar::Init()` BEFORE registering GLFW callbacks and BEFORE creating the Vulkan backend. Then query the actual framebuffer size and pass it to the backend.

```
Fixed init order:
  1. glfwCreateWindow(1600, 900)
  2. glfwShowWindow()
  3. CustomTitleBar::Init()            ← WS_CAPTION removal fires WM_SIZE
     → but no callbacks registered yet, so _pendingResize stays false
  4. glfwGetFramebufferSize()          ← returns 1616×938 (actual size)
  5. Register GLFW callbacks
  6. IRenderContext::Initialize(1616, 938)  ← swapchain created at correct size
  7. First BeginFrame() → no resize needed → clean frame
```

**File:** `Application.cpp`

```cpp
glfwShowWindow(_windowHandle);

// Initialize custom title bar BEFORE registering callbacks and creating
// the render backend. This prevents the WS_CAPTION removal from triggering
// a resize callback after the swapchain is already created.
if (_specification.customTitleBar)
{
    _titleBar.Init(_windowHandle);
    _titleBar.SetTitle(_specification.name);
    _titleBar.SetLogoText("LUNA");
    _titleBar.SetBackendLabel(
        _specification.backend == RenderBackendType::Vulkan ? "Vulkan" : "DirectX 12");
}

// Query the ACTUAL framebuffer size after title bar init
int actualFBWidth = 0, actualFBHeight = 0;
glfwGetFramebufferSize(_windowHandle, &actualFBWidth, &actualFBHeight);
uint32_t initWidth  = (actualFBWidth  > 0) ? (uint32_t)actualFBWidth  : _specification.width;
uint32_t initHeight = (actualFBHeight > 0) ? (uint32_t)actualFBHeight : _specification.height;

// Register GLFW callbacks AFTER title bar init
glfwSetWindowUserPointer(_windowHandle, this);
glfwSetFramebufferSizeCallback(_windowHandle, OnFramebufferResize);
// ... other callbacks ...

// Initialize render backend with actual framebuffer size
IRenderContext::Initialize(RenderBackendType::Vulkan, _windowHandle, initWidth, initHeight);

// Update camera aspect ratio if framebuffer size differs from spec
if (initWidth != _specification.width || initHeight != _specification.height)
{
    float aspect = static_cast<float>(initWidth) / static_cast<float>(initHeight);
    _camera.SetAspect(aspect);
}
```

### 2. Swapchain Image Layout Barrier in ppReady=false Path

When PP resources aren't ready and the tonemap pass is skipped, explicitly transition the swapchain image from `UNDEFINED` to `PRESENT_SRC_KHR` before opening the ImGui render pass.

**File:** `VulkanBackend.cpp` — `CompositeFrame()`

```cpp
if (!ppReady)
{
    if (_ssaoPipeline) {
        DrawSSAOPass(cmd);
        DrawSSAOBlurPass(cmd);
    }

    // Swapchain image is UNDEFINED after vkAcquireNextImageKHR.
    // _renderPass expects initialLayout = PRESENT_SRC_KHR.
    // Without this barrier, we get undefined behavior.
    VkImageMemoryBarrier swapBarrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    swapBarrier.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
    swapBarrier.newLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    swapBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    swapBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    swapBarrier.image               = _swapchainImages[_imageIndex];
    swapBarrier.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    swapBarrier.srcAccessMask       = 0;
    swapBarrier.dstAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
                                    | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0, 0, nullptr, 0, nullptr, 1, &swapBarrier);

    VkRenderPassBeginInfo irpi{};
    irpi.sType       = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    irpi.renderPass  = _renderPass;
    irpi.framebuffer = _framebuffers[_imageIndex];
    irpi.renderArea.extent = _swapchainExtent;
    irpi.clearValueCount   = 0;
    vkCmdBeginRenderPass(cmd, &irpi, VK_SUBPASS_CONTENTS_INLINE);
    return;
}
```

---

## Why Not Fix the Resize Path Instead?

The alternative was to make the first-frame `RecreateSwapchain()` safe — ensuring all descriptors, views, and pipelines reference valid resources after recreation. This would require:

- Auditing every descriptor set for stale image/buffer views
- Adding descriptor update logic post-resize for all passes
- Handling edge cases where resources reference the old swapchain extent

The init reorder is simpler, eliminates the resize entirely, and is the correct architectural fix: the backend should be initialized at the final framebuffer size, not at a size that will immediately change.

---

## Files Modified

| File | Changes |
|------|---------|
| `Application.cpp` | Reordered init: title bar → query FB size → register callbacks → create backend |
| `VulkanBackend.cpp` | Added layout barrier in `CompositeFrame()` ppReady=false path |

---

## Verification

```
Swapchain: 1616x938, 3 images    ← correct size from the start
```

- ✅ No `VK BeginFrame: deferred resize` log (resize eliminated)
- ✅ No `VK_ERROR_DEVICE_LOST` errors
- ✅ Application runs stably with custom title bar enabled
- ✅ Empty STDERR after 12 seconds of runtime

---

## Key Insight

**Init order matters.** Any operation that changes the window's client area (removing `WS_CAPTION`, DPI changes, `SetWindowPos` with `SWP_FRAMECHANGED`) must happen BEFORE the graphics backend is initialized and BEFORE resize callbacks are registered. Otherwise, the backend is created at one resolution and immediately forced to recreate at another — a dangerous path that exposes every resource lifetime bug in the resize codepath.

---

## References

- [Win32: SetWindowPos SWP_FRAMECHANGED](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowpos)
- [Vulkan Spec §8.1: Image Layout Transitions](https://registry.khronos.org/vulkan/specs/1.3-extensions/html/vkspec.html#resources-image-layouts)
- [GLFW: Window Size Callback](https://www.glfw.org/docs/latest/window_guide.html#window_size)

