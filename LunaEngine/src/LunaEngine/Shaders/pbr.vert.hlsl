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

    // Normal transform: use adjugate of upper 3x3 (= det * inverse).
    // This correctly handles non-uniform scaling (e.g. annotation boxes: 2x1.5x4.5 m).
    // After normalization, adjugate gives the same direction as inverse-transpose.
    // Columns of modelMatrix upper 3x3:
    float3x3 m = (float3x3)modelMatrix;
    float3 mc0 = float3(m[0][0], m[1][0], m[2][0]);
    float3 mc1 = float3(m[0][1], m[1][1], m[2][1]);
    float3 mc2 = float3(m[0][2], m[1][2], m[2][2]);
    float3x3 normalMatrix;
    normalMatrix[0] = cross(mc1, mc2);
    normalMatrix[1] = cross(mc2, mc0);
    normalMatrix[2] = cross(mc0, mc1);
    output.normalWS  = normalize(mul(input.normal, normalMatrix));

    // Tangent/bitangent are vectors (not normals) — standard model matrix is correct.
    output.tangentWS = normalize(mul(input.tangent.xyz, m));
    output.bitanWS   = normalize(cross(output.normalWS, output.tangentWS) * input.tangent.w);

    output.uv = input.uv;
    return output;
}
