// gbuffer_mesh.frag.hlsl — Pixel shader for mesh shader G-buffer fill (SM 6.5)
// Phase 25: Same G-buffer output as gbuffer.frag.hlsl but uses mesh shader root sig.
// Material index is passed via root constant b1; bindless heap at t0+ space1.

cbuffer MaterialConstants : register(b1)
{
    float4 albedoFactor;
    float  metallicFactor;
    float  roughnessFactor;
    float2 _pad;
};

cbuffer MaterialIndex : register(b2)
{
    uint gMaterialIndex;
};

Texture2D<float4> gAllTextures[] : register(t0, space1);

SamplerState linearSampler : register(s0);

struct PSInput
{
    float4 posCS     : SV_POSITION;
    float3 posWS     : POSITION;
    float3 normalWS  : NORMAL;
    float2 uv        : TEXCOORD0;
    float3 tangentWS : TANGENT;
    float3 bitanWS   : BITANGENT;
};

struct GBufOut
{
    float4 gb0 : SV_Target0;  // albedo.rgb + emissive.r
    float4 gb1 : SV_Target1;  // world-space normal.xyz
    float4 gb2 : SV_Target2;  // metallic.r + roughness.g + emissive.gb
};

GBufOut main(PSInput input)
{
    float3 albedo     = gAllTextures[gMaterialIndex + 0].Sample(linearSampler, input.uv).rgb
                        * albedoFactor.rgb;

    float2 metalRough = gAllTextures[gMaterialIndex + 2].Sample(linearSampler, input.uv).bg;
    float  metallic   = metalRough.x * metallicFactor;
    float  roughness  = clamp(metalRough.y * roughnessFactor, 0.04, 1.0);

    float3 n = gAllTextures[gMaterialIndex + 1].Sample(linearSampler, input.uv).xyz;
    n = n * 2.0 - 1.0;
    float3x3 TBN    = float3x3(normalize(input.tangentWS),
                                normalize(input.bitanWS),
                                normalize(input.normalWS));
    float3 normalWS = normalize(mul(n, TBN));

    float3 emissive = gAllTextures[gMaterialIndex + 3].Sample(linearSampler, input.uv).rgb;

    GBufOut o;
    o.gb0 = float4(albedo, emissive.r);
    o.gb1 = float4(normalWS * 0.5 + 0.5, 0.0);
    o.gb2 = float4(metallic, roughness, emissive.g, emissive.b);
    return o;
}

