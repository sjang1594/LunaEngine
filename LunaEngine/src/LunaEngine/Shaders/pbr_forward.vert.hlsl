// pbr_forward.vert.hlsl — Forward PBR vertex shader (SM 6.0, Vulkan SPIR-V)

[[vk::binding(0, 0)]]
cbuffer TransformBuffer : register(b0)
{
    row_major float4x4 modelMatrix;
    row_major float4x4 viewMatrix;
    row_major float4x4 projectionMatrix;
};

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD0;
    float4 tangent  : TANGENT;
};

struct VSOutput
{
    float4 posCS     : SV_POSITION;
    [[vk::location(0)]] float3 posWS     : POSITION;
    [[vk::location(1)]] float3 normalWS  : NORMAL;
    [[vk::location(2)]] float2 uv        : TEXCOORD0;
    [[vk::location(3)]] float3 tangentWS : TANGENT;
    [[vk::location(4)]] float3 bitanWS   : BITANGENT;
};

VSOutput main(VSInput input)
{
    VSOutput output;

    float4 worldPos  = mul(float4(input.position, 1.0), modelMatrix);
    float4 viewPos   = mul(worldPos, viewMatrix);
    float4 clipPos   = mul(viewPos, projectionMatrix);

    // Y flip handled by DXC -fvk-invert-y flag; no manual flip needed.

    output.posCS = clipPos;
    output.posWS = worldPos.xyz;

    float3x3 normalMatrix = (float3x3)modelMatrix;
    output.normalWS  = normalize(mul(input.normal,       normalMatrix));
    output.tangentWS = normalize(mul(input.tangent.xyz,  normalMatrix));
    output.bitanWS   = normalize(cross(output.normalWS, output.tangentWS) * input.tangent.w);
    output.uv        = input.uv;
    return output;
}

