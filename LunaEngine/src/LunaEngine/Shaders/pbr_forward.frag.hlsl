// pbr_forward.frag.hlsl — Forward PBR pixel shader (SM 6.0, Vulkan SPIR-V)
// Phase 5C: split per-frame scene data (eye/light) from static material factors.
//   set=0, binding=1 — SceneBuffer (per-frame, updated every frame from camera state)
//   set=1, binding=0 — MaterialBuffer (per-material, static after load)

// Per-frame scene constants — eye position + directional light
[[vk::binding(1, 0)]]
cbuffer SceneBuffer : register(b2)
{
    float3 eyePosition;  float _padEye;
    float3 lightDir;     float  lightIntensity;  // toward-light, normalised
    float3 lightColor;   float  _padLight;
};

// Per-material constants — PBR factors only (textures still in set=1)
[[vk::binding(0, 1)]]
cbuffer MaterialBuffer : register(b1)
{
    float4 albedoFactor;
    float  metallicFactor;
    float  roughnessFactor;
    float2 _pad;
};

[[vk::binding(1, 1)]]
Texture2D    albedoTex     : register(t0);
[[vk::binding(2, 1)]]
Texture2D    normalTex     : register(t1);
[[vk::binding(3, 1)]]
Texture2D    metalRoughTex : register(t2);

[[vk::binding(4, 1)]]
SamplerState linearSampler : register(s0);

static const float PI = 3.14159265359;

float D_GGX(float3 N, float3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdH = max(dot(N, H), 0.0);
    float denom = NdH * NdH * (a2 - 1.0) + 1.0;
    return a2 / (PI * denom * denom);
}

float G_Schlick(float NdV, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdV / (NdV * (1.0 - k) + k);
}

float G_Smith(float3 N, float3 V, float3 L, float roughness)
{
    return G_Schlick(max(dot(N,V),0.0), roughness) * G_Schlick(max(dot(N,L),0.0), roughness);
}

float3 F_Schlick(float cosTheta, float3 F0)
{
    return F0 + (1.0 - F0) * pow(max(1.0 - cosTheta, 0.0), 5.0);
}

struct PSInput
{
    float4 posCS     : SV_POSITION;
    float3 posWS     : POSITION;
    float3 normalWS  : NORMAL;
    float2 uv        : TEXCOORD0;
    float3 tangentWS : TANGENT;
    float3 bitanWS   : BITANGENT;
};

float4 main(PSInput input) : SV_Target0
{
    // Sample textures
    float3 albedo    = albedoTex.Sample(linearSampler, input.uv).rgb * albedoFactor.rgb;
    float2 mr        = metalRoughTex.Sample(linearSampler, input.uv).bg; // b=metal, g=rough
    float  metallic  = mr.x * metallicFactor;
    float  roughness = clamp(mr.y * roughnessFactor, 0.04, 1.0);

    // Normal mapping
    float3 n = normalTex.Sample(linearSampler, input.uv).xyz * 2.0 - 1.0;
    float3x3 TBN = float3x3(normalize(input.tangentWS),
                             normalize(input.bitanWS),
                             normalize(input.normalWS));
    float3 N = normalize(mul(n, TBN));

    // Lighting
    float3 V = normalize(eyePosition - input.posWS);
    float3 L = normalize(lightDir);
    float3 H = normalize(V + L);

    // sRGB → linear
    albedo = pow(max(albedo, 0.0001), 2.2);

    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);

    float  D = D_GGX(N, H, roughness);
    float  G = G_Smith(N, V, L, roughness);
    float3 F = F_Schlick(max(dot(H, V), 0.0), F0);

    float3 specular = (D * G * F) / (4.0 * max(dot(N,V),0.0) * max(dot(N,L),0.0) + 0.001);
    float3 kD       = (1.0 - F) * (1.0 - metallic);
    float3 diffuse  = kD * albedo / PI;

    float  NdL      = max(dot(N, L), 0.0);
    float3 radiance = lightColor * lightIntensity;
    float3 Lo       = (diffuse + specular) * radiance * NdL;

    float3 ambient = float3(0.03, 0.03, 0.03) * albedo;
    float3 color   = ambient + Lo;

    // Tone map + gamma
    color = color / (color + 1.0);
    color = pow(max(color, 0.0), 1.0 / 2.2);

    return float4(color, 1.0);
}

