#pragma once

/*** CORE LAYER INTERFACE ***/
class IRenderDevice
{
public:
    virtual ~IRenderDevice() = default;
    virtual const char* GetDeviceName() const = 0;
};