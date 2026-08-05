#version 460
// vol_inject_vk.comp.glsl — Phase 29: Volumetric Fog — Material Injection (Vulkan)
// Fills the 3D froxel volume with fog density and scattering coefficients.
//
// set=0, binding=0: VolumetricParams UBO
// set=0, binding=1: froxelVolume (image3D, rgba16f, writeonly)
//
// Dispatch: (ceil(FROXEL_X/8), ceil(FROXEL_Y/8), ceil(FROXEL_Z/4))

layout(local_size_x = 8, local_size_y = 8, local_size_z = 4) in;

const uint FROXEL_X = 160u;
const uint FROXEL_Y = 90u;
const uint FROXEL_Z = 64u;

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

layout(set = 0, binding = 1, rgba16f) writeonly uniform image3D froxelVolume;

// Reconstruct world-space center of a froxel
vec3 froxelCenterWorld(uvec3 id)
{
    vec2 uv = (vec2(id.xy) + 0.5) / vec2(FROXEL_X, FROXEL_Y);

    float tNear = params.nearZ * pow(params.farZ / params.nearZ, float(id.z)      / float(FROXEL_Z));
    float tFar  = params.nearZ * pow(params.farZ / params.nearZ, float(id.z + 1u) / float(FROXEL_Z));
    float viewZ = -(tNear + tFar) * 0.5; // view space, negative Z forward

    // Vulkan NDC: Y is flipped vs DX12
    vec4 ndc = vec4(uv.x * 2.0 - 1.0, uv.y * 2.0 - 1.0, 0.5, 1.0);
    vec4 vs  = params.invProj * ndc;
    vs /= vs.w;
    vec3 vsPos = vec3(vs.xy * ((-viewZ) / (-vs.z)), viewZ);

    vec4 world = params.invView * vec4(vsPos, 1.0);
    return world.xyz;
}

float heightFogDensity(vec3 worldPos)
{
    float h = max(0.0, worldPos.y - params.fogBaseHeight);
    return params.fogDensity * exp(-h * params.fogHeightFalloff);
}

void main()
{
    uvec3 id = gl_GlobalInvocationID;
    if (id.x >= FROXEL_X || id.y >= FROXEL_Y || id.z >= FROXEL_Z)
        return;

    vec3  worldPos = froxelCenterWorld(id);
    float density  = heightFogDensity(worldPos);

    float scatter = density * params.scatteringCoeff;
    float extinct = density * params.extinctionCoeff;

    imageStore(froxelVolume, ivec3(id), vec4(scatter, scatter, scatter, extinct));
}
