#version 460
// vol_scatter_vk.comp.glsl — Phase 29: Volumetric Fog — Scattering Accumulation (Vulkan)
// Marches along each froxel column (Z), accumulating in-scattering and transmittance.
//
// set=0, binding=0: VolumetricParams UBO
// set=0, binding=1: froxelInject (sampler3D — inject pass output)
// set=0, binding=2: csmShadow (sampler2DArrayShadow — 4 cascades)
// set=0, binding=3: froxelAccum (image3D rgba16f — accumulation output)
//
// Dispatch: (ceil(FROXEL_X/8), ceil(FROXEL_Y/8), 1)

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

const uint  FROXEL_X   = 160u;
const uint  FROXEL_Y   = 90u;
const uint  FROXEL_Z   = 64u;
const float PI         = 3.14159265;

layout(set = 0, binding = 0, std140) uniform VolumetricParams
{
    mat4  invProj;
    mat4  invView;
    mat4  lightVP[4];
    vec4  cascadeSplits;
    vec3  lightDir;       float _p0;
    vec3  lightColor;     float lightIntensity;
    float nearZ;          float farZ;
    float screenW;        float screenH;
    float fogDensity;     float fogHeightFalloff;
    float fogBaseHeight;  float scatteringCoeff;
    float extinctionCoeff; float phaseG;
    float _pad1[2];
} params;

layout(set = 0, binding = 1, rgba16f) readonly  uniform image3D froxelInject;
layout(set = 0, binding = 2) uniform sampler2DArrayShadow csmShadow;
layout(set = 0, binding = 3, rgba16f) writeonly uniform image3D froxelAccum;

float phaseHG(float cosTheta, float g)
{
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(max(1.0 + g2 - 2.0 * g * cosTheta, 0.001), 1.5));
}

// Froxel slice [z, z+1] thickness in world units (view-space depth extent)
float froxelThickness(uint z)
{
    float tNear = params.nearZ * pow(params.farZ / params.nearZ, float(z)      / float(FROXEL_Z));
    float tFar  = params.nearZ * pow(params.farZ / params.nearZ, float(z + 1u) / float(FROXEL_Z));
    return tFar - tNear;
}

// Reconstruct view-space position for froxel center
vec3 froxelViewPos(uvec3 id)
{
    vec2 uv    = (vec2(id.xy) + 0.5) / vec2(FROXEL_X, FROXEL_Y);
    float tN   = params.nearZ * pow(params.farZ / params.nearZ, float(id.z)      / float(FROXEL_Z));
    float tF   = params.nearZ * pow(params.farZ / params.nearZ, float(id.z + 1u) / float(FROXEL_Z));
    float vz   = -(tN + tF) * 0.5;

    // Vulkan NDC: Y flipped from DX12 convention
    vec4 ndc = vec4(uv.x * 2.0 - 1.0, uv.y * 2.0 - 1.0, 0.5, 1.0);
    vec4 vs  = params.invProj * ndc;
    vs /= vs.w;
    return vec3(vs.xy * ((-vz) / (-vs.z)), vz);
}

vec3 viewToWorld(vec3 vsPos)
{
    return (params.invView * vec4(vsPos, 1.0)).xyz;
}

float sampleCSM(vec3 worldPos, float viewZ)
{
    float absZ = -viewZ;
    uint cascade = 0u;
    if      (absZ < params.cascadeSplits.x) cascade = 0u;
    else if (absZ < params.cascadeSplits.y) cascade = 1u;
    else if (absZ < params.cascadeSplits.z) cascade = 2u;
    else                                    cascade = 3u;

    vec4 lc = params.lightVP[cascade] * vec4(worldPos, 1.0);
    lc.xyz /= lc.w;

    // Vulkan NDC → UV: x [-1,1]→[0,1], y [-1,1]→[0,1]
    vec2 uv = lc.xy * 0.5 + 0.5;
    float compareDepth = lc.z;

    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))) ||
        compareDepth < 0.0 || compareDepth > 1.0)
        return 1.0;

    return texture(csmShadow, vec4(uv, float(cascade), compareDepth - 0.001));
}

void main()
{
    uvec3 id = gl_GlobalInvocationID;
    if (id.x >= FROXEL_X || id.y >= FROXEL_Y)
        return;

    // View direction for phase function
    vec2 uv    = (vec2(id.xy) + 0.5) / vec2(FROXEL_X, FROXEL_Y);
    vec4 ndcD  = vec4(uv.x * 2.0 - 1.0, uv.y * 2.0 - 1.0, 0.5, 1.0);
    vec4 vsD   = params.invProj * ndcD; vsD /= vsD.w;
    vec3 rayW  = normalize((params.invView * vec4(normalize(vsD.xyz), 0.0)).xyz);
    vec3 sunN  = normalize(-params.lightDir);
    float cosT = dot(rayW, sunN);
    float phase = phaseHG(cosT, params.phaseG);

    vec3  accInscatter = vec3(0.0);
    float accTransmit  = 1.0;

    for (uint z = 0u; z < FROXEL_Z; ++z)
    {
        uvec3 coord = uvec3(id.xy, z);

        vec4 density = imageLoad(froxelInject, ivec3(coord));

        float scatter  = density.x;
        float extinct  = density.w;
        float stepSize = froxelThickness(z);

        if (extinct > 1e-5)
        {
            vec3  vsPos  = froxelViewPos(coord);
            vec3  wsPos  = viewToWorld(vsPos);
            float shadow = sampleCSM(wsPos, vsPos.z);

            // Directional light + ambient; multiply scatter by extra factor for visibility
            vec3  Li     = (shadow * phase + 0.3) * params.lightColor * params.lightIntensity * scatter * 10.0;
            float stepT  = exp(-extinct * stepSize);

            accInscatter += accTransmit * Li * (1.0 - stepT) / max(extinct, 1e-5);
            accTransmit  *= stepT;
        }

        imageStore(froxelAccum, ivec3(coord), vec4(accInscatter, accTransmit));
    }
}
