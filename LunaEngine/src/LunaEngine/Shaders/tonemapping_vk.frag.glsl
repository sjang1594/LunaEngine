// tonemapping_vk.frag.glsl — HDR composite + ACES tonemapping (Vulkan GLSL 4.60, Phase 16C)
// Paired with fullscreen.vert.hlsl: inUV at location 0.
//
// Descriptor layout (set=0):
//   binding=0 — hdrTex    (texture2D, R16G16B16A16_SFLOAT)
//   binding=1 — ssrTex    (texture2D, R16G16B16A16_SFLOAT)
//   binding=2 — texSampler (sampler)
#version 460

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform texture2D hdrTex;
layout(set = 0, binding = 1) uniform texture2D ssrTex;
layout(set = 0, binding = 2) uniform sampler   texSampler;

vec3 ACESFilm(vec3 x)
{
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

float ACESFilmLum(float x)
{
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

vec3 ACESFilmHuePreserving(vec3 x)
{
    float lum = dot(x, vec3(0.2126, 0.7152, 0.0722));
    if (lum <= 0.0) return vec3(0);
    float toneMappedLum = ACESFilmLum(lum);
    return x * (toneMappedLum / lum);
}

void main()
{
    vec3 hdr = texture(sampler2D(hdrTex, texSampler), inUV).rgb;
    vec3 ssr = texture(sampler2D(ssrTex, texSampler), inUV).rgb;

    vec3 color = ACESFilmHuePreserving(hdr + ssr);
    color = clamp(color, 0.0, 1.0);
    color = pow(max(color, 0.0), vec3(1.0 / 2.2));
    outColor = vec4(color, 1.0);
}
