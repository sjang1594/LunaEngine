// prefilter_env_vk.comp.hlsl — Vulkan IBL precompute (Phase 15C)
// GGX importance-sampled prefiltered environment map.
// One dispatch per mip level; roughness = mipLevel / (mipCount - 1).
// Dispatch(ceil(faceSize/8), ceil(faceSize/8), 6) per mip.
//
// Bindings (set=0):
//   binding=0 — cbuffer CB { uint gFaceSize; uint gMipLevel; uint gNumMips; float gRoughness; }
//   binding=1 — TextureCube<float4> gEnvCube
//   binding=2 — RWTexture2DArray<float4> gPrefOut  (view for specific mip)
//   binding=3 — SamplerState gSampler

[[vk::binding(0, 0)]]
cbuffer CB : register(b0)
{
    uint  gFaceSize;
    uint  gMipLevel;
    uint  gNumMips;
    float gRoughness;
};

[[vk::binding(1, 0)]] TextureCube<float4>      gEnvCube : register(t0);
[[vk::binding(2, 0)]] [[vk::image_format("rgba16f")]] RWTexture2DArray<float4> gPrefOut : register(u0);
[[vk::binding(3, 0)]] SamplerState             gSampler : register(s0);

static const float PI = 3.14159265359f;

float3 FaceDir(uint face, float2 uv)
{
    switch (face)
    {
        case 0: return normalize(float3( 1.0f, -uv.y, -uv.x));
        case 1: return normalize(float3(-1.0f, -uv.y,  uv.x));
        case 2: return normalize(float3( uv.x,  1.0f,  uv.y));
        case 3: return normalize(float3( uv.x, -1.0f, -uv.y));
        case 4: return normalize(float3( uv.x, -uv.y,  1.0f));
        default:return normalize(float3(-uv.x, -uv.y, -1.0f));
    }
}

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
    H.x = sinTheta * cos(phi);
    H.y = sinTheta * sin(phi);
    H.z = cosTheta;

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
    float3 V  = N;  // split-sum: V = R = N

    float3 prefilteredColor = 0.0f;
    float  totalWeight      = 0.0f;
    const uint SAMPLE_COUNT = 1024u;

    for (uint i = 0u; i < SAMPLE_COUNT; ++i)
    {
        float2 Xi = Hammersley(i, SAMPLE_COUNT);
        float3 H  = ImportanceSampleGGX(Xi, N, max(gRoughness, 0.001f));
        float3 L  = normalize(2.0f * dot(V, H) * H - V);

        float NdL = max(dot(N, L), 0.0f);
        if (NdL > 0.0f)
        {
            prefilteredColor += gEnvCube.SampleLevel(gSampler, L, 0.0f).rgb * NdL;
            totalWeight      += NdL;
        }
    }

    prefilteredColor = (totalWeight > 0.0f) ? prefilteredColor / totalWeight : 0.0f;
    gPrefOut[id] = float4(prefilteredColor, 1.0f);
}
