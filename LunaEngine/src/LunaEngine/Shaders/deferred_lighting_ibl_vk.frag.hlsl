// deferred_lighting_ibl_vk.frag.hlsl — Vulkan deferred PBR + IBL lighting (Phase 15C)
// Extends deferred_lighting_vk.frag.hlsl with full IBL ambient:
//   diffuse irradiance from irrCube + specular split-sum from prefilterCube + brdfLUT.
//
// Descriptor layout:
//   set=0, binding=0  — DeferredSceneBuffer (same as deferred_lighting_vk)
//   set=1, binding=0  — gbuffer0 (albedo)
//   set=1, binding=1  — gbuffer1 (normal)
//   set=1, binding=2  — gbuffer2 (metalRough)
//   set=1, binding=3  — depthTex
//   set=1, binding=4  — pointSampler
//   set=1, binding=5  — csmShadowMap
//   set=1, binding=6  — csmSampler
//   set=1, binding=7  — ssaoBlurTex
//   set=1, binding=8  — bilinearClamp
//   set=1, binding=9  — gEnvCube   (prefiltered env, TextureCube)
//   set=1, binding=10 — gIrrCube   (irradiance,     TextureCube)
//   set=1, binding=11 — gBrdfLUT   (BRDF split-sum, Texture2D<float2>)
//   set=1, binding=12 — gEnvSampler (trilinear clamp)
//   set=1, binding=13 — rtShadowTex (R8_UNORM, Phase 18D — PARTIALLY_BOUND when RT disabled)

[[vk::binding(0, 0)]]
cbuffer DeferredSceneBuffer : register(b0)
{
    row_major float4x4 invViewProj;
    float3 eyePosition;  float _pad0;
    float3 lightDir;     float  lightIntensity;
    float3 lightColor;   float  _pad1;
    row_major float4x4 viewMatrix;
    row_major float4x4 lightVP[4];
    float4 cascadeSplits;
    uint   rtEnabled;    uint3  _pad2;  // Phase 18D: 1 = use RT shadow mask, 0 = use CSM
};

[[vk::binding(0,  1)]] Texture2D              gbuffer0     : register(t0);
[[vk::binding(1,  1)]] Texture2D              gbuffer1     : register(t1);
[[vk::binding(2,  1)]] Texture2D              gbuffer2     : register(t2);
[[vk::binding(3,  1)]] Texture2D<float>       depthTex     : register(t3);
[[vk::binding(4,  1)]] SamplerState           pointSampler : register(s0);
[[vk::binding(5,  1)]] Texture2DArray<float>  csmShadowMap : register(t4);
[[vk::binding(6,  1)]] SamplerState           csmSampler   : register(s1);
[[vk::binding(7,  1)]] Texture2D<float>       ssaoBlurTex  : register(t5);
[[vk::binding(8,  1)]] SamplerState           bilinearClamp: register(s2);
[[vk::binding(9,  1)]] TextureCube<float4>    gEnvCube     : register(t6);
[[vk::binding(10, 1)]] TextureCube<float4>    gIrrCube     : register(t7);
[[vk::binding(11, 1)]] Texture2D<float2>      gBrdfLUT     : register(t8);
[[vk::binding(12, 1)]] SamplerState           gEnvSampler  : register(s3);
[[vk::binding(13, 1)]] Texture2D<float>       rtShadowTex  : register(t9);  // Phase 18D

static const float PI = 3.14159265359f;
static const uint  PREFILTER_MIP_COUNT = 5u;

// ---- Cook-Torrance helpers ----
float D_GGX(float3 N, float3 H, float roughness)
{
    float a  = roughness * roughness;
    float a2 = a * a;
    float NdH = max(dot(N, H), 0.0f);
    float d   = NdH * NdH * (a2 - 1.0f) + 1.0f;
    return a2 / max(PI * d * d, 1e-7f);
}

float G_Schlick(float NdV, float roughness)
{
    float r = roughness + 1.0f;
    float k = (r * r) / 8.0f;
    return NdV / (NdV * (1.0f - k) + k);
}

float G_Smith(float3 N, float3 V, float3 L, float roughness)
{
    return G_Schlick(max(dot(N, V), 0.0f), roughness)
         * G_Schlick(max(dot(N, L), 0.0f), roughness);
}

float3 F_Schlick(float cosTheta, float3 F0)
{
    return F0 + (1.0f - F0) * pow(max(1.0f - cosTheta, 0.0f), 5.0f);
}

float3 F_SchlickRoughness(float cosTheta, float3 F0, float roughness)
{
    return F0 + (max(1.0f - roughness, F0) - F0) * pow(max(1.0f - cosTheta, 0.0f), 5.0f);
}

float3 ReconstructWorldPos(float2 uv, float depth)
{
    float4 ndc;
    ndc.x = uv.x * 2.0f - 1.0f;
    ndc.y = 1.0f - uv.y * 2.0f;
    ndc.z = depth;
    ndc.w = 1.0f;
    float4 ws = mul(ndc, invViewProj);
    return ws.xyz / ws.w;
}

