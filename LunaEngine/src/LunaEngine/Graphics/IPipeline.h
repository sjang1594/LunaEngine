#pragma once

///** Interface **
namespace Luna
{

enum class VertexLayout
{
    Triangle,  // POSITION(RGB32F) + COLOR(RGBA32F)              — stride 28 B
    PBR,       // POSITION + NORMAL(RGB32F) + TEXCOORD(RG32F) + TANGENT(RGBA32F) — stride 48 B
};

// Root signature layout drives which CBVs/SRVs are bound in CreateRootSignature().
// MVP  — single CBV b0 (legacy triangle / mesh-preview pipelines)
// PBR  — b0 MVP + b1 Material + b2 Scene + descriptor table t0-t3 + static sampler s0
enum class RootSignatureLayout
{
    MVP,
    PBR,
};

struct PipelineStateDesc
{
    bool                enableDepthTest  = false;
    bool                enableWireFrame  = false;
    VertexLayout        vertexLayout     = VertexLayout::Triangle;
    RootSignatureLayout rootSigLayout    = RootSignatureLayout::MVP;
};

class IPipeline
{
public:
    virtual ~IPipeline() = default;
};
} // namespace Luna