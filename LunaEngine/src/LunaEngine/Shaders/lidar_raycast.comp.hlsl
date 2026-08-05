// lidar_raycast.comp.hlsl — S3: LiDAR sensor GPU raycasting via inline RayQuery
// One thread per ray. Fires a ray against the scene TLAS, interpolates surface
// normal for incidence angle, reads material albedo for NIR intensity proxy.
// Compiled as cs_6_5 (RayQuery requires SM 6.5 — set in DX12Pipeline via csTarget).
//
// Root signature (RootSignatureLayout::LiDARRaycast) — all root descriptors (GPU VAs):
//   b0 = LiDARSensorCB
//   t0 = TLAS  (RaytracingAccelerationStructure)
//   t1 = gRayDirs  (StructuredBuffer<float3>)
//   t2 = gObjects  (StructuredBuffer<GPUObjectData>)
//   t3 = gMergedVB (ByteAddressBuffer — PBRVertex array, stride 48B)
//   t4 = gMergedIB (ByteAddressBuffer — uint32 index array)
//   u0 = gOutput   (RWStructuredBuffer<LiDAROutPoint>)

cbuffer LiDARSensorCB : register(b0)
{
    row_major float4x4 worldMat;       // sensor-to-world transform
    float  maxRange;                   // metres
    uint   numRays;
    float  rangeNoiseSigma;            // metres (σ ≈ 0.02 for HDL-32E)
    float  nirMultiplier;              // per-sensor reflectivity scale
};

RaytracingAccelerationStructure sceneTLAS : register(t0);

struct float3_packed { float x, y, z; };
StructuredBuffer<float3_packed> gRayDirs : register(t1);

// GPUObjectData mirrors Mesh.h (96 bytes)
struct GPUObjectData {
    float4x4 model;           // 64B
    float4   boundingSphere;  // 16B
    uint     meshIndex;       //  4B
    uint     materialIndex;   //  4B
    uint2    matCBAddrLo_Hi;  //  8B — D3D12_GPU_VIRTUAL_ADDRESS split into 2 uint32
};
StructuredBuffer<GPUObjectData> gObjects : register(t2);

ByteAddressBuffer gMergedVB : register(t3);  // PBRVertex, stride 48B
ByteAddressBuffer gMergedIB : register(t4);  // uint32 indices

// LiDAROutPoint (32B)
struct LiDAROutPoint {
    float3 posWS;      // world-space hit (1e30 if miss)
    float  intensity;  // [0,1] (0 if miss)
    uint   ringIndex;  // beam ring (0 = bottom)
    float  range;      // metres (0 if miss)
    uint   _pad0;
    uint   _pad1;
};
RWStructuredBuffer<LiDAROutPoint> gOutput : register(u0);

// ---- Helpers ----------------------------------------------------------------
uint lcg(uint v) { return v * 1664525u + 1013904223u; }
float uintToFloat01(uint v) { return float(v >> 9u) * (1.0f / float(1u << 23u)); }

// Box-Muller: returns one Gaussian sample
float randGaussian(uint s0, uint s1)
{
    float u1 = max(uintToFloat01(lcg(s0)), 1e-7f);
    float u2 = uintToFloat01(lcg(s1));
    return sqrt(-2.0f * log(u1)) * cos(6.28318530718f * u2);
}

// Load PBRVertex normal at index i (stride 48B, normal at offset 12B)
float3 LoadNormal(uint idx)
{
    uint byteOff = idx * 48u + 12u;
    uint4 raw    = gMergedVB.Load4(byteOff);  // loads 16 bytes (normal xyz + start of uv)
    return float3(asfloat(raw.x), asfloat(raw.y), asfloat(raw.z));
}

// Load triangle vertex indices from merged IB
uint3 LoadTriangle(uint primIdx)
{
    uint byteOff = primIdx * 12u;  // 3 × 4B per triangle
    uint3 i;
    i.x = gMergedIB.Load(byteOff + 0u);
    i.y = gMergedIB.Load(byteOff + 4u);
    i.z = gMergedIB.Load(byteOff + 8u);
    return i;
}

float luminance(float3 c) { return dot(c, float3(0.2126f, 0.7152f, 0.0722f)); }

