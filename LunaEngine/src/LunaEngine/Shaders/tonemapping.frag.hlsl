// tonemapping.frag.hlsl — Phase 10: ACES filmic tone mapping + bloom composite (SM 6.0)
// Reads TAA-resolved HDR (t0) and blurred bloom (t1), adds bloom, applies ACES
// approximation, and outputs sRGB to the LDR back buffer.
//
// Root signature (RootSignatureLayout::ToneMap):
//   params[0] b0 — 4 root constants: float bloomStrength, float exposure, float2 pad
//   params[1]    — SRV table t0: TAA-resolved HDR  (R16G16B16A16_FLOAT)
//   params[2]    — SRV table t1: blurred bloom      (R11G11B10_FLOAT)
//   s0           — point-clamp


cbuffer ToneMapCB : register(b0)
{
    float bloomStrength;
    float exposure;
    float2 _pad;
};

#ifdef __spirv__
[[vk::binding(0, 0)]]
#endif
Texture2D<float4> resolvedTex : register(t0);
#ifdef __spirv__
[[vk::binding(1, 0)]]
#endif
Texture2D<float4> bloomTex    : register(t1);
#ifdef __spirv__
[[vk::binding(2, 0)]]
#endif
SamplerState      pointClamp  : register(s0);

struct PSInput { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

// ACES filmic curve approximation (Narkowicz 2015)
float3 ACESFilm(float3 x)
{
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

// Single-channel ACES for luminance-based approach
float ACESFilmLum(float x)
{
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

// Hue-preserving ACES: tone map luminance, then scale color proportionally.
// This prevents the per-channel ACES from shifting saturated golds → green.
float3 ACESFilmHuePreserving(float3 x)
{
    float lum = dot(x, float3(0.2126, 0.7152, 0.0722));
    if (lum <= 0.0) return float3(0, 0, 0);
    float toneMappedLum = ACESFilmLum(lum);
    return x * (toneMappedLum / lum);
}

float4 main(PSInput input) : SV_Target
{
    float3 hdr   = resolvedTex.Sample(pointClamp, input.uv).rgb;
    float3 bloom = bloomTex.Sample(pointClamp, input.uv).rgb;

    float3 color = (hdr + bloom * bloomStrength) * exposure;
    color = ACESFilmHuePreserving(color);
    color = saturate(color);
    color = pow(max(color, 0.0), 1.0 / 2.2);

    return float4(color, 1.0);
}

