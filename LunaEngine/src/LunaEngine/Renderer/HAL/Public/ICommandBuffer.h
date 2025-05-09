#pragma once

class ICommandBuffer
{
public:
    virtual ~ICommandBuffer() = default;
    virtual void Begin() = 0;
    virtual void End() = 0;
    virtual void Execute() = 0;
};
