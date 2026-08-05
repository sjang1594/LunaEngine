#version 450
// probe_update_vk.comp.glsl — Phase 30: Probe irradiance update (Vulkan)

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform ProbeConstants {
    vec4  origin;         // xyz=origin, w=pad
    vec4  spacing;        // xyz=spacing, w=pad
    uvec4 dims;           // xyz=dims (8,4,8), w=pad
    vec4  screenSize;     // xy=screenSize, zw=pad
    mat4  invViewProj;
    uvec4 probeIndexPad;  // x=probeIndex, yzw=pad
} pc;

layout(set = 0, binding = 1) uniform sampler2D    ssgiTex;
layout(set = 0, binding = 2) uniform samplerCube  irrCubemap;
layout(set = 0, binding = 3) uniform sampler2D    depthTex;
layout(set = 0, binding = 4, rgba16f) uniform writeonly image2DArray probeIrrArray;

const uint  PROBE_TEX_SIZE = 16u;
const float PI             = 3.14159265359;

vec3 OctahedralDecode(vec2 f)
{
    vec3 n = vec3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float t = max(-n.z, 0.0);
    // mix(x, y, bvec): returns y where true, x where false
    n.xy += mix(vec2(t), vec2(-t), greaterThanEqual(n.xy, vec2(0.0)));
    return normalize(n);
}

void main()
{
    uint probe = pc.probeIndexPad.x % (pc.dims.x * pc.dims.y * pc.dims.z);
    uint gx    = probe % pc.dims.x;
    uint gy    = (probe / pc.dims.x) % pc.dims.y;
    uint gz    = probe / (pc.dims.x * pc.dims.y);

    vec3 probeWS = pc.origin.xyz + vec3(float(gx), float(gy), float(gz)) * pc.spacing.xyz;

    uint atlasX = gx * PROBE_TEX_SIZE + gl_LocalInvocationID.x;
    uint atlasY = gy * PROBE_TEX_SIZE + gl_LocalInvocationID.y;
    uint slice  = gz;

    vec2 octUV = (vec2(gl_LocalInvocationID.xy) + 0.5) / float(PROBE_TEX_SIZE);
    vec2 octF  = octUV * 2.0 - 1.0;
    vec3 dir   = OctahedralDecode(octF);

    vec3 iblIrr = texture(irrCubemap, dir).rgb;

    vec3 radiance = iblIrr;

    imageStore(probeIrrArray, ivec3(atlasX, atlasY, slice), vec4(radiance, 1.0));
}
