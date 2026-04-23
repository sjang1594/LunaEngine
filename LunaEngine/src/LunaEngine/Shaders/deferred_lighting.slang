// deferred_lighting.slang — Deferred lighting pixel shader (Slang / SM 6.0)
// Ported from deferred_lighting.frag.hlsl.
//
// Root signature: matches DeferredLighting in DX12Pipeline.cpp
//   b0 = SceneConstants CBV
//   t0..t4 = GB0/GB1/GB2/depth/CSM shadow array
//   t5 = SSAO blur
//   s0 = point-clamp, s1 = bilinear-clamp

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

Texture2D             gbuffer0     : register(t0);  // albedo.rgb
Texture2D             gbuffer1     : register(t1);  // world-space normal (encoded [0,1])
Texture2D             gbuffer2     : register(t2);  // metallic.r + roughness.g
Texture2D<float>      depthTex     : register(t3);  // hardware depth R32_FLOAT
Texture2DArray<float> csmShadowMap : register(t4);  // CSM 4-cascade array
Texture2D<float>      ssaoBlurTex  : register(t5);  // SSAO blur result

SamplerState pointClamp    : register(s0);
SamplerState bilinearClamp : register(s1);

static const float kPI = 3.14159265359f;

// ---------------------------------------------------------------------------
// BRDFParams — inputs to Cook-Torrance.
// ---------------------------------------------------------------------------
struct BRDFParams
{
    float3 albedo;
    float  metallic;
    float  roughness;
    float3 N;        // surface normal (world-space)
    float3 V;        // view direction
    float3 L;        // light direction
    float3 radiance; // light color * intensity
};

// ---------------------------------------------------------------------------
// Cook-Torrance BRDF — GGX specular + Lambertian diffuse.
// ---------------------------------------------------------------------------
float3 CookTorrance(BRDFParams p)
{
    float3 H  = normalize(p.V + p.L);
    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), p.albedo, p.metallic);

    // D — GGX normal distribution function
    float a    = p.roughness * p.roughness;
    float a2   = a * a;
    float NdH  = max(dot(p.N, H), 0.0f);
    float denom = NdH * NdH * (a2 - 1.0f) + 1.0f;
    float D    = a2 / (kPI * denom * denom);

    // G — Smith-Schlick geometry term
    float NdV = max(dot(p.N, p.V), 0.0f);
    float NdL = max(dot(p.N, p.L), 0.0f);
    float r   = p.roughness + 1.0f;
    float k   = (r * r) / 8.0f;
    float gV  = NdV / (NdV * (1.0f - k) + k);
    float gL  = NdL / (NdL * (1.0f - k) + k);
    float G   = gV * gL;

    // F — Schlick Fresnel approximation
    float3 F = F0 + (1.0f - F0) * pow(max(1.0f - dot(H, p.V), 0.0f), 5.0f);

    float3 specular = (D * G * F) / (4.0f * NdV * NdL + 0.001f);
    float3 kD       = (float3(1.0f, 1.0f, 1.0f) - F) * (1.0f - p.metallic);
    float3 diffuse  = kD * p.albedo / kPI;

    return (diffuse + specular) * p.radiance * NdL;
}

// ---------------------------------------------------------------------------
// Reconstruct world-space position from hardware depth + inverse view-proj
// ---------------------------------------------------------------------------
float3 ReconstructWorldPos(float2 uv, float depth)
{
    float4 ndcPos;
    ndcPos.x = uv.x * 2.0f - 1.0f;
    ndcPos.y = (1.0f - uv.y) * 2.0f - 1.0f;  // flip Y: UV top=0, NDC top=+1
    ndcPos.z = depth;
    ndcPos.w = 1.0f;
    float4 worldPos = mul(ndcPos, invViewProj);
    return worldPos.xyz / worldPos.w;
}

// ---------------------------------------------------------------------------
// CSM shadow factor — 5-tap manual PCF across cascade array
// ---------------------------------------------------------------------------
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
            float stored  = csmShadowMap.Sample(pointClamp, float3(shadowUV + offset, cascadeF));
            shadow += (stored >= shadowDepth - bias) ? 1.0f : 0.0f;
        }
    }
    float centre = csmShadowMap.Sample(pointClamp, float3(shadowUV, cascadeF));
    shadow += (centre >= shadowDepth - bias) ? 1.0f : 0.0f;
    shadow /= 5.0f;
    return shadow;
}

// ---------------------------------------------------------------------------
// Pixel shader entry point
// ---------------------------------------------------------------------------
struct PSInput
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

[shader("pixel")]
float4 main(PSInput input) : SV_Target0
{
    // G-buffer reads
    float3 albedo    = gbuffer0.Sample(pointClamp, input.uv).rgb;
    float3 normalEnc = gbuffer1.Sample(pointClamp, input.uv).rgb;
    float2 mr        = gbuffer2.Sample(pointClamp, input.uv).rg;
    float  depth     = depthTex.Sample(pointClamp, input.uv);

    // Sky / background: depth == 1.0 means no geometry
    if (depth >= 1.0f)
        return float4(0.1f, 0.1f, 0.2f, 1.0f);

    float metallic  = mr.r;
    float roughness = clamp(mr.g, 0.04f, 1.0f);
    float3 N        = normalize(normalEnc * 2.0f - 1.0f);
    float3 posWS    = ReconstructWorldPos(input.uv, depth);
    float3 V        = normalize(eyePosition - posWS);
    float3 L        = normalize(lightDir);

    // sRGB → linear for albedo (textures are stored gamma-encoded)
    albedo = pow(max(albedo, 0.0001f), 2.2f);

    // CSM shadow
    float4 posVS = mul(float4(posWS, 1.0f), viewMatrix);
    float shadow = SampleCSMShadow(posWS, posVS.z);

    // SSAO — bilinear-upscale half-res occlusion into ambient
    float ao = ssaoBlurTex.Sample(bilinearClamp, input.uv);

    // Assemble BRDF params and evaluate
    BRDFParams p;
    p.albedo    = albedo;
    p.metallic  = metallic;
    p.roughness = roughness;
    p.N         = N;
    p.V         = V;
    p.L         = L;
    p.radiance  = lightColor.rgb * lightColor.w;

    float3 Lo      = CookTorrance(p) * shadow;
    float3 ambient = float3(0.03f, 0.03f, 0.03f) * albedo * ao;
    float3 color   = ambient + Lo;

    // Tone map (Reinhard) + gamma encode
    color = color / (color + float3(1.0f, 1.0f, 1.0f));
    color = pow(max(color, 0.0f), 1.0f / 2.2f);

    return float4(color, 1.0f);
}
