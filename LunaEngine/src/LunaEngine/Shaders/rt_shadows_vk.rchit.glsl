// rt_shadows_vk.rchit.glsl — Phase 18D: Vulkan RT Shadow Closest-Hit (GLSL 4.60)
// Using RAY_FLAG_SKIP_CLOSEST_HIT_SHADER in rgen, this is never invoked
// for opaque geometry. Kept for pipeline completeness.
#version 460
#extension GL_EXT_ray_tracing : require

struct ShadowPayload {
    float shadow;
};
layout(location = 0) rayPayloadInEXT ShadowPayload payload;

hitAttributeEXT vec2 attribs;

void main()
{
    payload.shadow = 0.0;
}
