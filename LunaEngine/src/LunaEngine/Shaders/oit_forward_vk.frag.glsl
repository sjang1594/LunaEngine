#version 460
// oit_forward_vk.frag.glsl — Phase 31: WBOIT accumulation pass (Vulkan)
// McGuire & Bavoil 2013 Weighted Blended OIT.
//
// MRT output:
//   location=0 (RGBA16_SFLOAT, blend ONE+ONE):      accum = color*alpha*w, accum.a = alpha*w
//   location=1 (R8_UNORM,      blend ZERO+SRC_COLOR): revealage = 1-alpha → dst *= src
//
// Push constants (80B):
//   model[16] (64B) + alpha (4B) + _pad[3] (12B)
// Descriptor layout:
//   set=0, binding=0 — OITSceneUBO (view, proj, lightDir, lightColor)
//   set=1, binding=0 — albedoTex (combined image sampler)

layout(location = 0) in  vec3 inPosWS;
layout(location = 1) in  vec3 inNormalWS;
layout(location = 2) in  vec2 inUV;

layout(location = 0) out vec4  outAccum;      // RGBA16F — weighted color accumulation
layout(location = 1) out float outRevealage;  // R8_UNORM — 1-alpha (multiplicative coverage)

layout(push_constant) uniform PC {
    layout(row_major) mat4 model;  // unused in FS but must match VS layout
    float alpha;
    float _pad[3];
} pc;

layout(set = 0, binding = 0, std140) uniform OITSceneUBO {
    layout(row_major) mat4 viewMatrix;
    layout(row_major) mat4 projMatrix;
    vec3  lightDir;   float _p0;
    vec4  lightColor;
} scene;

layout(set = 1, binding = 0) uniform sampler2D albedoTex;

void main()
{
    vec4  samp  = texture(albedoTex, inUV);
    float alpha = pc.alpha * samp.a;
    vec3  color = samp.rgb;

    // Simple directional light
    vec3  N   = normalize(inNormalWS);
    vec3  L   = normalize(-scene.lightDir);
    float NdL = max(dot(N, L), 0.0);
    color = color * scene.lightColor.rgb * (0.1 + 0.9 * NdL);

    // WBOIT depth-based weight (McGuire 2013 eq. 10)
    float z = gl_FragCoord.z;
    float w = alpha * max(1e-2, min(3e3, 0.03 / (1e-5 + pow(z / 200.0, 4.0))));

    outAccum     = vec4(color * alpha * w, alpha * w);
    outRevealage = 1.0 - alpha;  // blend ZERO+SRC_COLOR: dst.r *= src.r
}
