#pragma once

/*** CORE LAYER INTERFACE ***/
class IRenderDevice
{
public:
    virtual ~IRenderDevice() = default;
    // Default no-ops — each backend overrides only the signature it needs.
    virtual bool Initialize()                                   { return false; }
    virtual bool Initialize(VkInstance, VkSurfaceKHR)          { return false; }
    virtual const char* GetDeviceName() const = 0;
};