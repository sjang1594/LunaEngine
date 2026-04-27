// meshlet_cull.as.hlsl — Amplification shader for mesh shader pipeline (SM 6.5)
// Phase 25: Per-meshlet frustum culling. One threadgroup per batch of 32 meshlets.
// Visible meshlets are dispatched to the mesh shader via DispatchMesh().

struct Meshlet
{
    uint vertexOffset;
    uint triangleOffset;
    uint vertexCount;
    uint triangleCount;
};

struct MeshletBounds
{
    float3 center;
    float  radius;
};

struct MeshletMeshInfo
{
    uint meshletOffset;
    uint meshletCount;
    uint _pad[2];
};

struct GPUObjectData
{
    row_major float4x4 model;
    float4   boundingSphere;
    uint     meshIndex;
    uint     materialIndex;
    uint     _pad[2];
};

cbuffer MeshShaderConstants : register(b0)
{
    row_major float4x4 viewMatrix;
    row_major float4x4 projMatrix;
    float4   frustumPlanes[6];  // normalised world-space frustum planes
    uint     objectIndex;
    uint     meshletOffset;     // global meshlet offset for this mesh
    uint     meshletCount;      // total meshlets for this mesh
    uint     _pad0;
};

StructuredBuffer<GPUObjectData>  gObjectData    : register(t0);
StructuredBuffer<Meshlet>        gMeshlets      : register(t1);
StructuredBuffer<MeshletBounds>  gMeshletBounds : register(t2);

// Payload passed to mesh shader
struct MeshShaderPayload
{
    uint meshletIndices[32];  // global meshlet indices (into gMeshlets)
    uint objectIndex;
    uint materialIndex;
};

groupshared MeshShaderPayload s_payload;

bool FrustumTestSphere(float3 center, float radius)
{
    [unroll]
    for (uint i = 0; i < 6; ++i)
    {
        float d = dot(frustumPlanes[i].xyz, center) + frustumPlanes[i].w;
        if (d < -radius)
            return false;
    }
    return true;
}

#define AS_GROUP_SIZE 32

[numthreads(AS_GROUP_SIZE, 1, 1)]
void main(uint gtid : SV_GroupThreadID, uint gid : SV_GroupID)
{
    // Each group processes up to 32 meshlets starting at gid*32
    uint meshletLocalIdx = gid * AS_GROUP_SIZE + gtid;
    bool visible = false;

    if (meshletLocalIdx < meshletCount)
    {
        uint globalMeshletIdx = meshletOffset + meshletLocalIdx;
        MeshletBounds mb = gMeshletBounds[globalMeshletIdx];

        // Transform bounding sphere centre to world space
        float4x4 modelMatrix = gObjectData[objectIndex].model;
        float3 centerWS = mul(float4(mb.center, 1.0), modelMatrix).xyz;

        // Scale radius by max axis scale
        float sx = length(float3(modelMatrix[0][0], modelMatrix[0][1], modelMatrix[0][2]));
        float sy = length(float3(modelMatrix[1][0], modelMatrix[1][1], modelMatrix[1][2]));
        float sz = length(float3(modelMatrix[2][0], modelMatrix[2][1], modelMatrix[2][2]));
        float radiusWS = mb.radius * max(sx, max(sy, sz));

        visible = FrustumTestSphere(centerWS, radiusWS);
    }

    // Compact visible meshlets using wave intrinsics
    uint visibleCount;
    uint visibleIndex = WavePrefixCountBits(visible);
    visibleCount = WaveActiveCountBits(visible);

    if (visible)
    {
        uint globalMeshletIdx = meshletOffset + meshletLocalIdx;
        s_payload.meshletIndices[visibleIndex] = globalMeshletIdx;
    }

    // First thread writes shared payload fields and dispatches
    if (gtid == 0)
    {
        s_payload.objectIndex   = objectIndex;
        s_payload.materialIndex = gObjectData[objectIndex].materialIndex;
    }

    DispatchMesh(visibleCount, 1, 1, s_payload);
}

