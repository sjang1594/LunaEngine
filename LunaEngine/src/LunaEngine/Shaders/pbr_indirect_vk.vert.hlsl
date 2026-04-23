// pbr_indirect_vk.vert.hlsl — Vulkan indirect G-buffer vertex shader (SM 6.0)
// Phase 15B: Reads model matrix + materialIndex from an SSBO indexed by SV_InstanceID.
// firstInstance = objectIndex in VkDrawIndexedIndirectCommand → SV_InstanceID = objectIndex.
//
// Descriptor layout:
//   set=0, binding=0 — ViewProjCB (view + proj matrices)
//   set=0, binding=1 — StructuredBuffer<GPUObjectData> gObjectData

struct GPUObjectData
{
    row_major float4x4 model;   // 64 B
    float4  boundingSphere;     // 16 B
    uint    meshIndex;          //  4 B
    uint    materialIndex;      //  4 B
    uint2   _unused;            //  8 B
};

[[vk::binding(0, 0)]]
cbuffer ViewProjCB : register(b0)
{
    row_major float4x4 viewMatrix;
    row_major float4x4 projectionMatrix;
};

[[vk::binding(1, 0)]]
StructuredBuffer<GPUObjectData> gObjectData : register(t0);

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD0;
    float4 tangent  : TANGENT;
};

struct VSOutput
{
    float4 posCS                          : SV_POSITION;
    float3 posWS                          : POSITION;
    float3 normalWS                       : NORMAL;
    float2 uv                             : TEXCOORD0;
    float3 tangentWS                      : TANGENT;
    float3 bitanWS                        : BITANGENT;
    nointerpolation uint materialIndex    : MATERIAL_INDEX;
};

VSOutput main(VSInput input, uint gObjectIndex : SV_InstanceID)
{
    VSOutput output;

    GPUObjectData obj = gObjectData[gObjectIndex];

    float4 worldPos    = mul(float4(input.position, 1.0), obj.model);
    output.posCS       = mul(mul(worldPos, viewMatrix), projectionMatrix);
    output.posWS       = worldPos.xyz;

    float3x3 normalMat = (float3x3)obj.model;
    output.normalWS    = normalize(mul(input.normal,      normalMat));
    output.tangentWS   = normalize(mul(input.tangent.xyz, normalMat));
    output.bitanWS     = normalize(cross(output.normalWS, output.tangentWS) * input.tangent.w);

    output.uv            = input.uv;
    output.materialIndex = obj.materialIndex;

    return output;
}
