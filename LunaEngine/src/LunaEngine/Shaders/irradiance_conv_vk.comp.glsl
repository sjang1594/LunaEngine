// irradiance_conv_vk.comp.glsl — Vulkan IBL precompute (Vulkan GLSL 4.60, Phase 15C)
// Hemispherical irradiance convolution: envCubemap → irrCubemap (32²×6).
// Dispatch(ceil(faceSize/8), ceil(faceSize/8), 6)
//
// Bindings (set=0):
//   binding=0 — CB { uint gFaceSize; } (UBO)
//   binding=1 — textureCube gEnvCube
//   binding=2 — image2DArray gIrrOut (rgba16f)
//   binding=3 — sampler gSampler
#version 460

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform CB {
    uint gFaceSize;
    uint _pad0;
    uint _pad1;
    uint _pad2;
};

layout(set = 0, binding = 1) uniform textureCube  gEnvCube;
layout(set = 0, binding = 2, rgba16f) uniform image2DArray gIrrOut;
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

    vec2 uv = (vec2(id.xy) + 0.5) / float(gFaceSize) * 2.0 - 1.0;
    vec3 N  = FaceDir(id.z, uv);

    vec3 up    = (abs(N.y) < 0.999) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 right = normalize(cross(up, N));
    up         = cross(N, right);

    vec3  irr   = vec3(0.0);
    float count = 0.0;

    float dPhi   = 0.025;
    float dTheta = 0.025;

    for (float phi = 0.0; phi < 2.0 * PI; phi += dPhi)
    {
        for (float theta = 0.0; theta < 0.5 * PI; theta += dTheta)
        {
            vec3 ts  = vec3(sin(theta) * cos(phi),
                            sin(theta) * sin(phi),
                            cos(theta));
            vec3 dir = ts.x * right + ts.y * up + ts.z * N;

            irr   += textureLod(samplerCube(gEnvCube, gSampler), dir, 0.0).rgb
                     * cos(theta) * sin(theta);
            count += 1.0;
        }
    }

    irr = PI * irr / count;
    imageStore(gIrrOut, ivec3(id), vec4(irr, 1.0));
}
