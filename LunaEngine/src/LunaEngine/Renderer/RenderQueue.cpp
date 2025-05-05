#include "LunaPCH.h"
#include "IRenderCommand.h"
#include "RenderQueue.h"

namespace Luna
{

void RenderQueue::Submit(std::unique_ptr<class IRenderCommand> command){
    _queue.emplace_back(std::move(command));    
}

void RenderQueue::Execute()
{
    for (auto& cmd: _queue)
        cmd->Execute();
    _queue.clear();
}

void RenderQueue::Clear()
{
    _queue.clear();   
}
}