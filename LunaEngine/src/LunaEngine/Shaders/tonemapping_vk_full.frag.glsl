// tonemapping_vk_full.frag.glsl — Full tonemap (TAA + Bloom + SSR → swapchain) (Vulkan GLSL 4.60, Phase 17)
// Paired with fullscreen.vert.hlsl: inUV at location 0.
//
// Descriptor layout (set=0):
//   binding=0 — resolvedTex  (texture2D, R16G16B16A16_SFLOAT, TAA output)
//   binding=1 — bloomTex     (texture2D, R16G16B16A16_SFLOAT, blurred bloom)
//   binding=2 — ssrTex       (texture2D, R16G16B16A16_SFLOAT, SSR contribution)
//   binding=3 — pointClamp   (sampler)
// Push constant (16B): bloomStrength, exposure, _pad
#version 460

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform texture2D resolvedTex;
layout(set = 0, binding = 1) uniform texture2D bloomTex;
layout(set = 0, binding = 2) uniform texture2D ssrTex;
layout(set = 0, binding = 3) uniform sampler   pointClamp;

layout(push_constant) uniform ToneMapPush {
    float bloomStrength;
    float exposure;
    vec2  _pad;
} ToneMapCB;

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

// Hue-preserving ACES: tone map luminance, then scale color proportionally.
vec3 ACESFilmHuePreserving(vec3 x)
{
    float lum = dot(x, vec3(0.2126, 0.7152, 0.0722));
    if (lum <= 0.0) return vec3(0);
    float toneMappedLum = ACESFilmLum(lum);
    return x * (toneMappedLum / lum);
}

// Debug: 0=normal, 1=passthrough (no ACES, no gamma — raw HDR clamped to [0,1])
#define DEBUG_TONEMAP 0

void main()
{
    vec3 hdr   = texture(sampler2D(resolvedTex, pointClamp), inUV).rgb;
    vec3 bloom = texture(sampler2D(bloomTex,    pointClamp), inUV).rgb;
    vec3 ssr   = texture(sampler2D(ssrTex,      pointClamp), inUV).rgb;

    vec3 color = (hdr + ssr + bloom * ToneMapCB.bloomStrength) * ToneMapCB.exposure;
#if DEBUG_TONEMAP == 1
    outColor = vec4(clamp(color, 0.0, 1.0), 1.0);  // raw HDR, no ACES, no gamma
#else
    color = ACESFilmHuePreserving(color);
    color = clamp(color, 0.0, 1.0);
    color = pow(max(color, 0.0), vec3(1.0 / 2.2));
    outColor = vec4(color, 1.0);
#endif
}
