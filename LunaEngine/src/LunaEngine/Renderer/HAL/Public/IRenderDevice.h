#pragma once

/*** CORE LAYER INTERFACE ***/
// P0-06: Removed Vulkan-typed Initialize() overload from this backend-agnostic header.
// VulkanDevice declares its own platform-specific Initialize() without base-class involvement.
class IRenderDevice
{
public:
    virtual ~IRenderDevice() = default;
    virtual bool Initialize() { return true; }
    virtual const char* GetDeviceName() const = 0;
};