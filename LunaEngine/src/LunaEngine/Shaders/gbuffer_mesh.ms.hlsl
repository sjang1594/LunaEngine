// gbuffer_mesh.ms.hlsl — Mesh shader for G-buffer fill (SM 6.5)
// Phase 25: Reads meshlet vertex/index data from structured buffers,
// outputs triangles matching the PSInput layout of gbuffer.frag.hlsl.

struct Meshlet
{
    uint vertexOffset;
    uint triangleOffset;
    uint vertexCount;
    uint triangleCount;
};

struct GPUObjectData
{
    row_major float4x4 model;
    float4   boundingSphere;
    uint     meshIndex;
    uint     materialIndex;
    uint     _pad[2];
};

struct PBRVertex
{
    float3 position;
    float3 normal;
    float2 uv;
    float4 tangent;
};

cbuffer MeshShaderConstants : register(b0)
{
    row_major float4x4 viewMatrix;
    row_major float4x4 projMatrix;
    float4   frustumPlanes[6];
    uint     objectIndex;
    uint     meshletOffset;
    uint     meshletCount;
    uint     _pad0;
};

StructuredBuffer<GPUObjectData> gObjectData      : register(t0);
StructuredBuffer<Meshlet>       gMeshlets        : register(t1);
// t2 = meshletBounds (not needed in MS)
StructuredBuffer<PBRVertex>     gVertices        : register(t3);
StructuredBuffer<uint>          gMeshletVertices : register(t4);  // global vertex indices
StructuredBuffer<uint>          gMeshletTriangles: register(t5);  // packed local tri indices

// Payload from amplification shader
struct MeshShaderPayload
{
    uint meshletIndices[32];
    uint objectIndex;
    uint materialIndex;
};

struct VertexOut
{
    float4 posCS     : SV_POSITION;
    float3 posWS     : POSITION;
    float3 normalWS  : NORMAL;
    float2 uv        : TEXCOORD0;
    float3 tangentWS : TANGENT;
    float3 bitanWS   : BITANGENT;
};

#define MS_GROUP_SIZE 128

[numthreads(MS_GROUP_SIZE, 1, 1)]
[outputtopology("triangle")]
void main(
    uint gtid : SV_GroupThreadID,
    uint gid  : SV_GroupID,
    in payload MeshShaderPayload payloadData,
    out vertices VertexOut verts[64],
    out indices uint3 tris[124])
{
    uint meshletGlobalIdx = payloadData.meshletIndices[gid];
    Meshlet ml = gMeshlets[meshletGlobalIdx];

    SetMeshOutputCounts(ml.vertexCount, ml.triangleCount);

    // Transform vertices
    float4x4 modelMatrix = gObjectData[payloadData.objectIndex].model;
    float3x3 normalMatrix = (float3x3)modelMatrix;

    // Each thread processes one or more vertices (loop if vertexCount > GROUP_SIZE)
    for (uint v = gtid; v < ml.vertexCount; v += MS_GROUP_SIZE)
    {
        uint globalVertexIdx = gMeshletVertices[ml.vertexOffset + v];
        PBRVertex vtx = gVertices[globalVertexIdx];

        VertexOut o;
        float4 worldPos = mul(float4(vtx.position, 1.0), modelMatrix);
        o.posCS     = mul(mul(worldPos, viewMatrix), projMatrix);
        o.posWS     = worldPos.xyz;
        o.normalWS  = normalize(mul(vtx.normal,       normalMatrix));
        o.tangentWS = normalize(mul(vtx.tangent.xyz,   normalMatrix));
        o.bitanWS   = normalize(cross(o.normalWS, o.tangentWS) * vtx.tangent.w);
        o.uv        = vtx.uv;

        verts[v] = o;
    }

    // Each thread processes one or more triangles
    for (uint t = gtid; t < ml.triangleCount; t += MS_GROUP_SIZE)
    {
        uint packed = gMeshletTriangles[ml.triangleOffset + t];
        uint i0 = (packed >>  0) & 0xFF;
        uint i1 = (packed >>  8) & 0xFF;
        uint i2 = (packed >> 16) & 0xFF;
        tris[t] = uint3(i0, i1, i2);
    }
}

