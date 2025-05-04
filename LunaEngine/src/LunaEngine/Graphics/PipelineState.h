#pragma once

namespace Luna
{
    struct PipelineStateDesc
    {
        bool enableDepthTest = false;
        bool enableWireFrame = false;
    };

    class PipelineState
    {
    public:
        virtual ~PipelineState() = default;
        virtual void Bind() = 0;
    };
}