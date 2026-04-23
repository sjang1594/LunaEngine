// gbuffer.frag.hlsl — G-buffer fill pixel shader (SM 6.0)
// Phase 11: Bindless textures — single Texture2D[] heap in space1, indexed by gMaterialIndex.
// Root signature: b0=MVP, b1=MaterialConstants, b2=gMaterialIndex (1 DWORD root constant),
//                 t0+ space1=unbounded SRV table (whole _imGuiSrvHeap), s0=static anisotropic.
// No per-draw descriptor table switch; albedo/normal/metalRough at gMaterialIndex+0/1/2.

cbuffer TransformBuffer : register(b0)
{
    row_major float4x4 modelMatrix;
    row_major float4x4 viewMatrix;
    row_major float4x4 projectionMatrix;
};

cbuffer MaterialBuffer : register(b1)
{
    float4 albedoFactor;
    float  metallicFactor;
    float  roughnessFactor;
    float2 _pad;
};

// Phase 11: material base index into the bindless heap — injected as a 1-DWORD root constant at b2.
cbuffer MaterialIndex : register(b2)
{
    uint gMaterialIndex;
};

// Bindless heap: entire _imGuiSrvHeap exposed as an unbounded Texture2D array in space1.
// space1 avoids register clash with the deferred lighting pass t0-t5 in space0.
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
    float4 gb0 : SV_Target0;  // albedo.rgb (RGBA8)
    float4 gb1 : SV_Target1;  // world-space normal.xyz (RGBA16F)
    float4 gb2 : SV_Target2;  // metallic.r + roughness.g (RGBA8)
};

GBufOut main(PSInput input)
{
    // Bindless reads: heap slots [base+0]=albedo, [base+1]=normalMap, [base+2]=metalRough
    float3 albedo     = gAllTextures[gMaterialIndex + 0].Sample(linearSampler, input.uv).rgb
                        * albedoFactor.rgb;

    float2 metalRough = gAllTextures[gMaterialIndex + 2].Sample(linearSampler, input.uv).bg; // b=metallic, g=roughness
    float  metallic   = metalRough.x * metallicFactor;
    float  roughness  = clamp(metalRough.y * roughnessFactor, 0.04, 1.0);

    // Normal mapping: tangent-space → world-space
    float3 n = gAllTextures[gMaterialIndex + 1].Sample(linearSampler, input.uv).xyz;
    n = n * 2.0 - 1.0;
    float3x3 TBN    = float3x3(normalize(input.tangentWS),
                                normalize(input.bitanWS),
                                normalize(input.normalWS));
    float3 normalWS = normalize(mul(n, TBN));

    // Emissive: packed into unused alpha channels (GB0.a = emissive.r, GB2.ba = emissive.gb)
    float3 emissive = gAllTextures[gMaterialIndex + 3].Sample(linearSampler, input.uv).rgb;

    GBufOut o;
    o.gb0 = float4(albedo, emissive.r);
    o.gb1 = float4(normalWS * 0.5 + 0.5, 0.0);
    o.gb2 = float4(metallic, roughness, emissive.g, emissive.b);
    return o;
}
