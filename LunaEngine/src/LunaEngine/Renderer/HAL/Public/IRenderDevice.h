#pragma once

/*** CORE LAYER INTERFACE ***/
class IRenderDevice
{
public:
    virtual ~IRenderDevice() = default;
    virtual bool Initialize();
    virtual bool Initialize(VkInstance instance, VkSurfaceKHR surface);
    virtual const char* GetDeviceName() const = 0;
};