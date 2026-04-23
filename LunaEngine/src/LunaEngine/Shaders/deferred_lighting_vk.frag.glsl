// deferred_lighting_vk.frag.glsl — Deferred PBR lighting fragment shader (Vulkan GLSL 4.60)
// Paired with fullscreen.vert.hlsl: inUV at location 0.
//
// Descriptor layout:
//   set=0, binding=0 — DeferredSceneBuffer (UBO: invViewProj + eye + light + viewMatrix + lightVP[4] + cascadeSplits)
//   set=1, binding=0 — gbuffer0   (albedo,     texture2D)
//   set=1, binding=1 — gbuffer1   (normal,     texture2D)
//   set=1, binding=2 — gbuffer2   (metalRough, texture2D)
//   set=1, binding=3 — depthTex   (texture2D)
//   set=1, binding=4 — pointSampler (sampler)
//   set=1, binding=5 — csmShadowMap (texture2DArray)
//   set=1, binding=6 — csmSampler   (sampler)
//   set=1, binding=7 — ssaoBlurTex  (texture2D)
//   set=1, binding=8 — bilinearClamp (sampler)
#version 460

// Debug visualization modes:
//   0 = normal lit output
//   1 = raw G-buffer albedo (sRGB, before pow 2.2)
//   2 = world-space normal (encoded [0,1])
//   3 = metallic (R) + roughness (G)
//   4 = SSAO only
//   5 = shadow only
//   6 = albedo after linear conversion (pow 2.2 applied)
//   7 = FLAT RED test (1,0,0) — if not red on screen, post-process chain swaps channels
#define DEBUG_DEFERRED 0

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0, std140) uniform DeferredSceneBuffer {
    layout(row_major) mat4 invViewProj;
    vec3  eyePosition;  float _pad0;
    vec3  lightDir;     float lightIntensity;
    vec3  lightColor;   float _pad1;
    layout(row_major) mat4 viewMatrix;
    layout(row_major) mat4 lightVP[4];
    vec4  cascadeSplits;
};

layout(set = 1, binding = 0) uniform texture2D      gbuffer0;
layout(set = 1, binding = 1) uniform texture2D      gbuffer1;
layout(set = 1, binding = 2) uniform texture2D      gbuffer2;
layout(set = 1, binding = 3) uniform texture2D      depthTex;
layout(set = 1, binding = 4) uniform sampler        pointSampler;
layout(set = 1, binding = 5) uniform texture2DArray csmShadowMap;
layout(set = 1, binding = 6) uniform sampler        csmSampler;
layout(set = 1, binding = 7) uniform texture2D      ssaoBlurTex;
layout(set = 1, binding = 8) uniform sampler        bilinearClamp;

const float PI = 3.14159265359;

float D_GGX(vec3 N, vec3 H, float roughness)
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

float G_Smith(vec3 N, vec3 V, vec3 L, float roughness)
{
    return G_Schlick(max(dot(N, V), 0.0), roughness)
         * G_Schlick(max(dot(N, L), 0.0), roughness);
}

vec3 F_Schlick(float cosTheta, vec3 F0)
{
    return F0 + (1.0 - F0) * pow(max(1.0 - cosTheta, 0.0), 5.0);
}

vec3 ReconstructWorldPos(vec2 uv, float depth)
{
    vec4 ndc;
    ndc.x = uv.x * 2.0 - 1.0;
    ndc.y = 1.0 - uv.y * 2.0;
    ndc.z = depth;
    ndc.w = 1.0;
    vec4 ws = ndc * invViewProj;
    return ws.xyz / ws.w;
}

