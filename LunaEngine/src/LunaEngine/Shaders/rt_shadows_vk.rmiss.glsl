// rt_shadows_vk.rmiss.glsl — Phase 18D: Vulkan RT Shadow Miss Shader (GLSL 4.60)
// Ray missed all geometry → surface point is lit.
#version 460
#extension GL_EXT_ray_tracing : require

struct ShadowPayload {
    float shadow;
};
layout(location = 0) rayPayloadInEXT ShadowPayload payload;

void main()
{
    payload.shadow = 1.0;
}
