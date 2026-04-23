// csm_depth_vk.vert.hlsl — CSM depth-only vertex shader (Vulkan SM 6.0)
// Renders scene geometry depth from the directional light's perspective.
// One draw call per cascade; the light-space MVP is passed as a push constant (64 B).

struct PushConstants
{
    row_major float4x4 lightMVP;
};
[[vk::push_constant]] PushConstants pc;

struct VSInput
{
    float3 position : POSITION;
};

float4 main(VSInput input) : SV_POSITION
{
    return mul(float4(input.position, 1.0), pc.lightMVP);
}

