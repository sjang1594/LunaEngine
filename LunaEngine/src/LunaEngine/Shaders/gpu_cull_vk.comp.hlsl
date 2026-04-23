// gpu_cull_vk.comp.hlsl — Vulkan GPU frustum culling compute shader (SM 6.0)
// Phase 15B: Tests each object's bounding sphere against 6 frustum planes.
// Visible objects are appended to the VkDrawIndexedIndirectCommand buffer via atomic counter.
//
// Push constants: CullConstants (frustum planes + objectCount) — 112 bytes
// Bindings (set=0):
//   binding=0 — StructuredBuffer<GPUObjectData>  gObjects
//   binding=1 — StructuredBuffer<MeshDrawInfo>   gMeshInfo
//   binding=2 — RWStructuredBuffer<VkDrawCmd>    gDrawArgs
//   binding=3 — RWByteAddressBuffer              gDrawCount

struct GPUObjectData
{
    row_major float4x4 model;   // 64 B
    float4  boundingSphere;     // 16 B — xyz=centre(obj-space), w=radius
    uint    meshIndex;          //  4 B
    uint    materialIndex;      //  4 B
    uint2   _unused;            //  8 B — materialCBAddr (DX12 only, ignored here)
};

struct MeshDrawInfo
{
    uint indexCount;
    uint firstIndex;
    int  vertexOffset;
    uint _pad;
};

// VkDrawIndexedIndirectCommand — 20 bytes
struct VkDrawCmd
{
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int  vertexOffset;
    uint firstInstance;   // ← objectIndex (SV_InstanceID in VS)
};

struct CullPush
{
    // Flattened from float4[6] — DXC SPIR-V doesn't support arrays in push constant blocks
    float4 plane0;      // 16 B
    float4 plane1;      // 16 B
    float4 plane2;      // 16 B
    float4 plane3;      // 16 B
    float4 plane4;      // 16 B
    float4 plane5;      // 16 B  → 96 B total for planes
    uint   objectCount; //  4 B
    uint3  _cullPad;    // 12 B → 112 B total
};
[[vk::push_constant]] CullPush _cc;

[[vk::binding(0, 0)]] StructuredBuffer<GPUObjectData> gObjects;
[[vk::binding(1, 0)]] StructuredBuffer<MeshDrawInfo>  gMeshInfo;
[[vk::binding(2, 0)]] RWStructuredBuffer<VkDrawCmd>   gDrawArgs;
[[vk::binding(3, 0)]] RWByteAddressBuffer             gDrawCount;

bool FrustumTestSphere(float3 centre, float radius)
{
    if (dot(_cc.plane0.xyz, centre) + _cc.plane0.w < -radius) return false;
    if (dot(_cc.plane1.xyz, centre) + _cc.plane1.w < -radius) return false;
    if (dot(_cc.plane2.xyz, centre) + _cc.plane2.w < -radius) return false;
    if (dot(_cc.plane3.xyz, centre) + _cc.plane3.w < -radius) return false;
    if (dot(_cc.plane4.xyz, centre) + _cc.plane4.w < -radius) return false;
    if (dot(_cc.plane5.xyz, centre) + _cc.plane5.w < -radius) return false;
    return true;
}

[numthreads(64, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    uint idx = dtid.x;
    if (idx >= _cc.objectCount)
        return;

    GPUObjectData obj = gObjects[idx];

    // Transform bounding sphere centre to world space
    float4 centreWS = mul(float4(obj.boundingSphere.xyz, 1.0), obj.model);
    float  radius   = obj.boundingSphere.w;

    // Extract uniform scale factor from model matrix rows
    float sx = length(float3(obj.model[0][0], obj.model[0][1], obj.model[0][2]));
    float sy = length(float3(obj.model[1][0], obj.model[1][1], obj.model[1][2]));
    float sz = length(float3(obj.model[2][0], obj.model[2][1], obj.model[2][2]));
    radius *= max(sx, max(sy, sz));

    if (!FrustumTestSphere(centreWS.xyz, radius))
        return;

    // Atomically claim a slot
    uint drawIdx;
    gDrawCount.InterlockedAdd(0, 1, drawIdx);

    MeshDrawInfo mi = gMeshInfo[obj.meshIndex];

    VkDrawCmd cmd;
    cmd.indexCount    = mi.indexCount;
    cmd.instanceCount = 1;
    cmd.firstIndex    = mi.firstIndex;
    cmd.vertexOffset  = mi.vertexOffset;
    cmd.firstInstance = idx;   // objectIndex passed via SV_InstanceID

    gDrawArgs[drawIdx] = cmd;
}
