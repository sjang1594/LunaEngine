#pragma once

class IRenderCommand
{
  public:
    virtual ~IRenderCommand() = default;
    virtual void Execute() = 0;
};
