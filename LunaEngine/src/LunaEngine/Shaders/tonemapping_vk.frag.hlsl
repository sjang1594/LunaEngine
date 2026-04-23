// tonemapping_vk.frag.hlsl — HDR composite + ACES tonemapping (Vulkan, Phase 16C)
// Reads _hdrImage (deferred lighting output) + _ssrImage (SSR contribution),
// applies ACES filmic tonemapping and gamma correction, writes to swapchain.
//
// Descriptor layout (set=0):
//   binding=0 — hdrTex    (SAMPLED_IMAGE, R16G16B16A16_SFLOAT)
//   binding=1 — ssrTex    (SAMPLED_IMAGE, R16G16B16A16_SFLOAT, or clear black if SSR disabled)
//   binding=2 — texSampler (SAMPLER)

[[vk::binding(0, 0)]] Texture2D<float4> hdrTex     : register(t0);
[[vk::binding(1, 0)]] Texture2D<float4> ssrTex     : register(t1);
[[vk::binding(2, 0)]] SamplerState      texSampler : register(s0);

// Narkowicz 2015 ACES approximation
float3 ACESFilm(float3 x)
{
    const float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float ACESFilmLum(float x)
{
    const float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

// Hue-preserving ACES: tone map luminance, then scale color proportionally.
float3 ACESFilmHuePreserving(float3 x)
{
    float lum = dot(x, float3(0.2126f, 0.7152f, 0.0722f));
    if (lum <= 0.0f) return float3(0, 0, 0);
    float toneMappedLum = ACESFilmLum(lum);
    return x * (toneMappedLum / lum);
}

struct PSInput
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target0
{
    float3 hdr = hdrTex.Sample(texSampler, input.uv).rgb;
    float3 ssr = ssrTex.Sample(texSampler, input.uv).rgb;

    float3 color = ACESFilmHuePreserving(hdr + ssr);
    color = saturate(color);
    color = pow(max(color, 0.0f), 1.0f / 2.2f);
    return float4(color, 1.0f);
}
