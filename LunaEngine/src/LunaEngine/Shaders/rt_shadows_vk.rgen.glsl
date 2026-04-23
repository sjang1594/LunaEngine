// rt_shadows_vk.rgen.glsl — Phase 18D: Vulkan Ray Tracing Shadow RayGen (GLSL 4.60)
// Reconstructs world position from GBuffer depth, traces a shadow ray
// toward the directional light, writes occlusion to shadowMask storage image.
//
// Bindings (set=0):
//   binding=0 — accelerationStructureEXT tlas
//   binding=1 — image2D shadowMask (r8, STORAGE_IMAGE)
//   binding=2 — sampler2D depthTex  (COMBINED_IMAGE_SAMPLER, for texelFetch)
//   binding=3 — sampler2D normalTex (COMBINED_IMAGE_SAMPLER, for texelFetch)
//   binding=4 — SceneRT UBO (invViewProj, lightDir, maxDist)
#version 460
#extension GL_EXT_ray_tracing : require

layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;
layout(set = 0, binding = 1, r8) uniform image2D shadowMask;
layout(set = 0, binding = 2) uniform sampler2D depthTex;
layout(set = 0, binding = 3) uniform sampler2D normalTex;

layout(set = 0, binding = 4, std140) uniform SceneRT {
    layout(row_major) mat4 invViewProj;
    vec3  lightDir;
    float maxDist;
};

struct ShadowPayload {
    float shadow;
};
layout(location = 0) rayPayloadEXT ShadowPayload payload;

void main()
{
    uvec2 launchIndex = gl_LaunchIDEXT.xy;
    uvec2 launchDim   = gl_LaunchSizeEXT.xy;

    vec2 uv = (vec2(launchIndex) + 0.5) / vec2(launchDim);

    float d = texelFetch(depthTex, ivec2(launchIndex), 0).r;

    if (d >= 1.0)
    {
        imageStore(shadowMask, ivec2(launchIndex), vec4(1.0));
        return;
    }

    vec4 ndcPos  = vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, d, 1.0);
    vec4 worldH  = ndcPos * invViewProj;
    vec3 worldPos = worldH.xyz / worldH.w;

    vec3 n = texelFetch(normalTex, ivec2(launchIndex), 0).xyz * 2.0 - 1.0;
    worldPos += n * 0.005;

    payload.shadow = 1.0;

    traceRayEXT(
        tlas,
        gl_RayFlagsOpaqueEXT | gl_RayFlagsSkipClosestHitShaderEXT,
        0xFF,
        0,
        1,
        0,
        worldPos,
        0.001,
        lightDir,
        maxDist,
        0);

    imageStore(shadowMask, ivec2(launchIndex), vec4(payload.shadow));
}
