// pbr_indirect.vert.hlsl — PBR vertex shader for GPU-driven indirect drawing (SM 6.0)
// Phase 12: Model matrix comes from a StructuredBuffer indexed by objectIndex (pushed via
// root constant in the IndirectDrawCommand). View/Proj are still in a per-frame CBV.
//
// Root signature (PBRIndirect layout):
//   b0 — ViewProjCB: view + proj matrices
//   b2 — materialIndex (1 DWORD root constant, same as Phase 11)
//   b3 — objectIndex   (1 DWORD root constant, from command signature)
//   t0, space0 — StructuredBuffer<GPUObjectData>: per-instance data
//   t0, space1 — Texture2D<float4> gAllTextures[] (unbounded bindless heap)
//   s0 — static anisotropic sampler

struct GPUObjectData
{
    row_major float4x4 model;
    float4   boundingSphere;
    uint     meshIndex;
    uint     materialIndex;
    uint     _pad[2];
};

cbuffer ViewProjCB : register(b0)
{
    row_major float4x4 viewMatrix;
    row_major float4x4 projectionMatrix;
};

cbuffer ObjectIndexCB : register(b3)
{
    uint gObjectIndex;
};

StructuredBuffer<GPUObjectData> gObjectData : register(t0, space0);

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
    float3 posWS     : POSITION;
    float3 normalWS  : NORMAL;
    float2 uv        : TEXCOORD0;
    float3 tangentWS : TANGENT;
    float3 bitanWS   : BITANGENT;
};

VSOutput main(VSInput input)
{
    VSOutput output;

    float4x4 modelMatrix = gObjectData[gObjectIndex].model;

    float4 worldPos     = mul(float4(input.position, 1.0), modelMatrix);
    output.posCS        = mul(mul(worldPos, viewMatrix), projectionMatrix);
    output.posWS        = worldPos.xyz;

    float3x3 normalMatrix = (float3x3)modelMatrix;
    output.normalWS  = normalize(mul(input.normal,       normalMatrix));
    output.tangentWS = normalize(mul(input.tangent.xyz,  normalMatrix));
    output.bitanWS   = normalize(cross(output.normalWS, output.tangentWS) * input.tangent.w);

    output.uv = input.uv;
    return output;
}

