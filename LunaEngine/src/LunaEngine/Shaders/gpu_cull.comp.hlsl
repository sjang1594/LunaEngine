// gpu_cull.comp.hlsl — GPU frustum culling compute shader (SM 6.0)
// Phase 12: Tests each object's bounding sphere against 6 frustum planes.
// Visible objects are appended to the indirect argument buffer via an atomic counter.
//
// Root signature (GPUCull layout):
//   b0 — CullConstants: frustum planes (6 × float4) + objectCount (uint)
//   t0 — StructuredBuffer<GPUObjectData>: per-instance data (model, bounds, meshIdx, matIdx)
//   t1 — StructuredBuffer<MeshDrawInfo>:  per-mesh geometry offsets
//   u0 — RWStructuredBuffer<IndirectDrawCommand>: output indirect args
//   u1 — RWByteAddressBuffer: draw count (atomic)

struct GPUObjectData
{
    row_major float4x4 model;       // 64 B
    float4   boundingSphere;        // 16 B — xyz=centre(obj), w=radius
    uint     meshIndex;             //  4 B
    uint     materialIndex;         //  4 B
    uint64_t materialCBAddr;        //  8 B — material constants GPU virtual address
};

struct MeshDrawInfo
{
    uint indexCount;
    uint firstIndex;
    int  vertexOffset;
    uint _pad;
};

// Layout matches D3D12 command signature: CBV first, then constants, then DRAW_INDEXED.
struct IndirectDrawCommand
{
    uint64_t materialCBAddr;      // offset  0 — CBV root param 1 (b1)
    uint materialIndex;           // offset  8 — root const b2
    uint objectIndex;             // offset 12 — root const b3
    uint indexCountPerInstance;   // offset 16
    uint instanceCount;           // offset 20
    uint startIndexLocation;      // offset 24
    int  baseVertexLocation;      // offset 28
    uint startInstanceLocation;   // offset 32
    uint _pad;                    // offset 36
};

cbuffer CullConstants : register(b0)
{
    float4 frustumPlanes[6];   // 96 B — xyz=normal, w=distance (Ax+By+Cz+D=0, inward-facing)
    uint   objectCount;        //  4 B
    uint3  _cullPad;           // 12 B → 112 B total
};

StructuredBuffer<GPUObjectData>       gObjects   : register(t0);
StructuredBuffer<MeshDrawInfo>        gMeshInfo  : register(t1);
RWStructuredBuffer<IndirectDrawCommand> gDrawArgs : register(u0);
RWByteAddressBuffer                   gDrawCount : register(u1);

// Test if a world-space sphere (centre, radius) is inside all 6 frustum half-spaces.
bool FrustumTestSphere(float3 centre, float radius)
{
    [unroll]
    for (uint i = 0; i < 6; ++i)
    {
        float d = dot(frustumPlanes[i].xyz, centre) + frustumPlanes[i].w;
        if (d < -radius)
            return false;  // entirely outside this plane
    }
    return true;
}

[numthreads(64, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    uint idx = dtid.x;
    if (idx >= objectCount)
        return;

    GPUObjectData obj = gObjects[idx];

    // Transform bounding sphere centre to world space
    float4 centreWS = mul(float4(obj.boundingSphere.xyz, 1.0), obj.model);
    float  radius   = obj.boundingSphere.w;

    // Uniform scale assumption: extract max axis scale for radius scaling
    float sx = length(float3(obj.model[0][0], obj.model[0][1], obj.model[0][2]));
    float sy = length(float3(obj.model[1][0], obj.model[1][1], obj.model[1][2]));
    float sz = length(float3(obj.model[2][0], obj.model[2][1], obj.model[2][2]));
    float maxScale = max(sx, max(sy, sz));
    radius *= maxScale;

    if (!FrustumTestSphere(centreWS.xyz, radius))
        return;  // culled

    // Visible — atomically append an indirect draw command
    uint drawIdx;
    gDrawCount.InterlockedAdd(0, 1, drawIdx);

    MeshDrawInfo mi = gMeshInfo[obj.meshIndex];

    IndirectDrawCommand cmd;
    cmd.indexCountPerInstance = mi.indexCount;
    cmd.instanceCount        = 1;
    cmd.startIndexLocation   = mi.firstIndex;
    cmd.baseVertexLocation   = mi.vertexOffset;
    cmd.startInstanceLocation= 0;
    cmd.materialCBAddr       = obj.materialCBAddr;
    cmd.materialIndex        = obj.materialIndex;
    cmd.objectIndex          = idx;
    cmd._pad                 = 0;

    gDrawArgs[drawIdx] = cmd;
}

