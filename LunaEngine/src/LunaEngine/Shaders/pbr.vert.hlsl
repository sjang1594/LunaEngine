// pbr.vert.hlsl — PBR vertex shader (SM 6.0)
// Transforms position and passes world-space data to the pixel shader.

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
    float4 tangent  : TANGENT;  // xyz = tangent direction, w = bitangent sign
};

struct VSOutput
{
    float4 posCS     : SV_POSITION;   // clip-space position
    float3 posWS     : POSITION;      // world-space position (for lighting)
    float3 normalWS  : NORMAL;        // world-space normal
    float2 uv        : TEXCOORD0;
    float3 tangentWS : TANGENT;
    float3 bitanWS   : BITANGENT;
};

VSOutput main(VSInput input)
{
    VSOutput output;

    float4 worldPos     = mul(modelMatrix, float4(input.position, 1.0));
    output.posCS        = mul(projectionMatrix, mul(viewMatrix, worldPos));
    output.posWS        = worldPos.xyz;

    // Transform normals by the inverse-transpose of the model matrix (upper 3x3).
    // For uniform-scale models, the model matrix works directly.
    float3x3 normalMatrix = (float3x3)modelMatrix;
    output.normalWS  = normalize(mul(normalMatrix, input.normal));

    output.tangentWS = normalize(mul(normalMatrix, input.tangent.xyz));
    // Bitangent = cross(N, T) * tangent.w (w encodes handedness)
    output.bitanWS   = normalize(cross(output.normalWS, output.tangentWS) * input.tangent.w);

    output.uv = input.uv;
    return output;
}
