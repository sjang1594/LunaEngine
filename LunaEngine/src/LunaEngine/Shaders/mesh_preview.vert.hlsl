// mesh_preview.vert.hlsl — SM 6.0
// PBRVertex input; same b0=MVP CBV root signature as constantbuffer shaders.
// Outputs world-space normal for diffuse shading in the pixel shader.

cbuffer Transform : register(b0)
{
    row_major float4x4 model;
    row_major float4x4 view;
    row_major float4x4 proj;
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
    float4 position    : SV_POSITION;
    float3 worldNormal : NORMAL;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    float4 worldPos    = mul(float4(input.position, 1.0f), model);
    output.position    = mul(mul(worldPos, view), proj);
    // Assumes uniform scale; for non-uniform scale use the inverse-transpose normal matrix
    output.worldNormal = mul(input.normal, (float3x3)model);
    return output;
}
