// prefilter_env_vk.comp.glsl — Vulkan IBL precompute (Vulkan GLSL 4.60, Phase 15C)
// GGX importance-sampled prefiltered environment map.
// One dispatch per mip level; roughness = mipLevel / (mipCount - 1).
// Dispatch(ceil(faceSize/8), ceil(faceSize/8), 6) per mip.
//
// Bindings (set=0):
//   binding=0 — CB { uint gFaceSize; uint gMipLevel; uint gNumMips; float gRoughness; }
//   binding=1 — textureCube gEnvCube
//   binding=2 — image2DArray gPrefOut (rgba16f, specific mip view)
//   binding=3 — sampler gSampler
#version 460

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform CB {
    uint  gFaceSize;
    uint  gMipLevel;
    uint  gNumMips;
    float gRoughness;
};

layout(set = 0, binding = 1) uniform textureCube  gEnvCube;
layout(set = 0, binding = 2, rgba16f) uniform image2DArray gPrefOut;
layout(set = 0, binding = 3) uniform sampler      gSampler;

const float PI = 3.14159265359;

vec3 FaceDir(uint face, vec2 uv)
{
    switch (face)
    {
        case 0u: return normalize(vec3( 1.0, -uv.y, -uv.x));
        case 1u: return normalize(vec3(-1.0, -uv.y,  uv.x));
        case 2u: return normalize(vec3( uv.x,  1.0,  uv.y));
        case 3u: return normalize(vec3( uv.x, -1.0, -uv.y));
        case 4u: return normalize(vec3( uv.x, -uv.y,  1.0));
        default: return normalize(vec3(-uv.x, -uv.y, -1.0));
    }
}

vec2 Hammersley(uint i, uint N)
{
    uint bits = i;
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    float vdc = float(bits) * 2.3283064365386963e-10;
    return vec2(float(i) / float(N), vdc);
}

vec3 ImportanceSampleGGX(vec2 Xi, vec3 N, float roughness)
{
    float a  = roughness * roughness;
    float a2 = a * a;
    float phi      = 2.0 * PI * Xi.x;
    float cosTheta = sqrt((1.0 - Xi.y) / max(1.0 + (a2 - 1.0) * Xi.y, 1e-6));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);

    vec3 H;
    H.x = sinTheta * cos(phi);
    H.y = sinTheta * sin(phi);
    H.z = cosTheta;

    vec3 up    = (abs(N.z) < 0.999) ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 right = normalize(cross(up, N));
    vec3 fwd   = cross(N, right);
    return normalize(right * H.x + fwd * H.y + N * H.z);
}

void main()
{
    uvec3 id = gl_GlobalInvocationID;
    if (id.x >= gFaceSize || id.y >= gFaceSize) return;

    vec2 uv = (vec2(id.xy) + 0.5) / float(gFaceSize) * 2.0 - 1.0;
    vec3 N  = FaceDir(id.z, uv);
    vec3 V  = N;  // split-sum: V = R = N

    vec3  prefilteredColor = vec3(0.0);
    float totalWeight      = 0.0;
    const uint SAMPLE_COUNT = 1024u;

    for (uint i = 0u; i < SAMPLE_COUNT; ++i)
    {
        vec2 Xi = Hammersley(i, SAMPLE_COUNT);
        vec3 H  = ImportanceSampleGGX(Xi, N, max(gRoughness, 0.001));
        vec3 L  = normalize(2.0 * dot(V, H) * H - V);

        float NdL = max(dot(N, L), 0.0);
        if (NdL > 0.0)
        {
            prefilteredColor += textureLod(samplerCube(gEnvCube, gSampler), L, 0.0).rgb * NdL;
            totalWeight      += NdL;
        }
    }

    prefilteredColor = (totalWeight > 0.0) ? prefilteredColor / totalWeight : vec3(0.0);
    imageStore(gPrefOut, ivec3(id), vec4(prefilteredColor, 1.0));
}
