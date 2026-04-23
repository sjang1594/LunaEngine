#pragma once
#include <cstdint>

// P4-06: IRenderSwapChain — backend-agnostic swapchain interface.
// Both DX12 and Vulkan backends keep their swapchain state internal;
// this interface exposes the minimum contract needed for higher-level code
// (e.g. Render Graph) to query swapchain properties without coupling to a backend.
class IRenderSwapChain
{
public:
    virtual ~IRenderSwapChain() = default;

    // Acquire the next presentable image index. Returns false on out-of-date swapchain.
    virtual bool AcquireNextImage() { return true; }

    // Present the current back-buffer. syncInterval: 0 = no vsync, 1 = vsync.
    virtual void Present(uint32_t syncInterval = 1) {}

    // Recreate the swapchain at new dimensions.
    virtual void Resize(uint32_t width, uint32_t height) {}

    // Index of the image currently being recorded into.
    virtual uint32_t GetCurrentImageIndex() const { return 0; }

    // Pixel format of the back-buffer (DXGI_FORMAT on DX12, VkFormat on Vulkan).
    virtual uint32_t GetFormat() const { return 0; }
};
