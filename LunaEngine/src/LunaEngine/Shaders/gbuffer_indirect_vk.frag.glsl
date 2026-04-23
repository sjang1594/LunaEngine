// gbuffer_indirect_vk.frag.glsl — Vulkan indirect G-buffer fragment shader (GLSL 4.60)
// Phase 15B: Bindless texture access via runtime arrays indexed by materialIndex.
// Requires VK_EXT_descriptor_indexing.
//
// Descriptor layout:
//   set=1, binding=0 — MaterialFactors SSBO
//   set=1, binding=1 — texture2D gAlbedoTex[]    (runtime array)
//   set=1, binding=2 — texture2D gNormalTex[]
//   set=1, binding=3 — texture2D gMetalRoughTex[]
//   set=1, binding=4 — sampler   gSampler
//
// DEBUG_SURFACE modes (pair with DEBUG_GBUFFER 1 in deferred_lighting_ibl_vk.frag.glsl):
//   0 = normal G-buffer output
//   1 = UV as color  (R=U, G=V) — verify UV interpolation
//   2 = material index as grayscale — verify bindless material lookup
//   3 = vertex normal (no normal map) — verify vertex normal data
#version 460
// Set to 1/2/3 for diagnostics, 0 for normal rendering
// 99 = flat green albedo test
#define DEBUG_SURFACE 0
#define DEBUG_MR_TO_ALBEDO 0
#extension GL_EXT_nonuniform_qualifier : require

layout(location = 0) in vec3 inPosWS;
layout(location = 1) in vec3 inNormalWS;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec3 inTangentWS;
layout(location = 4) in vec3 inBitanWS;
layout(location = 5) flat in uint inMaterialIndex;

layout(location = 0) out vec4 outAlbedo;    // RGBA8_UNORM
layout(location = 1) out vec4 outNormal;    // RGBA16F
layout(location = 2) out vec4 outMetalRough;// RGBA8_UNORM

struct MaterialFactors {
    float albedoR, albedoG, albedoB, albedoA;
    float metallicFactor;
    float roughnessFactor;
    vec2  _pad;
};

layout(set = 1, binding = 0, std430) readonly buffer MaterialBuffer {
    MaterialFactors gMaterials[];
};
layout(set = 1, binding = 1) uniform texture2D gAlbedoTex[];
layout(set = 1, binding = 2) uniform texture2D gNormalTex[];
layout(set = 1, binding = 3) uniform texture2D gMetalRoughTex[];
layout(set = 1, binding = 4) uniform sampler   gSampler;
layout(set = 1, binding = 5) uniform texture2D gEmissiveTex[];

void main()
{
    uint matIdx = inMaterialIndex;
    MaterialFactors mf = gMaterials[matIdx];

    vec4 albedoSample = texture(sampler2D(gAlbedoTex[nonuniformEXT(matIdx)],    gSampler), inUV);
    vec4 normalSample = texture(sampler2D(gNormalTex[nonuniformEXT(matIdx)],    gSampler), inUV);
    vec4 mrSample     = texture(sampler2D(gMetalRoughTex[nonuniformEXT(matIdx)],gSampler), inUV);
    vec3 emissiveSample = texture(sampler2D(gEmissiveTex[nonuniformEXT(matIdx)],gSampler), inUV).rgb;

    vec3 albedo = albedoSample.rgb * vec3(mf.albedoR, mf.albedoG, mf.albedoB);

    vec3 N = normalize(inNormalWS);
    if (any(notEqual(normalSample.xyz, vec3(0.5))))
    {
        vec3 T   = normalize(inTangentWS);
        vec3 B   = normalize(inBitanWS);
        mat3 TBN = mat3(T, B, N);
        vec3 tn  = normalSample.xyz * 2.0 - 1.0;
        N = normalize(TBN * tn);
    }

    float metallic  = mrSample.b * mf.metallicFactor;   // glTF: B=metallic, G=roughness
    float roughness = mrSample.g * mf.roughnessFactor;

    outAlbedo     = vec4(albedo, emissiveSample.r);
#if DEBUG_MR_TO_ALBEDO
    // Test C: hardcoded constant — if this shows stripes, the metalRough ATTACHMENT is the problem
    outAlbedo     = vec4(0.5, 0.3, 0.0, 1.0);
#endif
    outNormal     = vec4(N * 0.5 + 0.5, 0.0);
    outMetalRough = vec4(metallic, roughness, emissiveSample.g, emissiveSample.b);

#if DEBUG_SURFACE == 1
    // UV as color: R=U, G=V — correct UV shows smooth gradient per UV island
    outAlbedo = vec4(fract(inUV.x), fract(inUV.y), 0.0, 1.0);
#elif DEBUG_SURFACE == 2
    // Material index as grayscale — each unique material shows as different brightness
    outAlbedo = vec4(vec3(float(inMaterialIndex) * 0.2 + 0.1), 1.0);
#elif DEBUG_SURFACE == 3
    // Raw vertex normal (no normal map) — verify geometry normals
    outAlbedo = vec4(normalize(inNormalWS) * 0.5 + 0.5, 1.0);
#elif DEBUG_SURFACE == 98
    // FIXED UV texture sample — test texture binding (should be solid color = center pixel of albedo)
    outAlbedo = vec4(texture(sampler2D(gAlbedoTex[nonuniformEXT(matIdx)], gSampler), vec2(0.5, 0.5)).rgb, 1.0);
    outNormal = vec4(0.5, 0.5, 1.0, 0.0);
    outMetalRough = vec4(0.0, 0.5, 0.0, 0.0);
#elif DEBUG_SURFACE == 99
    // FLAT GREEN — test G-buffer pipeline
    outAlbedo = vec4(0.0, 1.0, 0.0, 1.0);
    outNormal = vec4(0.5, 0.5, 1.0, 0.0);
    outMetalRough = vec4(0.0, 0.5, 0.0, 0.0);
#endif
}
