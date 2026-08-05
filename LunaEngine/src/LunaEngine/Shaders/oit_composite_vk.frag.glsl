#version 460
// oit_composite_vk.frag.glsl — Phase 31: WBOIT composite pass (Vulkan)
// Reads accum + revealage, blends transparent result onto opaque HDR.
//
// Descriptor layout:
//   set=0, binding=0 — accumTex    (sampler2D, RGBA16F)
//   set=0, binding=1 — revealageTex (sampler2D, R8_UNORM)
//
// Pipeline: no depth test, blend ONE_MINUS_SRC_ALPHA + SRC_ALPHA
//   output alpha = revealage
//   → finalHDR = compositeColor * (1-revealage) + hdr_opaque * revealage

layout(location = 0) in  vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D accumTex;
layout(set = 0, binding = 1) uniform sampler2D revealageTex;

void main()
{
    vec4  accum     = texture(accumTex,     inUV);
    float revealage = texture(revealageTex, inUV).r;

    if (revealage >= 0.9999)
        discard;

    vec3 color = accum.rgb / max(accum.a, 1e-5);

    // Output: color in RGB, revealage in alpha
    // Blend: src*(1-revealage) + dst*revealage = composite*(1-r) + opaqueHDR*r
    outColor = vec4(color, revealage);
}
