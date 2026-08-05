// Phase 32: Visibility buffer — vertex shader
// Transforms geometry to clip space; passes objectIndex as flat attribute.
// Root sig (VisibilityBuffer) is compatible with the Phase 12 ExecuteIndirect
// command signature so the same indirect draw buffer is reused.

struct PBRVertex
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD0;
    float4 tangent  : TANGENT;   // xyz=tangent, w=bitangent sign
};

struct VSOut
{
    float4 clipPos            : SV_Position;
    nointerpolation uint objectIdx : OBJECT_IDX;
};

// b0 — per-frame view+proj (128 B)
cbuffer ViewProjCB : register(b0)
{
    row_major float4x4 view;
    row_major float4x4 proj;
};

// b3 — objectIndex (1 DWORD root constant, set by command signature)
cbuffer ObjectIdxCB : register(b3) { uint gObjectIndex; }

struct GPUObjectData
{
    row_major float4x4 model;
    float4             boundingSphere;
    uint               meshIndex;
    uint               materialIndex;
    uint2              _matCBAddr;   // GPU VA — unused here
};
StructuredBuffer<GPUObjectData> gObjects : register(t0, space0);

VSOut main(PBRVertex v)
{
    float4x4 model = gObjects[gObjectIndex].model;
    float4 worldPos = mul(float4(v.position, 1.0f), model);
    float4 viewPos  = mul(worldPos, view);
    float4 clipPos  = mul(viewPos,  proj);

    VSOut o;
    o.clipPos   = clipPos;
    o.objectIdx = gObjectIndex;
    return o;
}