// ---- Main -------------------------------------------------------------------
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint rayIdx = tid.x;
    if (rayIdx >= numRays)
    {
        // Write miss for out-of-range threads
        LiDAROutPoint miss;
        miss.posWS    = float3(1e30f, 1e30f, 1e30f);
        miss.intensity = 0.0f;
        miss.ringIndex = 0u;
        miss.range     = 0.0f;
        miss._pad0 = miss._pad1 = 0u;
        gOutput[rayIdx] = miss;
        return;
    }

    // ── 1. Build world-space ray ───────────────────────────────────────────
    float3_packed dp = gRayDirs[rayIdx];
    float3 dirLocal  = float3(dp.x, dp.y, dp.z);

    // Transform direction by sensor world matrix (upper 3×3 — no translation)
    float3 dirWS = normalize(mul(float4(dirLocal, 0.0f), worldMat).xyz);

    // Ray origin = sensor world position (4th row of row-major world matrix)
    float3 originWS = float3(worldMat._41, worldMat._42, worldMat._43);

    RayDesc ray;
    ray.Origin    = originWS;
    ray.Direction = dirWS;
    ray.TMin      = 0.05f;   // avoid self-intersection at sensor origin
    ray.TMax      = maxRange;

    // ── 2. RayQuery ─────────────────────────────────────────────────────────
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH
           | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER
           | RAY_FLAG_CULL_BACK_FACING_TRIANGLES> rq;

    rq.TraceRayInline(sceneTLAS, RAY_FLAG_NONE, 0xFF, ray);
    rq.Proceed();

    LiDAROutPoint result;
    result._pad0 = result._pad1 = 0u;

    // Ring index: rays are stored elevation-major (inner loop = azimuth)
    // We encode ring index in the upper bits from the pattern: ringIdx = rayIdx / numAzSteps
    // The backend passes ringCount in worldMat._44 as a hack-free path via numRays only.
    // Simpler: ring = implicit from beam order; SensorLayer decodes from pointCloud index.
    // Store raw rayIdx — SensorLayer will derive ring from index ordering.
    result.ringIndex = rayIdx;  // backend resolves actual ring assignment in readback

    if (rq.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        float  t         = rq.CommittedRayT();
        uint   instIdx   = rq.CommittedInstanceIndex();
        uint   primIdx   = rq.CommittedPrimitiveIndex();
        float2 bary      = rq.CommittedTriangleBarycentrics(); // (u, v) — barycentric weights for v1, v2

        // ── 3. Interpolate surface normal ──────────────────────────────────
        uint3 tri = LoadTriangle(primIdx);
        float3 n0 = LoadNormal(tri.x);
        float3 n1 = LoadNormal(tri.y);
        float3 n2 = LoadNormal(tri.z);
        float3 nLocal = normalize(n0 * (1.0f - bary.x - bary.y) + n1 * bary.x + n2 * bary.y);

        // Transform normal to world space using object world matrix
        GPUObjectData obj   = gObjects[instIdx];
        float3        nWS   = normalize(mul(float4(nLocal, 0.0f), obj.model).xyz);

        // ── 4. NIR intensity: albedo luminance × cos(θ_incidence) / r² ────
        // Read albedo factor from material CB (GPU VA stored in 2×uint32)
        // MaterialConstants layout: float4 albedoFactor at offset 0
        // We can't dereference arbitrary GPU VA in HLSL — use object materialIndex
        // as proxy: treat it as 1.0 (default reflectivity) scaled by nirMultiplier.
        // Full material sampling requires bindless heap — deferred to S3b.
        // For now: NIR proxy = nirMultiplier × cos(incidence) / r²
        float cosInc    = max(0.0f, dot(nWS, -dirWS));
        float intensity = nirMultiplier * cosInc / max(t * t, 0.01f);
        // Normalize to [0,1] assuming max expected intensity at 1m with cosInc=1
        intensity = saturate(intensity);

        // ── 5. Range noise (Gaussian, σ = rangeNoiseSigma) ────────────────
        uint  seed = rayIdx * 2654435761u;
        float noise = rangeNoiseSigma * randGaussian(seed, lcg(seed));
        float tNoisy = max(0.0f, t + noise);

        float3 hitWS = originWS + dirWS * tNoisy;

        result.posWS     = hitWS;
        result.intensity = intensity;
        result.range     = tNoisy;
    }
    else
    {
        // Miss
        result.posWS     = float3(1e30f, 1e30f, 1e30f);
        result.intensity = 0.0f;
        result.range     = 0.0f;
    }

    gOutput[rayIdx] = result;
}
