#pragma once

///** Interface **
namespace Luna
{

struct PipelineStateDesc
{
    bool enableDepthTest = false;
    bool enableWireFrame = false;
};

class IPipeline
{
public:
    virtual ~IPipeline() = default;
};
} // namespace Luna