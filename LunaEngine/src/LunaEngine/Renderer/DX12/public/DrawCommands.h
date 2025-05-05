#pragma once
#include "Renderer/IRenderCommand.h"

namespace Luna
{
class DrawCommands : IRenderCommand
{
public:
    DrawCommands() = delete;
    DrawCommands(uint32_t vertexCount) : _vertexCount(vertexCount) {}
    void Execute() override;

private:
    uint32_t _vertexCount;
};
}