// equirect_to_cube_vk.comp.hlsl — Vulkan IBL precompute (Phase 15C)
// Converts equirectangular HDR panorama → 6-face RWTexture2DArray cubemap.
// Dispatch(ceil(faceSize/8), ceil(faceSize/8), 6)
//
// Bindings (set=0):
//   binding=0 — cbuffer CB { uint gFaceSize; }
//   binding=1 — Texture2D<float4> gEquirect
//   binding=2 — RWTexture2DArray<float4> gCubeOut  (VK_IMAGE_VIEW_TYPE_2D_ARRAY, all 6 layers)
//   binding=3 — SamplerState gSampler

[[vk::binding(0, 0)]]
cbuffer CB : register(b0)
{
    uint  gFaceSize;
    uint3 _pad;
};

[[vk::binding(1, 0)]] Texture2D<float4>        gEquirect : register(t0);
[[vk::binding(2, 0)]] [[vk::image_format("rgba16f")]] RWTexture2DArray<float4> gCubeOut  : register(u0);
[[vk::binding(3, 0)]] SamplerState             gSampler  : register(s0);

static const float PI = 3.14159265359f;

// Maps cubemap face index + normalized [-1,1] UV → world-space direction.
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

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gFaceSize || id.y >= gFaceSize) return;

    float2 uv  = (float2(id.xy) + 0.5f) / float(gFaceSize) * 2.0f - 1.0f;
    float3 dir = FaceDir(id.z, uv);

    float phi   = atan2(dir.z, dir.x);
    float theta = asin(clamp(dir.y, -1.0f, 1.0f));
    float2 eq   = float2(phi / (2.0f * PI) + 0.5f,
                         0.5f - theta / PI);

    gCubeOut[id] = gEquirect.SampleLevel(gSampler, eq, 0.0f);
}
