#version 450
// sensor_lighting_vk.frag.glsl — S2b: Vulkan IBL-only deferred lighting for camera sensor renders.
// Stripped deferred: Cook-Torrance BRDF + IBL, no SSAO, no CSM, no SSR, no GI.
// Mirrors sensor_lighting.frag.hlsl (DX12 S2).

layout(location = 0) in  vec2 inUV;
layout(location = 0) out vec4 outColor;

// set=0 — sensor camera constants
layout(set=0, binding=0) uniform SensorSceneUBO {
    mat4 invViewProj;
    vec4 eyePosition;   // .xyz = eye world position
    vec4 lightDir;      // .xyz = normalized toward-light direction
    vec4 lightColor;    // .xyz = color, .w = intensity
};

// set=1 — G-buffer samplers
layout(set=1, binding=0) uniform sampler2D gbufAlbedo;
layout(set=1, binding=1) uniform sampler2D gbufNormal;
layout(set=1, binding=2) uniform sampler2D gbufMetalRough;
layout(set=1, binding=3) uniform sampler2D depthTex;

// set=2 — IBL
layout(set=2, binding=0) uniform samplerCube irrMap;
layout(set=2, binding=1) uniform samplerCube prefilterMap;
layout(set=2, binding=2) uniform sampler2D   brdfLUT;

const float PI = 3.14159265359;
const uint  PREFILTER_MIP_COUNT = 5u;

// ---- Cook-Torrance helpers -------------------------------------------------
float D_GGX(vec3 N, vec3 H, float roughness)
{
    float a  = roughness * roughness;
    float a2 = a * a;
    float NdH  = max(dot(N, H), 0.0);
    float denom = NdH * NdH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denom * denom, 1e-7);
}

float G_Schlick(float NdV, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdV / (NdV * (1.0 - k) + k);
}

float G_Smith(vec3 N, vec3 V, vec3 L, float roughness)
{
    float NdV = max(dot(N, V), 0.0);
    float NdL = max(dot(N, L), 0.0);
    return G_Schlick(NdV, roughness) * G_Schlick(NdL, roughness);
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
    vec4 ndcPos;
    ndcPos.x =  uv.x * 2.0 - 1.0;
    ndcPos.y = (1.0 - uv.y) * 2.0 - 1.0; // Vulkan NDC: Y flipped
    ndcPos.z = depth;
    ndcPos.w = 1.0;
    vec4 worldPos = invViewProj * ndcPos;
    return worldPos.xyz / worldPos.w;
}

void main()
{
    float depth = texture(depthTex, inUV).r;
    if (depth >= 1.0)
    {
        outColor = vec4(0.05, 0.07, 0.1, 1.0); // sky fallback
        return;
    }

    vec4 gb0Data  = texture(gbufAlbedo,    inUV);
    vec4 gb1Data  = texture(gbufNormal,    inUV);
    vec4 gb2Data  = texture(gbufMetalRough,inUV);

    vec3 albedo    = gb0Data.rgb;
    vec3 normalEnc = gb1Data.rgb;
    float metallic  = gb2Data.r;
    float roughness = clamp(gb2Data.g, 0.04, 1.0);
    vec3 emissive   = vec3(gb0Data.a, gb2Data.b, gb2Data.a);

    vec3 N    = normalize(normalEnc * 2.0 - 1.0);
    vec3 posWS = ReconstructWorldPos(inUV, depth);
    vec3 V    = normalize(eyePosition.xyz - posWS);
    vec3 L    = normalize(lightDir.xyz);
    vec3 H    = normalize(V + L);
    vec3 R    = reflect(-V, N);

    albedo = pow(max(albedo, vec3(0.0001)), vec3(2.2)); // sRGB → linear

    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    // Direct sun (no shadow)
    float  D = D_GGX(N, H, roughness);
    float  G = G_Smith(N, V, L, roughness);
    vec3   F = F_Schlick(max(dot(H, V), 0.0), F0);
    vec3   specularDir = (D * G * F) / max(4.0 * dot(N, V) * dot(N, L), 0.001);
    vec3   kD_dir = (1.0 - F) * (1.0 - metallic);
    vec3   diffuseDir = kD_dir * albedo / PI;
    float  NdL = max(dot(N, L), 0.0);
    vec3   rad = lightColor.rgb * lightColor.w;
    vec3   Lo  = (diffuseDir + specularDir) * rad * NdL;

    // IBL ambient
    float  NdV   = max(dot(N, V), 0.0);
    vec3   F_ibl = F_SchlickRoughness(NdV, F0, roughness);
    vec3   kD_ibl = (1.0 - F_ibl) * (1.0 - metallic);

    vec3 irradiance       = texture(irrMap, N).rgb;
    vec3 diffuseIBL       = kD_ibl * irradiance * albedo;
    float mipLevel        = roughness * float(PREFILTER_MIP_COUNT - 1u);
    vec3 prefilteredColor = textureLod(prefilterMap, R, mipLevel).rgb;
    vec2 brdf             = texture(brdfLUT, vec2(NdV, roughness)).rg;
    vec3 specularIBL      = prefilteredColor * (F_ibl * brdf.x + brdf.y);
    vec3 ambient          = diffuseIBL + specularIBL;

    vec3 emissiveLin = pow(max(emissive, vec3(0.0001)), vec3(2.2));
    vec3 color = ambient + Lo + emissiveLin;

    outColor = vec4(color, 1.0);
}
