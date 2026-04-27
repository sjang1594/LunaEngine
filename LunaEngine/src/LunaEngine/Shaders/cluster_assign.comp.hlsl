// cluster_assign.comp.hlsl — Phase 24: Clustered Lighting (DX12)
// Assigns point lights to 16×9×24 view-space clusters (logarithmic depth slicing).
// Dispatch: (16, 9, 24) — one thread per cluster.
//
// RootSig:
//   b0: ClusterParams (cbuffer)
//   t0: GPUPointLight[] (StructuredBuffer)
//   u0: clusterLightCounts (RWStructuredBuffer)
//   u1: clusterLightIndices (RWStructuredBuffer)

cbuffer ClusterParams : register(b0)
{
    row_major float4x4 invProj;
    float nearZ;
    float farZ;
    float screenW;
    float screenH;
    uint  numLights;
    uint3 _pad;
};

struct GPUPointLight
{
    float3 position;  // view-space
    float  radius;
    float3 color;
    float  intensity;
};

StructuredBuffer<GPUPointLight>   lights             : register(t0);
RWStructuredBuffer<uint>          clusterLightCount  : register(u0);
RWStructuredBuffer<uint>          clusterLightIndex  : register(u1);

static const uint CLUSTER_X = 16u;
static const uint CLUSTER_Y = 9u;
static const uint CLUSTER_Z = 24u;
static const uint MAX_LIGHTS_PER_CLUSTER = 128u;

// Reconstruct view-space position from screen UV + linear depth
float3 ScreenToView(float2 uv, float z)
{
    float4 ndc;
    ndc.x = uv.x * 2.0f - 1.0f;
    ndc.y = 1.0f - uv.y * 2.0f;  // DX12 Y convention
    ndc.z = 0.5f;  // arbitrary — we only need XY direction
    ndc.w = 1.0f;
    float4 vs = mul(ndc, invProj);
    vs /= vs.w;
    // Scale XY by z/vs.z to get the actual view-space position at depth z
    return float3(vs.xy * (z / vs.z), z);
}

// Sphere-AABB intersection test
bool SphereAABBIntersect(float3 center, float radius, float3 aabbMin, float3 aabbMax)
{
    // Closest point on AABB to sphere center
    float3 closest = clamp(center, aabbMin, aabbMax);
    float3 d = center - closest;
    return dot(d, d) <= radius * radius;
}

[numthreads(1, 1, 1)]
void main(uint3 gid : SV_DispatchThreadID)
{
    if (gid.x >= CLUSTER_X || gid.y >= CLUSTER_Y || gid.z >= CLUSTER_Z)
        return;

    uint clusterIdx = gid.x + gid.y * CLUSTER_X + gid.z * CLUSTER_X * CLUSTER_Y;

    // Logarithmic depth slicing
    float logRatio = log(farZ / nearZ);
    float sliceNear = nearZ * exp(logRatio * float(gid.z) / float(CLUSTER_Z));
    float sliceFar  = nearZ * exp(logRatio * float(gid.z + 1u) / float(CLUSTER_Z));

    // Screen UV bounds for this cluster tile
    float2 uvMin = float2(float(gid.x) / float(CLUSTER_X), float(gid.y) / float(CLUSTER_Y));
    float2 uvMax = float2(float(gid.x + 1u) / float(CLUSTER_X), float(gid.y + 1u) / float(CLUSTER_Y));

    // Reconstruct 4 corners at near and far depth → view-space AABB
    float3 c0 = ScreenToView(uvMin, sliceNear);
    float3 c1 = ScreenToView(float2(uvMax.x, uvMin.y), sliceNear);
    float3 c2 = ScreenToView(float2(uvMin.x, uvMax.y), sliceNear);
    float3 c3 = ScreenToView(uvMax, sliceNear);
    float3 c4 = ScreenToView(uvMin, sliceFar);
    float3 c5 = ScreenToView(float2(uvMax.x, uvMin.y), sliceFar);
    float3 c6 = ScreenToView(float2(uvMin.x, uvMax.y), sliceFar);
    float3 c7 = ScreenToView(uvMax, sliceFar);

    float3 aabbMin = min(min(min(c0, c1), min(c2, c3)), min(min(c4, c5), min(c6, c7)));
    float3 aabbMax = max(max(max(c0, c1), max(c2, c3)), max(max(c4, c5), max(c6, c7)));

    // Test each light against this cluster's AABB
    uint count = 0u;
    uint baseIdx = clusterIdx * MAX_LIGHTS_PER_CLUSTER;

    for (uint i = 0u; i < numLights && count < MAX_LIGHTS_PER_CLUSTER; ++i)
    {
        if (SphereAABBIntersect(lights[i].position, lights[i].radius, aabbMin, aabbMax))
        {
            clusterLightIndex[baseIdx + count] = i;
            count++;
        }
    }

    clusterLightCount[clusterIdx] = count;
}

