// oit_forward.vert.hlsl — Phase 31: OIT transparent geometry vertex shader (DX12)
// Input: PBRVertex (Position + Normal + UV + Tangent, stride 48 B)
//
// Root signature (OITForward):
//   b0 (ALL) — OITTransform: modelMatrix(64B) + viewMatrix(64B) + projMatrix(64B)
//                             + alpha(4B) + _pad(60B) = 256B per draw slot
//   b1 (PS)  — SceneConstants (lightDir, lightColor, eyePos)
//   t0 (PS)  — albedoTex

cbuffer OITTransform : register(b0)
{
    row_major float4x4 modelMatrix;
    row_major float4x4 viewMatrix;
    row_major float4x4 projMatrix;
    float              alpha;      // read by PS — ignored in VS
    float3             _pad;
};

struct VSIn
{
    float3 posOS     : POSITION;
    float3 normalOS  : NORMAL;
    float2 uv        : TEXCOORD0;
    float4 tangentOS : TANGENT;
};

struct VSOut
{
    float4 posCS    : SV_POSITION;
    float3 posWS    : POSITION;
    float3 normalWS : NORMAL;
    float2 uv       : TEXCOORD0;
};

VSOut main(VSIn v)
{
    VSOut o;
    float4 posWS = mul(float4(v.posOS, 1.0), modelMatrix);
    o.posWS      = posWS.xyz;
    o.posCS      = mul(mul(posWS, viewMatrix), projMatrix);
    float3x3 normalMat = (float3x3)modelMatrix;
    o.normalWS   = normalize(mul(v.normalOS, normalMat));
    o.uv         = v.uv;
    return o;
}
