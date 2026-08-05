// sensor_lighting.frag.hlsl — S2: stripped deferred lighting for camera sensor renders
// IBL + direct sun only. No SSAO, no CSM shadows, no clustered point lights, no GI/SSR.
// Produces linear HDR output written to the per-camera litRT (RGBA16F).
//
// Root signature (RootSignatureLayout::SensorLighting):
//   b0 = SceneConstants CBV
//   t0-t2 = G-buffer SRV table (GB0, GB1, GB2)
//   t3    = depth SRV
//   t4    = IBL irradiance cubemap
//   t5    = IBL prefiltered env map
//   t6    = IBL BRDF LUT
//   s0    = point-clamp
//   s1    = bilinear-clamp
//   s2    = trilinear-clamp (IBL)

cbuffer SceneConstants : register(b0)
{
    row_major float4x4 invViewProj;
    float3 eyePosition; float _pad0;
    float3 lightDir;    float _pad1;
    float4 lightColor;
    row_major float4x4 viewMatrix;
    row_major float4x4 lightVP[4];   // unused in sensor path
    float4 cascadeSplits;            // unused in sensor path
    uint   numPointLights;
    uint3  _pad2;
};

Texture2D             gbuffer0     : register(t0);
Texture2D             gbuffer1     : register(t1);
Texture2D             gbuffer2     : register(t2);
Texture2D<float>      depthTex     : register(t3);
TextureCube<float4>   irrMap       : register(t4);
TextureCube<float4>   prefilterMap : register(t5);
Texture2D<float2>     brdfLUT      : register(t6);

SamplerState pointClamp    : register(s0);
SamplerState bilinearClamp : register(s1);
SamplerState trilinearClamp: register(s2);

static const float PI = 3.14159265359f;
static const uint  PREFILTER_MIP_COUNT = 5u;

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

struct PSInput { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float4 main(PSInput input) : SV_Target0
{
    float  depth    = depthTex.Sample(pointClamp, input.uv);
    if (depth >= 1.0f)
        return float4(0.05f, 0.07f, 0.1f, 1.0f);  // sky fallback

    float4 gb0Data  = gbuffer0.Sample(pointClamp, input.uv);
    float4 gb1Data  = gbuffer1.Sample(pointClamp, input.uv);
    float4 gb2Data  = gbuffer2.Sample(pointClamp, input.uv);

    float3 albedo   = gb0Data.rgb;
    float3 normalEnc= gb1Data.rgb;
    float2 mr       = gb2Data.rg;
    float3 emissive = float3(gb0Data.a, gb2Data.b, gb2Data.a);

    float metallic  = mr.r;
    float roughness = clamp(mr.g, 0.04f, 1.0f);

    float3 N    = normalize(normalEnc * 2.0f - 1.0f);
    float3 posWS= ReconstructWorldPos(input.uv, depth);
    float3 V    = normalize(eyePosition - posWS);
    float3 L    = normalize(lightDir);
    float3 H    = normalize(V + L);
    float3 R    = reflect(-V, N);

    albedo = pow(max(albedo, 0.0001f), 2.2f);

    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);

    // Direct sun (no shadow)
    float  D = D_GGX(N, H, roughness);
    float  G = G_SmithSchlick(N, V, L, roughness);
    float3 F = F_Schlick(max(dot(H, V), 0.0f), F0);
    float3 specularDirect = (D * G * F) / max(4.0f * dot(N, V) * dot(N, L), 0.001f);
    float3 kD_direct = (1.0f - F) * (1.0f - metallic);
    float3 diffuseDirect = kD_direct * albedo / PI;
    float  NdL      = max(dot(N, L), 0.0f);
    float3 radiance = lightColor.rgb * lightColor.w;
    float3 Lo = (diffuseDirect + specularDirect) * radiance * NdL;

    // IBL ambient
    float  NdV   = max(dot(N, V), 0.0f);
    float3 F_ibl = F_SchlickRoughness(NdV, F0, roughness);
    float3 kD_ibl = (1.0f - F_ibl) * (1.0f - metallic);

    float3 irradiance       = irrMap.Sample(trilinearClamp, N).rgb;
    float3 diffuseIBL       = kD_ibl * irradiance * albedo;
    float  mipLevel         = roughness * float(PREFILTER_MIP_COUNT - 1u);
    float3 prefilteredColor = prefilterMap.SampleLevel(trilinearClamp, R, mipLevel).rgb;
    float2 brdf             = brdfLUT.Sample(bilinearClamp, float2(NdV, roughness));
    float3 specularIBL      = prefilteredColor * (F_ibl * brdf.x + brdf.y);
    float3 ambient          = diffuseIBL + specularIBL;

    float3 emissiveLinear = pow(max(emissive, 0.0001f), 2.2f);
    float3 color = ambient + Lo + emissiveLinear;

    return float4(color, 1.0f);
}
