// rt_shadows_vk.rmiss.hlsl — Phase 18D: Vulkan RT Shadow Miss Shader
// Ray missed all geometry → surface point is lit.

struct ShadowPayload
{
    float shadow;
};

[shader("miss")]
void Miss(inout ShadowPayload payload)
{
    payload.shadow = 1.0f;
}
