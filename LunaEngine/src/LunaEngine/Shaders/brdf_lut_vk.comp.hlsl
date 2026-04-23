// brdf_lut_vk.comp.hlsl — Vulkan IBL precompute (Phase 15C)
// Precomputes BRDF split-sum LUT: LUT.r=scale, LUT.g=bias.
// Output: R16G16_SFLOAT 512×512. Dispatch(ceil(512/16), ceil(512/16), 1)
//
// Bindings (set=0):
//   binding=0 — RWTexture2D<float2> gLutOut

[[vk::binding(0, 0)]] [[vk::image_format("rg16f")]] RWTexture2D<float2> gLutOut : register(u0);

static const float PI = 3.14159265359f;

float2 Hammersley(uint i, uint N)
{
    uint bits = i;
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    float vdc = float(bits) * 2.3283064365386963e-10f;
    return float2(float(i) / float(N), vdc);
}

float3 ImportanceSampleGGX(float2 Xi, float3 N, float roughness)
{
    float a  = roughness * roughness;
    float a2 = a * a;
    float phi      = 2.0f * PI * Xi.x;
    float cosTheta = sqrt((1.0f - Xi.y) / max(1.0f + (a2 - 1.0f) * Xi.y, 1e-6f));
    float sinTheta = sqrt(1.0f - cosTheta * cosTheta);
    float3 H;
    H.x = sinTheta * cos(phi); H.y = sinTheta * sin(phi); H.z = cosTheta;
    float3 up    = (abs(N.z) < 0.999f) ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 right = normalize(cross(up, N));
    float3 fwd   = cross(N, right);
    return normalize(right * H.x + fwd * H.y + N * H.z);
}

float G_SchlickGGX(float NdV, float roughness)
{
    float a = roughness;
    float k = (a * a) / 2.0f;
    return NdV / (NdV * (1.0f - k) + k);
}

float G_Smith(float NdV, float NdL, float roughness)
{
    return G_SchlickGGX(NdV, roughness) * G_SchlickGGX(NdL, roughness);
}

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint2 dim;
    gLutOut.GetDimensions(dim.x, dim.y);
    if (any(id.xy >= dim)) return;

    float NdV       = max((float(id.x) + 0.5f) / float(dim.x), 1e-4f);
    float roughness = (float(id.y) + 0.5f) / float(dim.y);

    float3 V;
    V.x = sqrt(1.0f - NdV * NdV);
    V.y = 0.0f;
    V.z = NdV;

    float3 N = float3(0.0f, 0.0f, 1.0f);

    float A = 0.0f, B = 0.0f;
    const uint SAMPLE_COUNT = 1024u;

    for (uint i = 0u; i < SAMPLE_COUNT; ++i)
    {
        float2 Xi = Hammersley(i, SAMPLE_COUNT);
        float3 H  = ImportanceSampleGGX(Xi, N, max(roughness, 0.001f));
        float3 L  = normalize(2.0f * dot(V, H) * H - V);

        float NdL = max(L.z, 0.0f);
        float NdH = max(H.z, 0.0f);
        float VdH = max(dot(V, H), 0.0f);

        if (NdL > 0.0f)
        {
            float G     = G_Smith(NdV, NdL, roughness);
            float G_vis = (G * VdH) / (NdH * NdV + 1e-6f);
            float Fc    = pow(1.0f - VdH, 5.0f);
            A += (1.0f - Fc) * G_vis;
            B += Fc           * G_vis;
        }
    }

    gLutOut[id.xy] = float2(A, B) / float(SAMPLE_COUNT);
}
