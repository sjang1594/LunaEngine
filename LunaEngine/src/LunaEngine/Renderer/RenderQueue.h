#pragma once

namespace Luna
{
class RenderQueue
{
public:
    void Submit(std::unique_ptr<class IRenderCommand> command);
    void Execute();
    void Clear();

private:
    std::vector<std::unique_ptr<class IRenderCommand>> _queue;
};
}
