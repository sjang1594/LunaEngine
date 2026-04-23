// gbuffer_vk.frag.glsl — G-buffer fill fragment shader (Vulkan GLSL 4.60)
// Paired with pbr_forward.vert.hlsl (HLSL, not converted).
// pbr_forward outputs: posWS(loc 0), normalWS(loc 1), uv(loc 2), tangentWS(loc 3), bitanWS(loc 4)
//
// Descriptor layout:
//   set=1, binding=0  — MaterialBuffer (albedo / metallic / roughness factors)
//   set=1, binding=1  — albedoTex     (texture2D)
//   set=1, binding=2  — normalTex     (texture2D)
//   set=1, binding=3  — metalRoughTex (texture2D)
//   set=1, binding=4  — linearSampler (sampler)
#version 460

layout(location = 0) in vec3 inPosWS;
layout(location = 1) in vec3 inNormalWS;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec3 inTangentWS;
layout(location = 4) in vec3 inBitanWS;

layout(location = 0) out vec4 outAlbedo;    // RGBA8_UNORM
layout(location = 1) out vec4 outNormal;    // RGBA16F  — xyz encoded [0,1]
layout(location = 2) out vec4 outMetalRough;// RGBA8_UNORM — r=metallic, g=roughness

layout(set = 1, binding = 0, std140) uniform MaterialBuffer {
    vec4  albedoFactor;
    float metallicFactor;
    float roughnessFactor;
    vec2  _pad;
};

layout(set = 1, binding = 1) uniform texture2D albedoTex;
layout(set = 1, binding = 2) uniform texture2D normalTex;
layout(set = 1, binding = 3) uniform texture2D metalRoughTex;
layout(set = 1, binding = 4) uniform sampler   linearSampler;

void main()
{
    // Albedo
    vec3 albedo = texture(sampler2D(albedoTex, linearSampler), inUV).rgb * albedoFactor.rgb;

    // Metallic / roughness (glTF packing: metallic=B, roughness=G)
    vec2 mr       = texture(sampler2D(metalRoughTex, linearSampler), inUV).bg;
    float metallic  = mr.x * metallicFactor;
    float roughness = clamp(mr.y * roughnessFactor, 0.04, 1.0);

    // Normal mapping: tangent-space → world-space
    vec3 n = texture(sampler2D(normalTex, linearSampler), inUV).xyz * 2.0 - 1.0;
    mat3 TBN    = mat3(normalize(inTangentWS),
                       normalize(inBitanWS),
                       normalize(inNormalWS));
    vec3 normalWS = normalize(TBN * n);

    outAlbedo     = vec4(albedo, 0.0);  // alpha = emissive.r (no emissive in legacy path)
    outNormal     = vec4(normalWS * 0.5 + 0.5, 0.0);
    outMetalRough = vec4(metallic, roughness, 0.0, 0.0);
}
