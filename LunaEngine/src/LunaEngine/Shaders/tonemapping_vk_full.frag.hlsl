// tonemapping_vk_full.frag.hlsl — Phase 17: Full tonemap (TAA + Bloom + SSR → swapchain)
// Reads TAA-resolved output (t0), blurred bloom (t1), SSR contribution (t2).
// Applies ACES filmic tonemapping and gamma correction.
//
// Descriptor layout (set=0):
//   binding=0 — resolvedTex  (SAMPLED_IMAGE, R16G16B16A16_SFLOAT, TAA output)
//   binding=1 — bloomTex     (SAMPLED_IMAGE, R16G16B16A16_SFLOAT, blurred bloom)
//   binding=2 — ssrTex       (SAMPLED_IMAGE, R16G16B16A16_SFLOAT, SSR contribution)
//   binding=3 — pointClamp   (SAMPLER)
// Push constant (16B):
//   float bloomStrength, float exposure, float2 _pad

[[vk::binding(0, 0)]] Texture2D<float4> resolvedTex : register(t0);
[[vk::binding(1, 0)]] Texture2D<float4> bloomTex    : register(t1);
[[vk::binding(2, 0)]] Texture2D<float4> ssrTex      : register(t2);
[[vk::binding(3, 0)]] SamplerState      pointClamp  : register(s0);

struct ToneMapPush { float bloomStrength; float exposure; float2 _pad; };
[[vk::push_constant]] ToneMapPush ToneMapCB;

struct PSInput
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

// ACES filmic curve approximation (Narkowicz 2015)
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

float4 main(PSInput input) : SV_Target0
{
    float3 hdr   = resolvedTex.Sample(pointClamp, input.uv).rgb;
    float3 bloom = bloomTex.Sample(pointClamp, input.uv).rgb;
    float3 ssr   = ssrTex.Sample(pointClamp, input.uv).rgb;

    float3 color = (hdr + ssr + bloom * ToneMapCB.bloomStrength) * ToneMapCB.exposure;
    color = ACESFilmHuePreserving(color);
    color = saturate(color);
    color = pow(max(color, 0.0f), 1.0f / 2.2f);
    return float4(color, 1.0f);
}
