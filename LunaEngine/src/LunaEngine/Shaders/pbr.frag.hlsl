// pbr.frag.hlsl — Cook-Torrance BRDF pixel shader (SM 6.0)
// Implements: D_GGX, G_SmithSchlick, F_Schlick
// Phase 3: directional light; Phase 4: multiplied by shadow occlusion factor.

cbuffer TransformBuffer : register(b0)
{
    row_major float4x4 modelMatrix;
    row_major float4x4 viewMatrix;
    row_major float4x4 projectionMatrix;
};

cbuffer MaterialBuffer : register(b1)
{
    float4 albedoFactor;      // base color multiplier
    float  metallicFactor;
    float  roughnessFactor;
    float2 _pad;
};

cbuffer SceneBuffer : register(b2)
{
    row_major float4x4 invViewProj;   // used to reconstruct world position from depth
    float4 lightDirection;            // xyz = toward-light direction (normalized), w = intensity
    float4 lightColor;                // xyz = color, w = unused
    float4 viewPosition;              // xyz = camera world-space eye position
    float2 viewportSize;              // for shadow map sampling
    float2 _scenePad;
};

// Material textures
Texture2D    albedoTex    : register(t0);
Texture2D    normalTex    : register(t1);
Texture2D    metalRoughTex: register(t2);
Texture2D    shadowMap    : register(t3);  // R32_FLOAT: 1.0=lit, 0.0=shadowed (Phase 4)

SamplerState linearSampler : register(s0);

static const float PI = 3.14159265359;

// -------------------------------------------------------------------------
// Cook-Torrance BRDF components
// -------------------------------------------------------------------------

// GGX Normal Distribution Function
float D_GGX(float3 N, float3 H, float roughness)
{
    float a  = roughness * roughness;
    float a2 = a * a;
    float NdH  = max(dot(N, H), 0.0);
    float NdH2 = NdH * NdH;
    float denom = NdH2 * (a2 - 1.0) + 1.0;
    return a2 / (PI * denom * denom);
}

// Smith's Schlick-GGX geometry function (single direction)
float G_Schlick(float NdV, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdV / (NdV * (1.0 - k) + k);
}

// Smith's combined geometry function
float G_SmithSchlick(float3 N, float3 V, float3 L, float roughness)
{
    float NdV = max(dot(N, V), 0.0);
    float NdL = max(dot(N, L), 0.0);
    return G_Schlick(NdV, roughness) * G_Schlick(NdL, roughness);
}

// Fresnel-Schlick approximation
float3 F_Schlick(float cosTheta, float3 F0)
{
    return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
}

// -------------------------------------------------------------------------
// Normal mapping: sample normalTex → TBN → world-space normal
// -------------------------------------------------------------------------
float3 NormalFromMap(float3 normalWS, float3 tangentWS, float3 bitanWS, float2 uv)
{
    float3 n = normalTex.Sample(linearSampler, uv).xyz;
    n = n * 2.0 - 1.0;  // [0,1] → [-1,1]
    float3x3 TBN = float3x3(tangentWS, bitanWS, normalWS);
    return normalize(mul(n, TBN));
}

// -------------------------------------------------------------------------
// Pixel shader entry
// -------------------------------------------------------------------------
struct PSInput
{
    float4 posCS    : SV_POSITION;
    float3 posWS    : POSITION;
    float3 normalWS : NORMAL;
    float2 uv       : TEXCOORD0;
    float3 tangentWS: TANGENT;
    float3 bitanWS  : BITANGENT;
};

float4 main(PSInput input) : SV_TARGET
{
    // ---- Material sampling ----
    float3 albedo    = albedoTex.Sample(linearSampler, input.uv).rgb * albedoFactor.rgb;
    // Convert to linear if textures are sRGB (simple approximation)
    albedo = pow(albedo, 2.2);

    float2 metalRough = metalRoughTex.Sample(linearSampler, input.uv).bg; // b=metallic, g=roughness
    float  metallic   = metalRough.x * metallicFactor;
    float  roughness  = metalRough.y * roughnessFactor;
    roughness = clamp(roughness, 0.04, 1.0);

    // ---- Normal mapping ----
    float3 N = NormalFromMap(normalize(input.normalWS),
                              normalize(input.tangentWS),
                              normalize(input.bitanWS),
                              input.uv);

    float3 V = normalize(viewPosition.xyz - input.posWS);
    float3 L = normalize(lightDirection.xyz);  // pre-normalized toward-light direction
    float3 H = normalize(V + L);

    // ---- Fresnel base reflectivity ----
    // Dielectric F0 = 0.04; metallic: lerp to albedo color
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);

    // ---- Cook-Torrance specular BRDF ----
    float  D   = D_GGX(N, H, roughness);
    float  G   = G_SmithSchlick(N, V, L, roughness);
    float3 F   = F_Schlick(max(dot(H, V), 0.0), F0);

    float3 numerator   = D * G * F;
    float  denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.001;
    float3 specular    = numerator / denominator;

    // ---- Diffuse contribution ----
    // Metals have no diffuse; dialectrics lose energy to specular via Fresnel
    float3 kD = (float3(1.0, 1.0, 1.0) - F) * (1.0 - metallic);
    float3 diffuse = kD * albedo / PI;

    // ---- Lighting ----
    float NdL       = max(dot(N, L), 0.0);
    float3 radiance = lightColor.rgb * lightDirection.w;  // w = intensity

    float3 Lo = (diffuse + specular) * radiance * NdL;

    // ---- Shadow occlusion (Phase 4 — shadow map sampled at screen UV) ----
    float2 screenUV = input.posCS.xy / viewportSize;
    float  shadow   = shadowMap.Sample(linearSampler, screenUV).r;  // 1.0=lit, 0.0=shadow

    Lo *= shadow;

    // ---- Ambient (simple IBL approximation) ----
    float3 ambient = float3(0.03, 0.03, 0.03) * albedo;
    float3 color   = ambient + Lo;

    // ---- Tone mapping (Reinhard) + gamma ----
    color = color / (color + float3(1.0, 1.0, 1.0));
    color = pow(color, 1.0 / 2.2);

    return float4(color, 1.0);
}
