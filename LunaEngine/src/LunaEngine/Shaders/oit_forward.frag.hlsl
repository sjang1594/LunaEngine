// oit_forward.frag.hlsl — Phase 31: WBOIT accumulation pass (DX12)
// McGuire & Bavoil 2013 Weighted Blended OIT.
//
// MRT output:
//   SV_Target0 (RGBA16F, blend ONE+ONE):       accum.rgb = color*alpha*w, accum.a = alpha*w
//   SV_Target1 (R8_UNORM, blend ZERO+SRC_COLOR): revealage = 1-alpha  → dst *= src
//
// Root signature (OITForward):
//   b0 (ALL) — OITTransform CBV: model/view/proj + alpha (read alpha from offset 192)
//   b1 (PS)  — SceneConstants CBV (eyePos, lightDir, lightColor)
//   t0 (PS)  — albedoTex
//   s0       — linear-clamp sampler
//
// Pipeline: depth test ON (LESS_EQUAL), depth write OFF, MRT blend as above.

cbuffer OITTransform : register(b0)
{
    row_major float4x4 modelMatrix; // unused in PS
    row_major float4x4 viewMatrix;  // unused in PS
    row_major float4x4 projMatrix;  // unused in PS
    float              u_alpha;
    float3             _tpad;
};

cbuffer SceneConstants : register(b1)
{
    row_major float4x4 invViewProj;
    float3 eyePosition; float _s0;
    float3 lightDir;    float _s1;
    float4 lightColor;
};

Texture2D<float4> albedoTex    : register(t0);
SamplerState      linearSampler : register(s0);

struct PSIn
{
    float4 posCS    : SV_POSITION;
    float3 posWS    : POSITION;
    float3 normalWS : NORMAL;
    float2 uv       : TEXCOORD0;
};

struct PSOut
{
    float4 accum     : SV_Target0;  // RGBA16F — weighted color accumulation
    float  revealage : SV_Target1;  // R8_UNORM — transparency coverage
};

PSOut main(PSIn input)
{
    float4 albedo = albedoTex.Sample(linearSampler, input.uv);
    float  alpha  = u_alpha * albedo.a;
    float3 color  = albedo.rgb;

    // Simple directional light
    float3 N   = normalize(input.normalWS);
    float3 L   = normalize(-lightDir);
    float  NdL = max(dot(N, L), 0.0);
    color = color * lightColor.rgb * (0.1 + 0.9 * NdL);

    // WBOIT depth-based weight (McGuire 2013 eq. 10)
    float z = input.posCS.z;
    float w = alpha * max(1e-2, min(3e3, 0.03 / (1e-5 + pow(z / 200.0, 4.0))));

    PSOut o;
    o.accum     = float4(color * alpha * w, alpha * w);
    o.revealage = 1.0 - alpha;  // blend ZERO+SRC_COLOR: dst.r *= src.r
    return o;
}
