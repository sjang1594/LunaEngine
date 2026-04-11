#pragma once

///** Interface **
namespace Luna
{

enum class VertexLayout
{
    Triangle,  // POSITION(RGB32F) + COLOR(RGBA32F)              — stride 28 B
    PBR,       // POSITION + NORMAL(RGB32F) + TEXCOORD(RG32F) + TANGENT(RGBA32F) — stride 48 B
};

struct PipelineStateDesc
{
    bool         enableDepthTest = false;
    bool         enableWireFrame = false;
    VertexLayout vertexLayout    = VertexLayout::Triangle;
};

class IPipelineState
{
public:
    virtual ~IPipelineState() = default;
};
} // namespace Luna