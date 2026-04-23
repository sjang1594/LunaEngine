// csm_depth.vert.hlsl — CSM depth-only vertex shader (SM 6.0)
// Phase 8: renders scene geometry depth from the directional light's perspective.
// One draw call per cascade; the light-space MVP for that cascade is passed as
// 16 inline root constants (b0), avoiding any separate constant-buffer allocation.
//
// Root signature: CSMDepth
//   params[0] — 16 root constants at b0 (row_major float4x4 lightMVP)
//   Deny pixel shader access (depth-only pass, no render target output).

cbuffer LightMVP : register(b0)
{
    row_major float4x4 lightMVP;  // lightVP[cascade] * modelMatrix for this draw call
};

// PBR vertex layout — position(12) + normal(12) + uv(8) + tangent(16), stride 48 B.
// Only POSITION is read here; the other semantics must be declared to match the IA
// input layout, but are silently ignored by the GPU since the VS doesn't use them.
struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD0;
    float4 tangent  : TANGENT;
};

float4 main(VSInput input) : SV_POSITION
{
    return mul(float4(input.position, 1.0), lightMVP);
}
