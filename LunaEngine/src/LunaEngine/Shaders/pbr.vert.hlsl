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

    float4 worldPos     = mul(float4(input.position, 1.0), modelMatrix);
    output.posCS        = mul(mul(worldPos, viewMatrix), projectionMatrix);
    output.posWS        = worldPos.xyz;

    // Transform normals by the model matrix (upper 3x3).
    // mul(v, M) is the row-vector convention matching DirectXMath storage.
    // For uniform-scale models, the rotation part works directly for normals.
    float3x3 normalMatrix = (float3x3)modelMatrix;
    output.normalWS  = normalize(mul(input.normal,       normalMatrix));

    output.tangentWS = normalize(mul(input.tangent.xyz,  normalMatrix));
    // Bitangent = cross(N, T) * tangent.w (w encodes handedness)
    output.bitanWS   = normalize(cross(output.normalWS, output.tangentWS) * input.tangent.w);

    output.uv = input.uv;
    return output;
}