float SampleCSMShadow(vec3 posWS, float viewSpaceZ)
{
    uint cascade = 3u;
    if      (viewSpaceZ < cascadeSplits.x) cascade = 0u;
    else if (viewSpaceZ < cascadeSplits.y) cascade = 1u;
    else if (viewSpaceZ < cascadeSplits.z) cascade = 2u;

    vec4 posLS = vec4(posWS, 1.0) * lightVP[cascade];
    posLS.xyz /= posLS.w;

    vec2 shadowUV;
    shadowUV.x =  posLS.x * 0.5 + 0.5;
    shadowUV.y = -posLS.y * 0.5 + 0.5;
    float shadowDepth = posLS.z;

    if (any(lessThan(shadowUV, vec2(0.0))) || any(greaterThan(shadowUV, vec2(1.0)))
        || shadowDepth < 0.0 || shadowDepth > 1.0)
        return 1.0;

    float bias      = 0.005;
    float texelSize = 1.0 / 2048.0;
    float shadow    = 0.0;
    float cascadeF  = float(cascade);

    for (int dx = -1; dx <= 1; dx += 2)
    {
        for (int dy = -1; dy <= 1; dy += 2)
        {
            vec2 offset = vec2(dx, dy) * texelSize;
            float stored = texture(sampler2DArray(csmShadowMap, csmSampler), vec3(shadowUV + offset, cascadeF)).r;
            shadow += (stored >= shadowDepth - bias) ? 1.0 : 0.0;
        }
    }
    float centreDepth = texture(sampler2DArray(csmShadowMap, csmSampler), vec3(shadowUV, cascadeF)).r;
    shadow += (centreDepth >= shadowDepth - bias) ? 1.0 : 0.0;
    shadow /= 5.0;
    return shadow;
}

void main()
{
    vec2  uv    = inUV;
    float depth = texture(sampler2D(depthTex, pointSampler), uv).r;

    if (depth >= 1.0)
    {
        vec3 ndc = vec3(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 1.0);
        vec4 ws  = vec4(ndc, 1.0) * invViewProj;
        vec3 dir = normalize(ws.xyz / ws.w - eyePosition);
        float t  = clamp(dir.y * 0.5 + 0.5, 0.0, 1.0);
        outColor = vec4(mix(vec3(0.05, 0.08, 0.15), vec3(0.1, 0.2, 0.5), t), 1.0);
        return;
    }

    vec3 albedo    = texture(sampler2D(gbuffer0, pointSampler), uv).rgb;
    vec3 normalEnc = texture(sampler2D(gbuffer1, pointSampler), uv).rgb;
    vec2 mr        = texture(sampler2D(gbuffer2, pointSampler), uv).rg;

    float metallic  = mr.r;
    float roughness = clamp(mr.g, 0.04, 1.0);

    vec3 N     = normalize(normalEnc * 2.0 - 1.0);
    vec3 posWS = ReconstructWorldPos(uv, depth);

    vec3 V = normalize(eyePosition - posWS);
    vec3 L = normalize(lightDir);
    vec3 H = normalize(V + L);

    albedo = pow(max(albedo, vec3(0.0001)), vec3(2.2));

    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    float  D        = D_GGX(N, H, roughness);
    float  G        = G_Smith(N, V, L, roughness);
    vec3   F        = F_Schlick(max(dot(H, V), 0.0), F0);
    vec3   specular = (D * G * F) / (4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.001);

    vec3 kD      = (vec3(1.0) - F) * (1.0 - metallic);
    vec3 diffuse = kD * albedo / PI;

    float  NdL      = max(dot(N, L), 0.0);
    vec3   radiance = lightColor * lightIntensity;

    vec4  posVS  = vec4(posWS, 1.0) * viewMatrix;
    float viewZ  = posVS.z;
    float shadow = SampleCSMShadow(posWS, viewZ);

    vec3 Lo = (diffuse + specular) * radiance * NdL * shadow;

    float ao      = texture(sampler2D(ssaoBlurTex, bilinearClamp), uv).r;
    vec3  ambient = vec3(0.03) * albedo * ao;
    vec3  color   = ambient + Lo;

#if DEBUG_DEFERRED == 1
    outColor = vec4(texture(sampler2D(gbuffer0, pointSampler), uv).rgb, 1.0);  // raw sRGB albedo
#elif DEBUG_DEFERRED == 2
    outColor = vec4(normalEnc, 1.0);  // encoded normal [0,1]
#elif DEBUG_DEFERRED == 3
    outColor = vec4(mr.r, mr.g, 0.0, 1.0);  // metallic + roughness
#elif DEBUG_DEFERRED == 4
    outColor = vec4(vec3(ao), 1.0);
#elif DEBUG_DEFERRED == 5
    outColor = vec4(vec3(shadow), 1.0);
#elif DEBUG_DEFERRED == 6
    outColor = vec4(albedo, 1.0);  // linear albedo (after pow 2.2)
#elif DEBUG_DEFERRED == 7
    outColor = vec4(1.0, 0.0, 0.0, 1.0);  // FLAT RED — test post-process chain
#else
    outColor = vec4(color, 1.0);
#endif
}
