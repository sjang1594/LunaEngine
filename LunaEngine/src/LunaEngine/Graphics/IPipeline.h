#pragma once

///** Interface **
namespace Luna
{

struct PipelineStateDesc
{
    bool enableDepthTest = false;
    bool enableWireFrame = false;
};

class IPipelineState
{
public:
    virtual ~IPipelineState() = default;
};
} // namespace Luna