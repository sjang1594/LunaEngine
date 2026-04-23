// gbuffer_vk.frag.hlsl — G-buffer fill pixel shader (Vulkan SM 6.0)
// Writes albedo / world-space normal / metallic+roughness into 3 colour render targets.
//
// Descriptor layout (matches VulkanBackend _pipelineLayout):
//   set=0, binding=0  — TransformBuffer (dynamic UBO: model + view + proj)
//   set=0, binding=1  — SceneBuffer     (unused here, but bound to the same set)
//   set=1, binding=0  — MaterialBuffer  (albedo / metallic / roughness factors)
//   set=1, binding=1  — albedoTex
//   set=1, binding=2  — normalTex
//   set=1, binding=3  — metalRoughTex
//   set=1, binding=4  — linearSampler

[[vk::binding(0, 0)]]
cbuffer TransformBuffer : register(b0)
{
    row_major float4x4 modelMatrix;
    row_major float4x4 viewMatrix;
    row_major float4x4 projectionMatrix;
};

[[vk::binding(0, 1)]]
cbuffer MaterialBuffer : register(b1)
{
    float4 albedoFactor;
    float  metallicFactor;
    float  roughnessFactor;
    float2 _pad;
};

[[vk::binding(1, 1)]] Texture2D    albedoTex     : register(t0);
[[vk::binding(2, 1)]] Texture2D    normalTex     : register(t1);
[[vk::binding(3, 1)]] Texture2D    metalRoughTex : register(t2);
[[vk::binding(4, 1)]] SamplerState linearSampler : register(s0);

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
    float4 gb0 : SV_Target0;   // albedo.rgb        — RGBA8_UNORM
    float4 gb1 : SV_Target1;   // world normal.xyz  — RGBA16F (encoded [0,1])
    float4 gb2 : SV_Target2;   // metallic.r + roughness.g — RGBA8_UNORM
};

GBufOut main(PSInput input)
{
    // Albedo
    float3 albedo = albedoTex.Sample(linearSampler, input.uv).rgb * albedoFactor.rgb;

    // Metallic / roughness  (gltf packing: metallic=B, roughness=G)
    float2 mr       = metalRoughTex.Sample(linearSampler, input.uv).bg;
    float metallic  = mr.x * metallicFactor;
    float roughness = clamp(mr.y * roughnessFactor, 0.04, 1.0);

    // Normal mapping: tangent-space → world-space
    float3 n = normalTex.Sample(linearSampler, input.uv).xyz * 2.0 - 1.0;
    float3x3 TBN    = float3x3(normalize(input.tangentWS),
                                normalize(input.bitanWS),
                                normalize(input.normalWS));
    float3 normalWS = normalize(mul(n, TBN));

    GBufOut o;
    o.gb0 = float4(albedo, 1.0);
    o.gb1 = float4(normalWS * 0.5 + 0.5, 0.0);   // encode to [0,1] for RGBA8/16F
    o.gb2 = float4(metallic, roughness, 0.0, 0.0);
    return o;
}

