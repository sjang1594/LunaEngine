#pragma once

/*** CORE LAYER INTERFACE ***/
class IRenderDevice
{
public:
    virtual ~IRenderDevice() = default;
    // Default no-ops — each backend overrides only the signature it needs.
    virtual bool Initialize()                                   { return false; }
#ifdef LUNA_VULKAN_ENABLED
    // Vulkan-specific overload: takes a pre-created VkInstance + VkSurfaceKHR.
    virtual bool Initialize(VkInstance, VkSurfaceKHR)          { return false; }
#endif
    virtual const char* GetDeviceName() const = 0;
};