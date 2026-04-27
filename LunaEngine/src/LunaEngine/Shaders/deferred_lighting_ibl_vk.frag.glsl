// deferred_lighting_ibl_vk.frag.glsl — Vulkan deferred PBR + IBL (Vulkan GLSL 4.60, Phase 15C)
// Paired with fullscreen.vert.hlsl: inUV at location 0.
//
// Descriptor layout:
//   set=0, binding=0  — DeferredSceneBuffer (UBO)
//   set=1, binding=0  — gbuffer0 (albedo)
//   set=1, binding=1  — gbuffer1 (normal)
//   set=1, binding=2  — gbuffer2 (metalRough)
//   set=1, binding=3  — depthTex
//   set=1, binding=4  — pointSampler
//   set=1, binding=5  — csmShadowMap (texture2DArray)
//   set=1, binding=6  — csmSampler
//   set=1, binding=7  — ssaoBlurTex
//   set=1, binding=8  — bilinearClamp
//   set=1, binding=9  — gEnvCube   (prefiltered env, textureCube)
//   set=1, binding=10 — gIrrCube   (irradiance,     textureCube)
//   set=1, binding=11 — gBrdfLUT   (BRDF split-sum, texture2D)
//   set=1, binding=12 — gEnvSampler (trilinear clamp)
//   set=1, binding=13 — rtShadowTex (texture2D, R8_UNORM — PARTIALLY_BOUND when RT disabled)
//   set=2, binding=0  — ClusterParams (UBO)
//   set=2, binding=1  — GPUPointLight[] (SSBO)
//   set=2, binding=2  — clusterLightCounts (SSBO)
//   set=2, binding=3  — clusterLightIndices (SSBO)
#version 460

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
    uint  rtEnabled;    uint numPointLights; uvec2 _pad2;
};

layout(set = 1, binding = 0)  uniform texture2D      gbuffer0;
layout(set = 1, binding = 1)  uniform texture2D      gbuffer1;
layout(set = 1, binding = 2)  uniform texture2D      gbuffer2;
layout(set = 1, binding = 3)  uniform texture2D      depthTex;
layout(set = 1, binding = 4)  uniform sampler        pointSampler;
layout(set = 1, binding = 5)  uniform texture2DArray csmShadowMap;
layout(set = 1, binding = 6)  uniform sampler        csmSampler;
layout(set = 1, binding = 7)  uniform texture2D      ssaoBlurTex;
layout(set = 1, binding = 8)  uniform sampler        bilinearClamp;
layout(set = 1, binding = 9)  uniform textureCube    gEnvCube;
layout(set = 1, binding = 10) uniform textureCube    gIrrCube;
layout(set = 1, binding = 11) uniform texture2D      gBrdfLUT;
layout(set = 1, binding = 12) uniform sampler        gEnvSampler;
layout(set = 1, binding = 13) uniform texture2D      rtShadowTex;

// Phase 24: Clustered lighting data (set=2)
const uint CLUSTER_X = 16u;
const uint CLUSTER_Y = 9u;
const uint CLUSTER_Z = 24u;
const uint MAX_LIGHTS_PER_CLUSTER = 128u;

struct GPUPointLight {
    vec3  position;   // view-space
    float radius;
    vec3  color;
    float intensity;
};

layout(std140, set = 2, binding = 0) uniform ClusterParams {
    layout(row_major) mat4 clusterInvProj;
    float clusterNearZ;
    float clusterFarZ;
    float clusterScreenW;
    float clusterScreenH;
    uint  clusterNumLights;
    uint  _clPad[3];
};

layout(std430, set = 2, binding = 1) readonly buffer LightBuffer {
    GPUPointLight pointLights[];
};

layout(std430, set = 2, binding = 2) readonly buffer ClusterCounts {
    uint clusterLightCount[];
};

layout(std430, set = 2, binding = 3) readonly buffer ClusterIndices {
    uint clusterLightIndex[];
};

// G-buffer debug visualization:
//   0 = normal lit output
//   1 = albedo (sRGB-encoded, as stored in G-buffer)
//   2 = world-space normal (encoded [0,1])
//   3 = metallic (R) + roughness (G)
//   4 = SSAO only
//   5 = depth raw value (for diagnostics)
//   99 = magenta debug (test if pass executes)
#define DEBUG_GBUFFER 0

