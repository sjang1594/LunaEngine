#include "LunaPCH.h"
#include "Renderer/DX12/Public/DrawCommands.h"
#include "Renderer/HAL/Public/IRenderContext.h"

namespace Luna
{
void DrawCommands::Execute(){
    IRenderContext::GetBackend()->Draw(_vertexCount);
}
}