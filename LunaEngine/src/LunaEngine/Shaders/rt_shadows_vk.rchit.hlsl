// rt_shadows_vk.rchit.hlsl — Phase 18D: Vulkan RT Shadow Closest-Hit
// Using RAY_FLAG_SKIP_CLOSEST_HIT_SHADER in rgen, this stub is never
// invoked for opaque geometry. Kept for pipeline completeness.

struct ShadowPayload
{
    float shadow;
};

[shader("closesthit")]
void ClosestHit(inout ShadowPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    payload.shadow = 0.0f;  // hit something → occluded
}
