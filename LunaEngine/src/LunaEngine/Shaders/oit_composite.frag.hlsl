// oit_composite.frag.hlsl — Phase 31: WBOIT composite pass (DX12)
// Fullscreen pass: reads accum + revealage, blends transparent result onto opaque HDR.
//
// Root signature (OITComposite):
//   t0 — accumTex    (Texture2D<float4>, RGBA16F)
//   t1 — revealageTex (Texture2D<float>,  R8_UNORM)
//   s0 — point-clamp sampler
//
// Pipeline: no depth test, blend ONE_MINUS_SRC_ALPHA + SRC_ALPHA
//   output alpha = revealage
//   → finalHDR = compositeColor * (1-revealage) + hdr_opaque * revealage

Texture2D<float4> accumTex     : register(t0);
Texture2D<float>  revealageTex : register(t1);
SamplerState      pointClamp   : register(s0);

struct PSIn
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

float4 main(PSIn input) : SV_Target
{
    float4 accum     = accumTex.Sample(pointClamp, input.uv);
    float  revealage = revealageTex.Sample(pointClamp, input.uv).r;

    // No transparent coverage — skip to avoid divide-by-zero affecting HDR
    if (revealage >= 0.9999)
        discard;

    float3 color = accum.rgb / max(accum.a, 1e-5);

    // Output: color in RGB, revealage in alpha
    // Blend: src * (1-revealage) + dst * revealage
    //      = compositeColor * (1-r) + opaqueHDR * r
    return float4(color, revealage);
}
