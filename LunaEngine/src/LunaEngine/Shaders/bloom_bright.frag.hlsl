// bloom_bright.frag.hlsl — Phase 10: Bloom bright-pass (SM 6.0)
// Extracts over-bright pixels from TAA-resolved HDR with a soft-knee threshold.
// Renders to a half-resolution R11G11B10_FLOAT target.
//
// Root signature (RootSignatureLayout::BloomBright):
//   params[0] b0 — 4 root constants: float threshold, float knee, float2 pad
//   params[1]    — SRV table t0: TAA-resolved HDR  (R16G16B16A16_FLOAT)
//   s0           — point-clamp

#ifdef __spirv__
struct BloomBrightPush { float threshold; float knee; float2 _pad; };
[[vk::push_constant]] BloomBrightPush _pc;
#define threshold _pc.threshold
#define knee      _pc.knee
#else
cbuffer BloomBrightCB : register(b0)
{
    float threshold;
    float knee;
    float2 _pad;
};
#endif

#ifdef __spirv__
[[vk::binding(0, 0)]]
#endif
Texture2D<float4> hdrTex : register(t0);
#ifdef __spirv__
[[vk::binding(1, 0)]]
#endif
SamplerState pointClamp  : register(s0);

struct PSInput { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float4 main(PSInput input) : SV_Target
{
    float3 hdr = hdrTex.Sample(pointClamp, input.uv).rgb;
    float  lum = dot(hdr, float3(0.2126, 0.7152, 0.0722));

    // Soft-knee threshold (Jimenez 2014)
    float rq     = clamp(lum - threshold + knee, 0.0, 2.0 * knee);
    rq           = (rq * rq) / (4.0 * knee + 0.0001);
    float weight = max(rq, lum - threshold) / max(lum, 0.0001);

    return float4(hdr * weight, 1.0);
}

