// brdf_lut_vk.comp.glsl — Vulkan IBL precompute (Vulkan GLSL 4.60, Phase 15C)
// Precomputes BRDF split-sum LUT: LUT.r=scale, LUT.g=bias.
// Output: R16G16_SFLOAT 512×512. Dispatch(ceil(512/16), ceil(512/16), 1)
//
// Bindings (set=0):
//   binding=0 — image2D gLutOut (rg16f)
#version 460

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

layout(set = 0, binding = 0, rg16f) uniform image2D gLutOut;

const float PI = 3.14159265359;

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
    H.x = sinTheta * cos(phi); H.y = sinTheta * sin(phi); H.z = cosTheta;
    vec3 up    = (abs(N.z) < 0.999) ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 right = normalize(cross(up, N));
    vec3 fwd   = cross(N, right);
    return normalize(right * H.x + fwd * H.y + N * H.z);
}

float G_SchlickGGX(float NdV, float roughness)
{
    float a = roughness;
    float k = (a * a) / 2.0;
    return NdV / (NdV * (1.0 - k) + k);
}

float G_Smith(float NdV, float NdL, float roughness)
{
    return G_SchlickGGX(NdV, roughness) * G_SchlickGGX(NdL, roughness);
}

void main()
{
    ivec2 id  = ivec2(gl_GlobalInvocationID.xy);
    ivec2 dim = imageSize(gLutOut);
    if (any(greaterThanEqual(id, dim))) return;

    float NdV       = max((float(id.x) + 0.5) / float(dim.x), 1e-4);
    float roughness = (float(id.y) + 0.5) / float(dim.y);

    vec3 V;
    V.x = sqrt(1.0 - NdV * NdV);
    V.y = 0.0;
    V.z = NdV;

    vec3 N = vec3(0.0, 0.0, 1.0);

    float A = 0.0, B = 0.0;
    const uint SAMPLE_COUNT = 1024u;

    for (uint i = 0u; i < SAMPLE_COUNT; ++i)
    {
        vec2 Xi = Hammersley(i, SAMPLE_COUNT);
        vec3 H  = ImportanceSampleGGX(Xi, N, max(roughness, 0.001));
        vec3 L  = normalize(2.0 * dot(V, H) * H - V);

        float NdL = max(L.z, 0.0);
        float NdH = max(H.z, 0.0);
        float VdH = max(dot(V, H), 0.0);

        if (NdL > 0.0)
        {
            float G     = G_Smith(NdV, NdL, roughness);
            float G_vis = (G * VdH) / (NdH * NdV + 1e-6);
            float Fc    = pow(1.0 - VdH, 5.0);
            A += (1.0 - Fc) * G_vis;
            B += Fc          * G_vis;
        }
    }

    imageStore(gLutOut, id, vec4(vec2(A, B) / float(SAMPLE_COUNT), 0.0, 0.0));
}
