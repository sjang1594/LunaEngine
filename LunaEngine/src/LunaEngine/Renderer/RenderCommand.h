#pragma once

#include "Graphics/Buffer.h"

class RenderCommand
{
public:
    static void Init();
    static void BeginFrame();
    static void EndFrame();
    static void SubmitTriangle(Luna::VertexBuffer* vertexBuffer);

private:
    static void ExecuteCommands();
};
