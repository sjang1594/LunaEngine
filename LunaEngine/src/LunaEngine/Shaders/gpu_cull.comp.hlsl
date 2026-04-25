// gpu_cull.comp.hlsl — GPU frustum + Hi-Z occlusion culling compute shader (SM 6.0)
// Phase 12: Tests each object's bounding sphere against 6 frustum planes.
// Phase 23: After frustum test, projects bounding sphere to screen AABB and tests
//           against Hi-Z depth pyramid. Objects fully behind the pyramid are culled.
// Visible objects are appended to the indirect argument buffer via an atomic counter.
//
// Root signature (GPUCull layout):
//   b0 — CullConstants: frustum planes + objectCount + viewProj + screenSize + hizParams (48 DWORDs)
//   t0 — StructuredBuffer<GPUObjectData>: per-instance data (model, bounds, meshIdx, matIdx)
//   t1 — StructuredBuffer<MeshDrawInfo>:  per-mesh geometry offsets
//   t2 — Texture2D<float>: Hi-Z depth pyramid (Phase 23)
//   u0 — RWStructuredBuffer<IndirectDrawCommand>: output indirect args
//   u1 — RWByteAddressBuffer: draw count (atomic)
//   s0 — point-clamp sampler (for Hi-Z sampling)

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
    float4 frustumPlanes[6];       // 96 B — xyz=normal, w=distance
    uint   objectCount;            //  4 B
    uint   enableHiZ;              //  4 B — 0=frustum only, 1=frustum+Hi-Z
    uint   hizMipCount;            //  4 B
    uint   _cullPad0;              //  4 B → 112 B
    row_major float4x4 viewProj;   // 64 B — camera view-projection matrix
    float2 screenSize;             //  8 B — (width, height) in pixels
    float2 _cullPad1;              //  8 B → 192 B total = 48 DWORDs
};

StructuredBuffer<GPUObjectData>       gObjects   : register(t0);
StructuredBuffer<MeshDrawInfo>        gMeshInfo  : register(t1);
Texture2D<float>                      gHiZ       : register(t2);
RWStructuredBuffer<IndirectDrawCommand> gDrawArgs : register(u0);
RWByteAddressBuffer                   gDrawCount : register(u1);
SamplerState                          gHiZSampler: register(s0);

// Test if a world-space sphere (centre, radius) is inside all 6 frustum half-spaces.
bool FrustumTestSphere(float3 centre, float radius)
{
    [unroll]
    for (uint i = 0; i < 6; ++i)
    {
        float d = dot(frustumPlanes[i].xyz, centre) + frustumPlanes[i].w;
        if (d < -radius)
            return false;
    }
    return true;
}

// Phase 23: Hi-Z occlusion test. Returns true if VISIBLE (not occluded).
bool HiZTestSphere(float3 centreWS, float radius)
{
    float4 clipCentre = mul(float4(centreWS, 1.0), viewProj);

    // Behind near plane — conservatively visible
    if (clipCentre.w <= 0.0)
        return true;

    float3 ndc = clipCentre.xyz / clipCentre.w;

    // Project sphere radius to screen-space extent
    float screenRadiusX = abs(radius * viewProj[0][0] / clipCentre.w) * 0.5;
    float screenRadiusY = abs(radius * viewProj[1][1] / clipCentre.w) * 0.5;

    // NDC → [0,1] UV (flip Y for texture coords)
    float2 uvCentre = ndc.xy * 0.5 + 0.5;
    uvCentre.y = 1.0 - uvCentre.y;

    float2 uvMin = saturate(uvCentre - float2(screenRadiusX, screenRadiusY));
    float2 uvMax = saturate(uvCentre + float2(screenRadiusX, screenRadiusY));

    // AABB size in pixels at full-res
    float2 aabbPixels = (uvMax - uvMin) * screenSize;
    float maxDim = max(aabbPixels.x, aabbPixels.y);

    // Mip where one texel covers the AABB
    float mipF = (maxDim <= 1.0) ? 0.0 : ceil(log2(maxDim));
    uint mip = min((uint)mipF, hizMipCount - 1);

    // Sample Hi-Z at 4 AABB corners
    float d0 = gHiZ.SampleLevel(gHiZSampler, uvMin, mip);
    float d1 = gHiZ.SampleLevel(gHiZSampler, float2(uvMax.x, uvMin.y), mip);
    float d2 = gHiZ.SampleLevel(gHiZSampler, float2(uvMin.x, uvMax.y), mip);
    float d3 = gHiZ.SampleLevel(gHiZSampler, uvMax, mip);

    float occluderDepth = min(min(d0, d1), min(d2, d3));

    // Standard depth: 0=near, 1=far. Cull if object is entirely behind occluder.
    return ndc.z <= occluderDepth;
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
        return;  // culled by frustum

    // Phase 23: Hi-Z occlusion test (1-frame depth lag)
    if (enableHiZ != 0)
    {
        if (!HiZTestSphere(centreWS.xyz, radius))
            return;  // culled by occlusion
    }

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
