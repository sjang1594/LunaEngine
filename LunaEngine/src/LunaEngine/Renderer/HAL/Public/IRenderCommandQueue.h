#pragma once

class IRenderCommandQueue
{
public:
    virtual ~IRenderCommandQueue() = default;
    virtual void SubmitCommands() = 0;
    virtual void Sync() = 0;
};
