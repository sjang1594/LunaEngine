// deferred_lighting_ibl.frag.hlsl — Phase 14: IBL-enhanced deferred lighting
// Extends deferred_lighting_hdr.frag.hlsl with full IBL (diffuse irradiance +
// specular split-sum) replacing the flat ambient term.
//
// Additional bindings vs. the Phase 10 HDR variant:
//   t6 = irradiance cubemap  (TextureCube, R16G16B16A16_FLOAT, 32²×6)
//   t7 = prefiltered env map  (TextureCube, R16G16B16A16_FLOAT, 128²×6 + 5 mips)
//   t8 = BRDF integration LUT (Texture2D,   RG16F, 512×512)
//   s2 = trilinear-clamp sampler (IBL)


cbuffer SceneConstants : register(b0)
{
    row_major float4x4 invViewProj;
    float3 eyePosition; float _pad0;
    float3 lightDir;    float _pad1;
    float4 lightColor;
    row_major float4x4 viewMatrix;
    row_major float4x4 lightVP[4];
    float4 cascadeSplits;
};

Texture2D             gbuffer0     : register(t0);
Texture2D             gbuffer1     : register(t1);
Texture2D             gbuffer2     : register(t2);
Texture2D<float>      depthTex     : register(t3);
Texture2DArray<float> csmShadowMap : register(t4);
Texture2D<float>      ssaoBlurTex  : register(t5);

// Phase 14: IBL textures
TextureCube<float4>   irrMap       : register(t6);
TextureCube<float4>   prefilterMap : register(t7);
Texture2D<float2>     brdfLUT      : register(t8);

SamplerState pointClamp    : register(s0);
SamplerState bilinearClamp : register(s1);
SamplerState trilinearClamp: register(s2); // IBL

static const float PI = 3.14159265359f;
static const uint  PREFILTER_MIP_COUNT = 5u;

// ---------------------------------------------------------------------------
// Cook-Torrance microfacet helpers
// ---------------------------------------------------------------------------
float D_GGX(float3 N, float3 H, float roughness)
{
    float a  = roughness * roughness;
    float a2 = a * a;
    float NdH  = max(dot(N, H), 0.0f);
    float denom = NdH * NdH * (a2 - 1.0f) + 1.0f;
    return a2 / max(PI * denom * denom, 1e-7f);
}

float G_Schlick(float NdV, float roughness)
{
    float r = roughness + 1.0f;
    float k = (r * r) / 8.0f;
    return NdV / (NdV * (1.0f - k) + k);
}

float G_SmithSchlick(float3 N, float3 V, float3 L, float roughness)
{
    float NdV = max(dot(N, V), 0.0f);
    float NdL = max(dot(N, L), 0.0f);
    return G_Schlick(NdV, roughness) * G_Schlick(NdL, roughness);
}

float3 F_Schlick(float cosTheta, float3 F0)
{
    return F0 + (1.0f - F0) * pow(max(1.0f - cosTheta, 0.0f), 5.0f);
}

float3 F_SchlickRoughness(float cosTheta, float3 F0, float roughness)
{
    // Sébastien Lagarde's roughness-aware Fresnel for IBL ambient
    return F0 + (max(float3(1.0f - roughness, 1.0f - roughness, 1.0f - roughness), F0) - F0)
              * pow(max(1.0f - cosTheta, 0.0f), 5.0f);
}

float3 ReconstructWorldPos(float2 uv, float depth)
{
    float4 ndcPos;
    ndcPos.x = uv.x * 2.0f - 1.0f;
    ndcPos.y = (1.0f - uv.y) * 2.0f - 1.0f;
    ndcPos.z = depth;
    ndcPos.w = 1.0f;
    float4 worldPos = mul(ndcPos, invViewProj);
    return worldPos.xyz / worldPos.w;
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
            float2 offset = float2(dx, dy) * texelSize;
            float  stored = csmShadowMap.Sample(pointClamp, float3(shadowUV + offset, cascadeF));
            shadow += (stored >= shadowDepth - bias) ? 1.0f : 0.0f;
        }
    }
    float centreDepth = csmShadowMap.Sample(pointClamp, float3(shadowUV, cascadeF));
    shadow += (centreDepth >= shadowDepth - bias) ? 1.0f : 0.0f;
    shadow /= 5.0f;
    return shadow;
}

