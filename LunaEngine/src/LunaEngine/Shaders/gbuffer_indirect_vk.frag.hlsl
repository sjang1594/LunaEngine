// gbuffer_indirect_vk.frag.hlsl — Vulkan indirect G-buffer pixel shader (SM 6.0)
// Phase 15B: Bindless texture access via runtime arrays indexed by materialIndex.
// Requires VK_EXT_descriptor_indexing (runtimeDescriptorArray + NonUniformIndexing).
//
// Descriptor layout:
//   set=1, binding=0 — StructuredBuffer<MaterialFactors> gMaterials
//   set=1, binding=1 — Texture2D gAlbedoTex[]     (runtime array)
//   set=1, binding=2 — Texture2D gNormalTex[]
//   set=1, binding=3 — Texture2D gMetalRoughTex[]
//   set=1, binding=4 — SamplerState gSampler

struct MaterialFactors
{
    float albedoR, albedoG, albedoB, albedoA;
    float metallicFactor;
    float roughnessFactor;
    float2 _pad;
};

[[vk::binding(0, 1)]] StructuredBuffer<MaterialFactors> gMaterials  : register(t0, space1);
[[vk::binding(1, 1)]] Texture2D                         gAlbedoTex[]     : register(t1, space1);
[[vk::binding(2, 1)]] Texture2D                         gNormalTex[]     : register(t2, space1);
[[vk::binding(3, 1)]] Texture2D                         gMetalRoughTex[] : register(t3, space1);
[[vk::binding(4, 1)]] SamplerState                      gSampler         : register(s0, space1);

struct PSInput
{
    float4 posCS                          : SV_POSITION;
    float3 posWS                          : POSITION;
    float3 normalWS                       : NORMAL;
    float2 uv                             : TEXCOORD0;
    float3 tangentWS                      : TANGENT;
    float3 bitanWS                        : BITANGENT;
    nointerpolation uint materialIndex    : MATERIAL_INDEX;
};

struct PSOutput
{
    float4 albedo    : SV_Target0;   // RGBA8_UNORM
    float4 normal    : SV_Target1;   // RGBA16F — xyz encoded [0,1]
    float4 metalRough: SV_Target2;   // RGBA8 — r=metallic, g=roughness
};

PSOutput main(PSInput input)
{
    PSOutput output;

    uint matIdx = input.materialIndex;
    MaterialFactors mf = gMaterials[matIdx];

    // Sample textures via NonUniformResourceIndex for correct SPIR-V decoration
    float4 albedoSample    = gAlbedoTex[NonUniformResourceIndex(matIdx)].Sample(gSampler, input.uv);
    float4 normalSample    = gNormalTex[NonUniformResourceIndex(matIdx)].Sample(gSampler, input.uv);
    float4 mrSample        = gMetalRoughTex[NonUniformResourceIndex(matIdx)].Sample(gSampler, input.uv);

    // Modulate with material factors
    float3 albedo = albedoSample.rgb * float3(mf.albedoR, mf.albedoG, mf.albedoB);

    // Normal mapping: decode normal from normal map and apply TBN
    float3 N = normalize(input.normalWS);
    if (any(normalSample.xyz != 0.5))
    {
        float3 T = normalize(input.tangentWS);
        float3 B = normalize(input.bitanWS);
        float3x3 TBN = float3x3(T, B, N);
        float3 tn = normalSample.xyz * 2.0 - 1.0;
        N = normalize(mul(tn, TBN));
    }

    float metallic  = mrSample.b * mf.metallicFactor;   // glTF: B=metallic, G=roughness
    float roughness = mrSample.g * mf.roughnessFactor;

    // Encode normal to [0,1] range
    output.albedo     = float4(albedo, 1.0);
    output.normal     = float4(N * 0.5 + 0.5, 0.0);
    output.metalRough = float4(metallic, roughness, 0.0, 0.0);

    return output;
}
