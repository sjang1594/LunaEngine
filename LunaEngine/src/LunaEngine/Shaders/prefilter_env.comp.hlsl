// prefilter_env.comp.hlsl — Phase 14: IBL precompute
// GGX importance-sampled prefiltered environment map for specular split-sum.
// One dispatch per mip level; roughness = mipLevel / (mipCount - 1).
// Dispatch(ceil(W/8), ceil(H/8), 6) per mip.

cbuffer CB : register(b0)
{
    uint  gFaceSize;   // size of this mip level (e.g. 128 >> mipLevel)
    uint  gMipLevel;
    uint  gRoughnessBits; // float roughness bit-cast as uint (use asfloat)
    uint  gSampleCount;   // typically 1024
};

TextureCube<float4>      gEnvCube    : register(t0);
RWTexture2DArray<float4> gPrefilter  : register(u0);
SamplerState             gSampler    : register(s0);

static const float PI = 3.14159265359f;

float3 FaceDir(uint face, float2 uv)
{
    switch (face)
    {
        case 0: return normalize(float3( 1.0f, -uv.y, -uv.x)); // +X
        case 1: return normalize(float3(-1.0f, -uv.y,  uv.x)); // -X
        case 2: return normalize(float3( uv.x,  1.0f,  uv.y)); // +Y
        case 3: return normalize(float3( uv.x, -1.0f, -uv.y)); // -Y
        case 4: return normalize(float3( uv.x, -uv.y,  1.0f)); // +Z
        default:return normalize(float3(-uv.x, -uv.y, -1.0f)); // -Z
    }
}

// Hammersley low-discrepancy sequence
float2 Hammersley(uint i, uint N)
{
    // Van der Corput radical inverse
    uint bits = i;
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    float vdc = float(bits) * 2.3283064365386963e-10f; // / 0x100000000
    return float2(float(i) / float(N), vdc);
}

// GGX importance sample half-vector
float3 ImportanceSampleGGX(float2 Xi, float3 N, float roughness)
{
    float a  = roughness * roughness;
    float a2 = a * a;

    float phi      = 2.0f * PI * Xi.x;
    float cosTheta = sqrt((1.0f - Xi.y) / max(1.0f + (a2 - 1.0f) * Xi.y, 1e-6f));
    float sinTheta = sqrt(1.0f - cosTheta * cosTheta);

    float3 H;
    H.x = sinTheta * cos(phi);
    H.y = sinTheta * sin(phi);
    H.z = cosTheta;

    // Tangent → world
    float3 up    = (abs(N.z) < 0.999f) ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 right = normalize(cross(up, N));
    float3 fwd   = cross(N, right);

    return normalize(right * H.x + fwd * H.y + N * H.z);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gFaceSize || id.y >= gFaceSize) return;

    float2 uv = (float2(id.xy) + 0.5f) / float(gFaceSize) * 2.0f - 1.0f;
    float3 N  = FaceDir(id.z, uv);
    float3 R  = N;  // for split-sum, V = R = N (view = reflection direction)
    float3 V  = R;

    float3 prefilteredColor = 0.0f;
    float  totalWeight      = 0.0f;

    uint sampleCount = max(gSampleCount, 1u);
    float roughness  = asfloat(gRoughnessBits);

    for (uint i = 0u; i < sampleCount; ++i)
    {
        float2 Xi = Hammersley(i, sampleCount);
        float3 H  = ImportanceSampleGGX(Xi, N, max(roughness, 0.001f));
        float3 L  = normalize(2.0f * dot(V, H) * H - V);

        float NdL = max(dot(N, L), 0.0f);
        if (NdL > 0.0f)
        {
            prefilteredColor += gEnvCube.SampleLevel(gSampler, L, 0.0f).rgb * NdL;
            totalWeight      += NdL;
        }
    }

    prefilteredColor = (totalWeight > 0.0f)
                       ? prefilteredColor / totalWeight
                       : float3(0.0f, 0.0f, 0.0f);

    gPrefilter[id] = float4(prefilteredColor, 1.0f);
}

