// shadows.hlsl — DXR hybrid shadow pass (SM 6.5)
// RayGen reconstructs world-space position from depth + inverse VP,
// traces a shadow ray toward the directional light, and writes 0.0 (shadowed)
// or 1.0 (lit) to the output UAV.

// ---- Resources ----
RaytracingAccelerationStructure sceneTLAS : register(t0);
Texture2D<float>  depthBuffer            : register(t1); // D32_FLOAT depth
RWTexture2D<float> shadowOutput          : register(u0); // R32_FLOAT shadow mask

cbuffer ShadowCB : register(b0)
{
    row_major float4x4 invViewProj;   // inverse of (view * proj)
    float4 lightDirection;            // xyz = toward-light (normalized), w = unused
    float4 viewportSize;              // xy = width/height
};

// ---- Payload ----
struct ShadowPayload
{
    bool shadowed;
};

// -------------------------------------------------------------------------
// Ray generation shader
// -------------------------------------------------------------------------
[shader("raygeneration")]
void RayGen()
{
    uint2 launchIdx  = DispatchRaysIndex().xy;
    uint2 launchDims = DispatchRaysDimensions().xy;

    // Depth at this pixel — early out for sky/background
    float depth = depthBuffer[launchIdx].r;
    if (depth >= 1.0)
    {
        shadowOutput[launchIdx] = 1.0; // lit — no geometry
        return;
    }

    // Reconstruct NDC position
    float2 uv  = ((float2)launchIdx + 0.5) / (float2)launchDims;
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0); // flip Y for DX

    // Unproject to world space
    float4 clipPos  = float4(ndc, depth, 1.0);
    float4 worldPos = mul(invViewProj, clipPos);
    worldPos /= worldPos.w;

    // Trace a shadow ray from world surface toward the light
    // Small bias along the geometric normal direction (use light direction as proxy)
    float3 origin    = worldPos.xyz + lightDirection.xyz * 0.005;
    float3 direction = lightDirection.xyz; // toward-light

    RayDesc ray;
    ray.Origin    = origin;
    ray.Direction = direction;
    ray.TMin      = 0.001;
    ray.TMax      = 1e4;

    ShadowPayload payload;
    payload.shadowed = false;

    TraceRay(sceneTLAS,
             RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
             0xFF,   // instance mask — all instances
             0,      // ray contribution to hit group index
             0,      // multiplier for geometry contribution to hit group index
             0,      // miss shader index
             ray,
             payload);

    shadowOutput[launchIdx] = payload.shadowed ? 0.0 : 1.0;
}

// -------------------------------------------------------------------------
// Miss shader — ray did not hit anything → fully lit
// -------------------------------------------------------------------------
[shader("miss")]
void Miss(inout ShadowPayload payload)
{
    payload.shadowed = false;
}

// -------------------------------------------------------------------------
// Closest-hit shader — ray hit geometry → shadowed
// (RAY_FLAG_SKIP_CLOSEST_HIT_SHADER skips this at runtime for perf,
//  but we declare it to satisfy the RTPSO hit group requirement)
// -------------------------------------------------------------------------
[shader("closesthit")]
void ClosestHit(inout ShadowPayload payload, BuiltInTriangleIntersectionAttributes attr)
{
    payload.shadowed = true;
}