const float PI               = 3.14159265359;
const uint  PREFILTER_MIP_COUNT = 5u;

float D_GGX(vec3 N, vec3 H, float roughness)
{
    float a  = roughness * roughness;
    float a2 = a * a;
    float NdH = max(dot(N, H), 0.0);
    float d   = NdH * NdH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-7);
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

vec3 F_SchlickRoughness(float cosTheta, vec3 F0, float roughness)
{
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(max(1.0 - cosTheta, 0.0), 5.0);
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
            vec2 off    = vec2(dx, dy) * texelSize;
            float stored = texture(sampler2DArray(csmShadowMap, csmSampler), vec3(shadowUV + off, cascadeF)).r;
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
        // Environment mapping removed — gradient background same as non-IBL path
        vec3 ndc = vec3(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 1.0);
        vec4 ws  = vec4(ndc, 1.0) * invViewProj;
        vec3 dir = normalize(ws.xyz / ws.w - eyePosition);
        float t  = clamp(dir.y * 0.5 + 0.5, 0.0, 1.0);
        outColor = vec4(mix(vec3(0.05, 0.08, 0.15), vec3(0.1, 0.2, 0.5), t), 1.0);
        return;
    }

    vec4 gb0Full   = texture(sampler2D(gbuffer0, pointSampler), uv);
    vec3 albedo    = gb0Full.rgb;
    vec3 normalEnc = texture(sampler2D(gbuffer1, pointSampler), uv).rgb;
    vec4 gb2Full   = texture(sampler2D(gbuffer2, pointSampler), uv);
    vec2 mr        = gb2Full.rg;
    vec3 emissiveRaw = vec3(gb0Full.a, gb2Full.b, gb2Full.a);

    float metallic  = mr.r;
    float roughness = clamp(mr.g, 0.04, 1.0);

    vec3 N     = normalize(normalEnc * 2.0 - 1.0);
    vec3 posWS = ReconstructWorldPos(uv, depth);
    vec3 V     = normalize(eyePosition - posWS);
    vec3 L     = normalize(lightDir);
    vec3 H     = normalize(V + L);
    vec3 R     = reflect(-V, N);

    albedo = pow(max(albedo, vec3(0.0001)), vec3(2.2));

    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    // Direct lighting
    float  D        = D_GGX(N, H, roughness);
    float  G        = G_Smith(N, V, L, roughness);
    vec3   F_direct = F_Schlick(max(dot(H, V), 0.0), F0);
    vec3   specular = (D * G * F_direct) / max(4.0 * dot(N, V) * dot(N, L), 0.001);
    vec3   kD       = (1.0 - F_direct) * (1.0 - metallic);
    vec3   diffuse  = kD * albedo / PI;

    float  NdL      = max(dot(N, L), 0.0);
    vec3   radiance = lightColor * lightIntensity;

    vec4  posVS = vec4(posWS, 1.0) * viewMatrix;
    float viewZ = posVS.z;

    float shadow;
    if (rtEnabled != 0u)
        shadow = texelFetch(sampler2D(rtShadowTex, pointSampler), ivec2(gl_FragCoord.xy), 0).r;
    else
        shadow = SampleCSMShadow(posWS, viewZ);
    float ao  = texture(sampler2D(ssaoBlurTex, bilinearClamp), uv).r;
    vec3  Lo  = (diffuse + specular) * radiance * NdL * shadow;

    // Phase 24: Clustered point light accumulation
    if (numPointLights > 0u)
    {
        // Determine cluster index from screen position + view-space depth
        float logRatio = log(clusterFarZ / clusterNearZ);
        uint cx = uint(uv.x * float(CLUSTER_X));
        uint cy = uint(uv.y * float(CLUSTER_Y));
        uint cz = uint(log(viewZ / clusterNearZ) / logRatio * float(CLUSTER_Z));
        cx = min(cx, CLUSTER_X - 1u);
        cy = min(cy, CLUSTER_Y - 1u);
        cz = min(cz, CLUSTER_Z - 1u);

        uint clusterIdx = cx + cy * CLUSTER_X + cz * CLUSTER_X * CLUSTER_Y;
        uint lightCount = clusterLightCount[clusterIdx];
        uint baseIdx    = clusterIdx * MAX_LIGHTS_PER_CLUSTER;

        for (uint li = 0u; li < lightCount; ++li)
        {
            uint lightIdx = clusterLightIndex[baseIdx + li];
            GPUPointLight pl = pointLights[lightIdx];

            // Light is in view space — compute direction and attenuation
            vec3 Lpl = pl.position - posVS.xyz;
            float dist = length(Lpl);
            if (dist >= pl.radius) continue;
            Lpl /= dist;

            float attenuation = 1.0 / (dist * dist + 0.01);
            // Smooth radius falloff
            float falloff = 1.0 - smoothstep(0.8 * pl.radius, pl.radius, dist);
            attenuation *= falloff;

            // Transform light direction to world space for BRDF (N, V are world-space)
            // posVS = posWS * viewMatrix  →  Lpl_ws = inverse(viewMatrix) * Lpl_vs
            // Since viewMatrix is row-major in UBO, transpose to get column-major for GLSL
            vec3 LplWS = normalize(mat3(
                viewMatrix[0].xyz,
                viewMatrix[1].xyz,
                viewMatrix[2].xyz
            ) * Lpl);  // row-major mat3 * vec = transpose(col-major) * vec

            vec3 Hpl = normalize(V + LplWS);

            float Dpl = D_GGX(N, Hpl, roughness);
            float Gpl = G_Smith(N, V, LplWS, roughness);
            vec3  Fpl = F_Schlick(max(dot(Hpl, V), 0.0), F0);
            vec3  specPl = (Dpl * Gpl * Fpl) / max(4.0 * dot(N, V) * max(dot(N, LplWS), 0.0), 0.001);
            vec3  kDpl   = (1.0 - Fpl) * (1.0 - metallic);
            vec3  diffPl = kDpl * albedo / PI;

            float NdLpl = max(dot(N, LplWS), 0.0);
            vec3  plRadiance = pl.color * pl.intensity;

            Lo += (diffPl + specPl) * plRadiance * NdLpl * attenuation;
        }
    }

    // IBL ambient (split-sum)
    float NdV       = max(dot(N, V), 0.0);
    vec3  F_ibl     = F_SchlickRoughness(NdV, F0, roughness);
    vec3  kD_ibl    = (1.0 - F_ibl) * (1.0 - metallic);

    vec3 irradiance     = texture(samplerCube(gIrrCube, gEnvSampler), N).rgb;
    vec3 diffuseIBL     = kD_ibl * irradiance * albedo;

    float mipLevel         = roughness * float(PREFILTER_MIP_COUNT - 1u);
    vec3  prefilteredColor = textureLod(samplerCube(gEnvCube, gEnvSampler), R, mipLevel).rgb;
    vec2  brdf             = texture(sampler2D(gBrdfLUT, bilinearClamp), vec2(NdV, roughness)).rg;
    vec3  specularIBL      = prefilteredColor * (F_ibl * brdf.x + brdf.y);

    vec3 ambient = (diffuseIBL + specularIBL) * ao;
    vec3 emissiveLinear = pow(max(emissiveRaw, vec3(0.0001)), vec3(2.2));
    vec3 color   = ambient + Lo + emissiveLinear;

#if DEBUG_GBUFFER == 1
    outColor = vec4(albedo, 1.0);           // albedo (linear, after pow 2.2)
#elif DEBUG_GBUFFER == 2
    outColor = vec4(normalEnc, 1.0);        // world-space normal encoded [0,1]
#elif DEBUG_GBUFFER == 3
    outColor = vec4(metallic, roughness, 0.0, 1.0);  // R=metallic, G=roughness
#elif DEBUG_GBUFFER == 4
    outColor = vec4(vec3(ao), 1.0);         // SSAO
#elif DEBUG_GBUFFER == 99
    outColor = vec4(1.0, 0.0, 1.0, 1.0);    // MAGENTA - test if deferred pass runs
#else
    outColor = vec4(color, 1.0);
#endif
}
