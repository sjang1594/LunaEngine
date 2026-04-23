// ssr_blend.frag.hlsl — Additive blend SSR result into _hdrRT (DX12, Phase 16B)
// Root: SSRBlend
//   params[0] = SRV t0 (ssrTex)
//   s0 = point-clamp
// Blend state: additive (ONE, ONE, ADD) — set in DX12Pipeline for SSRBlend layout.

Texture2D<float4> ssrTex    : register(t0);
SamplerState      pointClamp : register(s0);

struct PSInput
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target0
{
    return ssrTex.Sample(pointClamp, input.uv);
}
