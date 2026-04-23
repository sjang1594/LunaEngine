// rt_shadows_vk.rgen.hlsl — Phase 18D: Vulkan Ray Tracing Shadow RayGen
// Reconstructs world position from GBuffer depth, traces a shadow ray
// toward the directional light, writes occlusion to shadowMask UAV.
// Compiled with: dxc -T lib_6_5 -spirv

[[vk::binding(0, 0)]]
RaytracingAccelerationStructure tlas : register(t0);

[[vk::binding(1, 0)]] [[vk::image_format("r8")]]
RWTexture2D<float> shadowMask : register(u0);

[[vk::binding(2, 0)]]
Texture2D<float>  depthTex   : register(t1);

[[vk::binding(3, 0)]]
Texture2D<float4> normalTex  : register(t2);

[[vk::binding(4, 0)]]
cbuffer SceneRT : register(b0)
{
    float4x4 invViewProj;
    float3   lightDir;    // normalized, toward light
    float    maxDist;
};

struct ShadowPayload
{
    float shadow; // 1.0 = lit, 0.0 = occluded
};

[shader("raygeneration")]
void RayGen()
{
    uint2 launchIndex = DispatchRaysIndex().xy;
    uint2 launchDim   = DispatchRaysDimensions().xy;

    float2 uv = (float2(launchIndex) + 0.5f) / float2(launchDim);

    float d = depthTex.Load(int3(launchIndex, 0)).r;

    // Sky pixels (depth == 1.0 in reversed-Z or 0.0 in standard) — fully lit
    if (d >= 1.0f)
    {
        shadowMask[launchIndex] = 1.0f;
        return;
    }

    // Reconstruct world position
    float4 ndcPos = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, d, 1.0f);
    float4 worldH = mul(ndcPos, invViewProj);
    float3 worldPos = worldH.xyz / worldH.w;

    // Offset along normal to avoid self-intersection
    float3 n = normalTex.Load(int3(launchIndex, 0)).xyz * 2.0f - 1.0f;
    worldPos += n * 0.005f;

    // Trace shadow ray
    RayDesc ray;
    ray.Origin    = worldPos;
    ray.Direction = lightDir;
    ray.TMin      = 0.001f;
    ray.TMax      = maxDist;

    ShadowPayload payload;
    payload.shadow = 1.0f; // miss shader sets to 1.0, hit shader sets to 0.0

    TraceRay(tlas,
        RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
        0xFF,      // instance mask
        0,         // hit group offset
        1,         // geometry multiplier
        0,         // miss shader index
        ray,
        payload);

    shadowMask[launchIndex] = payload.shadow;
}
