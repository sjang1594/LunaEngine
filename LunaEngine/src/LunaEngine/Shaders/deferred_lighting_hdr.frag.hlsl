// deferred_lighting_hdr.frag.hlsl — Phase 10 variant of deferred_lighting.frag.hlsl
// Identical root signature and G-buffer reads, but outputs raw linear HDR to an
// R16G16B16A16_FLOAT target instead of applying Reinhard + gamma in-shader.
// Tone mapping is handled by the separate tonemapping.frag.hlsl pass.
//
// Phase 9: SSAO blur result at t5 multiplies the ambient term.
// Phase 8: CSM Texture2DArray at t4 for shadow.


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

SamplerState pointClamp    : register(s0);
SamplerState bilinearClamp : register(s1);

static const float PI = 3.14159265359;

float D_GGX(float3 N, float3 H, float roughness)
{
    float a  = roughness * roughness;
    float a2 = a * a;
    float NdH  = max(dot(N, H), 0.0);
    float NdH2 = NdH * NdH;
    float denom = NdH2 * (a2 - 1.0) + 1.0;
    return a2 / (PI * denom * denom);
}

float G_Schlick(float NdV, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdV / (NdV * (1.0 - k) + k);
}

float G_SmithSchlick(float3 N, float3 V, float3 L, float roughness)
{
    float NdV = max(dot(N, V), 0.0);
    float NdL = max(dot(N, L), 0.0);
    return G_Schlick(NdV, roughness) * G_Schlick(NdL, roughness);
}

float3 F_Schlick(float cosTheta, float3 F0)
{
    return F0 + (1.0 - F0) * pow(max(1.0 - cosTheta, 0.0), 5.0);
}

float3 ReconstructWorldPos(float2 uv, float depth)
{
    float4 ndcPos;
    ndcPos.x = uv.x * 2.0 - 1.0;
    ndcPos.y = (1.0 - uv.y) * 2.0 - 1.0;
    ndcPos.z = depth;
    ndcPos.w = 1.0;
    float4 worldPos = mul(ndcPos, invViewProj);
    return worldPos.xyz / worldPos.w;
}

float SampleCSMShadow(float3 posWS, float viewSpaceZ)
{
    uint cascade = 3u;
    if      (viewSpaceZ < cascadeSplits.x) cascade = 0u;
    else if (viewSpaceZ < cascadeSplits.y) cascade = 1u;
    else if (viewSpaceZ < cascadeSplits.z) cascade = 2u;

    float4 posLS = mul(float4(posWS, 1.0), lightVP[cascade]);
    posLS.xyz /= posLS.w;

    float2 shadowUV;
    shadowUV.x =  posLS.x * 0.5 + 0.5;
    shadowUV.y = -posLS.y * 0.5 + 0.5;
    float shadowDepth = posLS.z;

    if (any(shadowUV < 0.0) || any(shadowUV > 1.0) || shadowDepth < 0.0 || shadowDepth > 1.0)
        return 1.0;

    float bias      = 0.005;
    float texelSize = 1.0 / 2048.0;
    float shadow    = 0.0;
    float cascadeF  = (float)cascade;

    [unroll]
    for (int dx = -1; dx <= 1; dx += 2)
    {
        [unroll]
        for (int dy = -1; dy <= 1; dy += 2)
        {
            float2 offset = float2(dx, dy) * texelSize;
            float  stored = csmShadowMap.Sample(pointClamp, float3(shadowUV + offset, cascadeF));
            shadow += (stored >= shadowDepth - bias) ? 1.0 : 0.0;
        }
    }
    float centreDepth = csmShadowMap.Sample(pointClamp, float3(shadowUV, cascadeF));
    shadow += (centreDepth >= shadowDepth - bias) ? 1.0 : 0.0;
    shadow /= 5.0;
    return shadow;
}

struct PSInput { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float4 main(PSInput input) : SV_Target0
{
    float3 albedo    = gbuffer0.Sample(pointClamp, input.uv).rgb;
    float3 normalEnc = gbuffer1.Sample(pointClamp, input.uv).rgb;
    float2 mr        = gbuffer2.Sample(pointClamp, input.uv).rg;
    float  depth     = depthTex.Sample(pointClamp, input.uv);

    if (depth >= 1.0)
        return float4(0.0, 0.0, 0.0, 1.0);

    float metallic  = mr.r;
    float roughness = clamp(mr.g, 0.04, 1.0);

    float3 N = normalize(normalEnc * 2.0 - 1.0);
    float3 posWS = ReconstructWorldPos(input.uv, depth);

    float3 V = normalize(eyePosition - posWS);
    float3 L = normalize(lightDir);
    float3 H = normalize(V + L);

    albedo = pow(max(albedo, 0.0001), 2.2);  // sRGB → linear

    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);

    float  D = D_GGX(N, H, roughness);
    float  G = G_SmithSchlick(N, V, L, roughness);
    float3 F = F_Schlick(max(dot(H, V), 0.0), F0);

    float3 numerator   = D * G * F;
    float  denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.001;
    float3 specular    = numerator / denominator;

    float3 kD      = (float3(1.0, 1.0, 1.0) - F) * (1.0 - metallic);
    float3 diffuse = kD * albedo / PI;

    float  NdL      = max(dot(N, L), 0.0);
    float3 radiance = lightColor.rgb * lightColor.w;

    float4 posVS = mul(float4(posWS, 1.0), viewMatrix);
    float  viewZ = posVS.z;
    float  shadow = SampleCSMShadow(posWS, viewZ);

    float ao = ssaoBlurTex.Sample(bilinearClamp, input.uv);

    float3 Lo      = (diffuse + specular) * radiance * NdL * shadow;
    float3 ambient = float3(0.03, 0.03, 0.03) * albedo * ao;
    float3 color   = ambient + Lo;

    return float4(color, 1.0);
}