// ---------------------------------------------------------------------------

struct PSInput { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float4 main(PSInput input) : SV_Target0
{
    float4 gb0Data   = gbuffer0.Sample(pointClamp, input.uv);
    float4 gb2Data   = gbuffer2.Sample(pointClamp, input.uv);
    float3 albedo    = gb0Data.rgb;
    float3 normalEnc = gbuffer1.Sample(pointClamp, input.uv).rgb;
    float2 mr        = gb2Data.rg;
    float3 emissive  = float3(gb0Data.a, gb2Data.b, gb2Data.a);
    float  depth     = depthTex.Sample(pointClamp, input.uv);

    if (depth >= 1.0f)
        return float4(0.0f, 0.0f, 0.0f, 1.0f);

    float metallic  = mr.r;
    float roughness = clamp(mr.g, 0.04f, 1.0f);

    float3 N    = normalize(normalEnc * 2.0f - 1.0f);
    float3 posWS= ReconstructWorldPos(input.uv, depth);
    float3 V    = normalize(eyePosition - posWS);
    float3 L    = normalize(lightDir);
    float3 H    = normalize(V + L);
    float3 R    = reflect(-V, N);

    albedo = pow(max(albedo, 0.0001f), 2.2f);  // sRGB → linear

    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);

    // ---- Direct lighting (Cook-Torrance) ----
    float  D = D_GGX(N, H, roughness);
    float  G = G_SmithSchlick(N, V, L, roughness);
    float3 F = F_Schlick(max(dot(H, V), 0.0f), F0);

    float3 specularDirect = (D * G * F)
                           / max(4.0f * dot(N, V) * dot(N, L), 0.001f);
    float3 kD_direct = (float3(1.0f, 1.0f, 1.0f) - F) * (1.0f - metallic);
    float3 diffuseDirect = kD_direct * albedo / PI;

    float  NdL      = max(dot(N, L), 0.0f);
    float3 radiance = lightColor.rgb * lightColor.w;

    float4 posVS = mul(float4(posWS, 1.0f), viewMatrix);
    float  viewZ = posVS.z;
    float  shadow = SampleCSMShadow(posWS, viewZ);
    float  ao = ssaoBlurTex.Sample(bilinearClamp, input.uv);

    float3 Lo = (diffuseDirect + specularDirect) * radiance * NdL * shadow;

    // Phase 14: IBL ambient (split-sum approximation)
    float  NdV      = max(dot(N, V), 0.0f);
    float3 F_ibl    = F_SchlickRoughness(NdV, F0, roughness);
    float3 kD_ibl   = (float3(1.0f, 1.0f, 1.0f) - F_ibl) * (1.0f - metallic);

    float3 irradiance    = irrMap.Sample(trilinearClamp, N).rgb;
    float3 diffuseIBL    = kD_ibl * irradiance * albedo;

    float  mipLevel         = roughness * float(PREFILTER_MIP_COUNT - 1u);
    float3 prefilteredColor = prefilterMap.SampleLevel(trilinearClamp, R, mipLevel).rgb;
    float2 brdf             = brdfLUT.Sample(bilinearClamp, float2(NdV, roughness));
    float3 specularIBL      = prefilteredColor * (F_ibl * brdf.x + brdf.y);

    float3 ambient = (diffuseIBL + specularIBL) * ao;

    float3 emissiveLinear = pow(max(emissive, 0.0001f), 2.2f);

    float3 color = ambient + Lo + emissiveLinear;

    return float4(color, 1.0f);
}