float SampleCSMShadow(float3 posWS, float viewSpaceZ)
{
    uint cascade = 3u;
    if      (viewSpaceZ < cascadeSplits.x) cascade = 0u;
    else if (viewSpaceZ < cascadeSplits.y) cascade = 1u;
    else if (viewSpaceZ < cascadeSplits.z) cascade = 2u;

    float4 posLS = mul(float4(posWS, 1.0f), lightVP[cascade]);
    posLS.xyz /= posLS.w;

    float2 shadowUV;
    shadowUV.x =  posLS.x * 0.5f + 0.5f;
    shadowUV.y = -posLS.y * 0.5f + 0.5f;
    float shadowDepth = posLS.z;

    if (any(shadowUV < 0.0f) || any(shadowUV > 1.0f) || shadowDepth < 0.0f || shadowDepth > 1.0f)
        return 1.0f;

    float bias      = 0.005f;
    float texelSize = 1.0f / 2048.0f;
    float shadow    = 0.0f;
    float cascadeF  = (float)cascade;

    [unroll]
    for (int dx = -1; dx <= 1; dx += 2)
    {
        [unroll]
        for (int dy = -1; dy <= 1; dy += 2)
        {
            float2 off = float2(dx, dy) * texelSize;
            float stored = csmShadowMap.Sample(csmSampler, float3(shadowUV + off, cascadeF));
            shadow += (stored >= shadowDepth - bias) ? 1.0f : 0.0f;
        }
    }
    float centreDepth = csmShadowMap.Sample(csmSampler, float3(shadowUV, cascadeF));
    shadow += (centreDepth >= shadowDepth - bias) ? 1.0f : 0.0f;
    shadow /= 5.0f;
    return shadow;
}

struct PSInput { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float4 main(PSInput input) : SV_Target0
{
    float2 uv    = input.uv;
    float  depth = depthTex.Sample(pointSampler, uv);

    if (depth >= 1.0f)
    {
        float3 ndc    = float3(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, 1.0f);
        float4 ws     = mul(float4(ndc, 1.0f), invViewProj);
        float3 skyDir = normalize(ws.xyz / ws.w - eyePosition);
        float3 sky    = gEnvCube.SampleLevel(gEnvSampler, skyDir, 0.0f).rgb * 0.8f;
        return float4(sky, 1.0f);
    }

    float3 albedo    = gbuffer0.Sample(pointSampler, uv).rgb;
    float3 normalEnc = gbuffer1.Sample(pointSampler, uv).rgb;
    float2 mr        = gbuffer2.Sample(pointSampler, uv).rg;

    float metallic  = mr.r;
    float roughness = clamp(mr.g, 0.04f, 1.0f);

    float3 N     = normalize(normalEnc * 2.0f - 1.0f);
    float3 posWS = ReconstructWorldPos(uv, depth);
    float3 V     = normalize(eyePosition - posWS);
    float3 L     = normalize(lightDir);
    float3 H     = normalize(V + L);
    float3 R     = reflect(-V, N);

    albedo = pow(max(albedo, 0.0001f), 2.2f);

    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);

    // ---- Direct lighting (Cook-Torrance) ----
    float  D        = D_GGX(N, H, roughness);
    float  G        = G_Smith(N, V, L, roughness);
    float3 F_direct = F_Schlick(max(dot(H, V), 0.0f), F0);
    float3 specular = (D * G * F_direct)
                    / max(4.0f * dot(N, V) * dot(N, L), 0.001f);
    float3 kD      = (1.0f - F_direct) * (1.0f - metallic);
    float3 diffuse = kD * albedo / PI;

    float  NdL      = max(dot(N, L), 0.0f);
    float3 radiance = lightColor * lightIntensity;

    float4 posVS  = mul(float4(posWS, 1.0f), viewMatrix);
    float  viewZ  = posVS.z;
    // Phase 18D: use RT shadow when available, otherwise fall back to CSM
    float  shadow;
    if (rtEnabled)
        shadow = rtShadowTex.Load(int3((int2)input.pos.xy, 0)).r;
    else
        shadow = SampleCSMShadow(posWS, viewZ);
    float  ao     = ssaoBlurTex.Sample(bilinearClamp, uv);

    float3 Lo = (diffuse + specular) * radiance * NdL * shadow;

    // ---- IBL ambient (split-sum) ----
    float  NdV      = max(dot(N, V), 0.0f);
    float3 F_ibl    = F_SchlickRoughness(NdV, F0, roughness);
    float3 kD_ibl   = (1.0f - F_ibl) * (1.0f - metallic);

    float3 irradiance   = gIrrCube.Sample(gEnvSampler, N).rgb;
    float3 diffuseIBL   = kD_ibl * irradiance * albedo;

    float  mipLevel         = roughness * float(PREFILTER_MIP_COUNT - 1u);
    float3 prefilteredColor = gEnvCube.SampleLevel(gEnvSampler, R, mipLevel).rgb;
    float2 brdf             = gBrdfLUT.Sample(bilinearClamp, float2(NdV, roughness));
    float3 specularIBL      = prefilteredColor * (F_ibl * brdf.x + brdf.y);

    float3 ambient = (diffuseIBL + specularIBL) * ao;
    float3 color   = ambient + Lo;

    // Output raw HDR (tone-mapping applied downstream)
    return float4(color, 1.0f);
}
