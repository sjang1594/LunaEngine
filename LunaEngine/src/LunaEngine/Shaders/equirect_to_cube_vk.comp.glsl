// equirect_to_cube_vk.comp.glsl — Vulkan IBL precompute (Vulkan GLSL 4.60, Phase 15C)
// Converts equirectangular HDR panorama → 6-face image2DArray cubemap.
// Dispatch(ceil(faceSize/8), ceil(faceSize/8), 6)
//
// Bindings (set=0):
//   binding=0 — CB { uint gFaceSize; } (UBO)
//   binding=1 — texture2D gEquirect (SAMPLED_IMAGE)
//   binding=2 — image2DArray gCubeOut (rgba16f, STORAGE_IMAGE)
//   binding=3 — sampler gSampler
#version 460

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform CB {
    uint  gFaceSize;
    uvec3 _pad;
};

layout(set = 0, binding = 1) uniform texture2D    gEquirect;
layout(set = 0, binding = 2, rgba16f) uniform image2DArray gCubeOut;
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

void main()
{
    uvec3 id = gl_GlobalInvocationID;
    if (id.x >= gFaceSize || id.y >= gFaceSize) return;

    vec2 uv  = (vec2(id.xy) + 0.5) / float(gFaceSize) * 2.0 - 1.0;
    vec3 dir = FaceDir(id.z, uv);

    float phi   = atan(dir.z, dir.x);
    float theta = asin(clamp(dir.y, -1.0, 1.0));
    vec2  eq    = vec2(phi / (2.0 * PI) + 0.5,
                       0.5 - theta / PI);

    vec4 color = textureLod(sampler2D(gEquirect, gSampler), eq, 0.0);
    imageStore(gCubeOut, ivec3(id), color);
}
