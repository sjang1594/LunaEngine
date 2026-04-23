// deferred_lighting_vk.frag.hlsl — Deferred PBR lighting pixel shader (Vulkan SM 6.0)
// Reads albedo / normal / metalRough G-buffers + hardware depth, runs Cook-Torrance BRDF.
// Phase 8 VK: CSM shadow sampling via Texture2DArray + 5-tap PCF.
//
// Descriptor layout:
//   set=0, binding=0 — DeferredSceneBuffer (invViewProj + eye + light + viewMatrix + lightVP[4] + cascadeSplits)
//   set=1, binding=0 — gbuffer0       (albedo,      RGBA8_UNORM)
//   set=1, binding=1 — gbuffer1       (normal,      RGBA16F)
//   set=1, binding=2 — gbuffer2       (metalRough,  RGBA8_UNORM)
//   set=1, binding=3 — depthTex       (D32_SFLOAT sampled as R32_SFLOAT)
//   set=1, binding=4 — pointSampler
//   set=1, binding=5 — csmShadowMap   (Texture2DArray<float>, 4 layers)
//   set=1, binding=6 — csmSampler     (point-clamp sampler for shadow reads)

[[vk::binding(0, 0)]]
cbuffer DeferredSceneBuffer : register(b0)
{
    row_major float4x4 invViewProj;       //  64 B
    float3 eyePosition;  float _pad0;     //  16 B
    float3 lightDir;     float  lightIntensity;  //  16 B
    float3 lightColor;   float  _pad1;    //  16 B
    row_major float4x4 viewMatrix;        //  64 B
    row_major float4x4 lightVP[4];        // 256 B
    float4 cascadeSplits;                 //  16 B
};

[[vk::binding(0, 1)]] Texture2D        gbuffer0       : register(t0);
[[vk::binding(1, 1)]] Texture2D        gbuffer1       : register(t1);
[[vk::binding(2, 1)]] Texture2D        gbuffer2       : register(t2);
[[vk::binding(3, 1)]] Texture2D<float> depthTex       : register(t3);
[[vk::binding(4, 1)]] SamplerState     pointSampler   : register(s0);
[[vk::binding(5, 1)]] Texture2DArray<float> csmShadowMap : register(t4);
[[vk::binding(6, 1)]] SamplerState     csmSampler     : register(s1);
[[vk::binding(7, 1)]] Texture2D<float> ssaoBlurTex    : register(t5);  // blurred SSAO (half-res, bilinear upscale)
[[vk::binding(8, 1)]] SamplerState     bilinearClamp  : register(s2);  // bilinear for SSAO upscale

static const float PI = 3.14159265359;

// ---------------------------------------------------------------------------
// Cook-Torrance BRDF helpers
// ---------------------------------------------------------------------------
float D_GGX(float3 N, float3 H, float roughness)
{
    float a   = roughness * roughness;
    float a2  = a * a;
    float NdH = max(dot(N, H), 0.0);
    float d   = NdH * NdH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

float G_Schlick(float NdV, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdV / (NdV * (1.0 - k) + k);
}

float G_Smith(float3 N, float3 V, float3 L, float roughness)
{
    return G_Schlick(max(dot(N, V), 0.0), roughness)
         * G_Schlick(max(dot(N, L), 0.0), roughness);
}

float3 F_Schlick(float cosTheta, float3 F0)
{
    return F0 + (1.0 - F0) * pow(max(1.0 - cosTheta, 0.0), 5.0);
}

// ---------------------------------------------------------------------------
// Reconstruct world-space position
// ---------------------------------------------------------------------------
float3 ReconstructWorldPos(float2 uv, float depth)
{
    float4 ndc;
    ndc.x = uv.x * 2.0 - 1.0;
    ndc.y = 1.0 - uv.y * 2.0;
    ndc.z = depth;
    ndc.w = 1.0;
    float4 ws = mul(ndc, invViewProj);
    return ws.xyz / ws.w;
}

// ---------------------------------------------------------------------------
// CSM shadow factor — 5-tap manual PCF
// ---------------------------------------------------------------------------
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
            float stored = csmShadowMap.Sample(csmSampler, float3(shadowUV + offset, cascadeF));
            shadow += (stored >= shadowDepth - bias) ? 1.0 : 0.0;
        }
    }
    float centreDepth = csmShadowMap.Sample(csmSampler, float3(shadowUV, cascadeF));
    shadow += (centreDepth >= shadowDepth - bias) ? 1.0 : 0.0;
    shadow /= 5.0;

    return shadow;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
struct PSInput
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target0
{
    float2 uv     = input.uv;
    float  depth  = depthTex.Sample(pointSampler, uv);

    if (depth >= 1.0)
    {
        float3 ndc = float3(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, 1.0f);
        float4 ws  = mul(float4(ndc, 1.0f), invViewProj);
        float3 dir = normalize(ws.xyz / ws.w - eyePosition);
        float  t   = saturate(dir.y * 0.5f + 0.5f);
        return float4(lerp(float3(0.05f, 0.08f, 0.15f), float3(0.1f, 0.2f, 0.5f), t), 1.0f);
    }

    float3 albedo    = gbuffer0.Sample(pointSampler, uv).rgb;
    float3 normalEnc = gbuffer1.Sample(pointSampler, uv).rgb;
    float2 mr        = gbuffer2.Sample(pointSampler, uv).rg;

    float metallic  = mr.r;
    float roughness = clamp(mr.g, 0.04, 1.0);

    float3 N     = normalize(normalEnc * 2.0 - 1.0);
    float3 posWS = ReconstructWorldPos(uv, depth);

    float3 V = normalize(eyePosition - posWS);
    float3 L = normalize(lightDir);
    float3 H = normalize(V + L);

    albedo = pow(max(albedo, 0.0001), 2.2);

    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);

    float  D        = D_GGX(N, H, roughness);
    float  G        = G_Smith(N, V, L, roughness);
    float3 F        = F_Schlick(max(dot(H, V), 0.0), F0);
    float3 specular = (D * G * F) / (4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.001);

    float3 kD      = (float3(1.0, 1.0, 1.0) - F) * (1.0 - metallic);
    float3 diffuse = kD * albedo / PI;

    float  NdL      = max(dot(N, L), 0.0);
    float3 radiance = lightColor * lightIntensity;

    // CSM shadow
    float4 posVS  = mul(float4(posWS, 1.0), viewMatrix);
    float  viewZ  = posVS.z;
    float  shadow = SampleCSMShadow(posWS, viewZ);

    float3 Lo = (diffuse + specular) * radiance * NdL * shadow;

    // SSAO — bilinear-upscale half-res occlusion factor
    float ao = ssaoBlurTex.Sample(bilinearClamp, uv);

    float3 ambient = float3(0.03f, 0.03f, 0.03f) * albedo * ao;
    float3 color   = ambient + Lo;

    // Output raw HDR — tonemapping applied downstream in tonemapping pass
    return float4(color, 1.0f);
}
