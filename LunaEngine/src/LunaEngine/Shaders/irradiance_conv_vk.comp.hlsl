// irradiance_conv_vk.comp.hlsl — Vulkan IBL precompute (Phase 15C)
// Hemispherical irradiance convolution: envCubemap → irrCubemap (32²×6).
// Dispatch(ceil(faceSize/8), ceil(faceSize/8), 6)
//
// Bindings (set=0):
//   binding=0 — cbuffer CB { uint gFaceSize; }
//   binding=1 — TextureCube<float4> gEnvCube
//   binding=2 — RWTexture2DArray<float4> gIrrOut
//   binding=3 — SamplerState gSampler

[[vk::binding(0, 0)]]
cbuffer CB : register(b0)
{
    uint  gFaceSize;
    uint3 _pad;
};

[[vk::binding(1, 0)]] TextureCube<float4>      gEnvCube : register(t0);
[[vk::binding(2, 0)]] [[vk::image_format("rgba16f")]] RWTexture2DArray<float4> gIrrOut  : register(u0);
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

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gFaceSize || id.y >= gFaceSize) return;

    float2 uv = (float2(id.xy) + 0.5f) / float(gFaceSize) * 2.0f - 1.0f;
    float3 N  = FaceDir(id.z, uv);

    float3 up    = (abs(N.y) < 0.999f) ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 right = normalize(cross(up, N));
    up           = cross(N, right);

    float3 irr   = 0.0f;
    float  count = 0.0f;

    float dPhi   = 0.025f;
    float dTheta = 0.025f;

    for (float phi = 0.0f; phi < 2.0f * PI; phi += dPhi)
    {
        for (float theta = 0.0f; theta < 0.5f * PI; theta += dTheta)
        {
            float3 ts  = float3(sin(theta) * cos(phi),
                                sin(theta) * sin(phi),
                                cos(theta));
            float3 dir = ts.x * right + ts.y * up + ts.z * N;

            irr   += gEnvCube.SampleLevel(gSampler, dir, 0.0f).rgb
                     * cos(theta) * sin(theta);
            count += 1.0f;
        }
    }

    irr = PI * irr / count;
    gIrrOut[id] = float4(irr, 1.0f);
}
