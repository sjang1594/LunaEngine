#version 460
// vol_apply_vk.frag.glsl — Phase 29: Volumetric Fog — Apply Pass (Vulkan)
// Fullscreen pass: samples froxel accumulation volume at each pixel's depth.
// Outputs in-scattering for additive blend onto HDR image.
//
// set=0, binding=0: VolumetricApplyParams push constant / UBO
// set=0, binding=1: depthTex (sampler2D)
// set=0, binding=2: froxelAccum (sampler3D)

layout(location = 0) in  vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0, std140) uniform VolumetricApplyParams
{
    float nearZ;
    float farZ;
    float _pad0;
    float _pad1;
} params;

layout(set = 0, binding = 1) uniform sampler2D depthTex;
layout(set = 0, binding = 2) uniform sampler3D froxelAccum;

const uint FROXEL_Z = 64u;

// Reconstruct positive linear view-space depth from NDC depth [0,1]
float linearDepth(float ndcZ)
{
    return (params.farZ * params.nearZ) / (params.farZ - ndcZ * (params.farZ - params.nearZ));
}

float froxelZFromViewDepth(float viewDepth)
{
    float logRatio = log(params.farZ / params.nearZ);
    float slice    = log(viewDepth / params.nearZ) / logRatio;
    return clamp(slice, 0.0, 1.0);
}

void main()
{
    // ── Debug Stage 1: comment in one line at a time ──────────────────────
    // Debug stages (commented out)
    // outColor = vec4(0.5, 0.2, 0.0, 0.0); return;  // Stage 1: constant
    // vec4 dbg = texture(froxelAccum, vec3(inUV.x, inUV.y, 0.5)); outColor = vec4(dbg.rgb * 20.0, 0.0); return;
    // ─────────────────────────────────────────────────────────────────────

    float ndcDepth  = texture(depthTex, inUV).r;
    float viewDepth = linearDepth(ndcDepth);

    float froxelW = froxelZFromViewDepth(clamp(viewDepth, params.nearZ, params.farZ));
    vec3  uvw     = vec3(inUV.x, inUV.y, froxelW);

    vec4 fog = texture(froxelAccum, uvw);

    outColor = vec4(fog.rgb, 0.0);
}
