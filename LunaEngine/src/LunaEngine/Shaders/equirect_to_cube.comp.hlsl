// equirect_to_cube.comp.hlsl — Phase 14: IBL precompute
// Converts equirectangular HDR panorama → 6-face RWTexture2DArray cubemap.
// Dispatch(ceil(W/8), ceil(H/8), 6)  where W=H=faceSize.

cbuffer CB : register(b0)
{
    uint  gFaceSize;
    uint3 _pad;
};

Texture2D<float4>        gEquirect : register(t0);
RWTexture2DArray<float4> gCubeOut  : register(u0);
SamplerState             gSampler  : register(s0);

static const float PI = 3.14159265359f;

// Maps cubemap face index + normalized [-1,1] UV → world-space direction.
// Convention matches D3D12 TextureCube face order (LH, v=0 at top of texture).
// For lateral faces (+X/-X/+Z/-Z): v=0 (uv.y=-1) → +Y direction (up).
// For +Y face: v=0 → -Z direction.  For -Y face: v=0 → +Z direction.
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

    // Pixel center in [-1, 1]
    float2 uv  = (float2(id.xy) + 0.5f) / float(gFaceSize) * 2.0f - 1.0f;
    float3 dir = FaceDir(id.z, uv);

    // Equirectangular UV: phi=[0,2π], theta=[-π/2,π/2]
    float phi   = atan2(dir.z, dir.x);          // [-π, π]
    float theta = asin(clamp(dir.y, -1.0f, 1.0f)); // [-π/2, π/2]
    float2 eq   = float2(phi / (2.0f * PI) + 0.5f,
                         0.5f - theta / PI);    // [0,1]×[0,1]

    gCubeOut[id] = gEquirect.SampleLevel(gSampler, eq, 0.0f);
}

