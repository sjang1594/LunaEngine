// irradiance_conv.comp.hlsl — Phase 14: IBL precompute
// Hemispherical irradiance convolution: envCubemap → irrCubemap (32²×6).
// Used for diffuse IBL (Lambertian ambient term).
// Dispatch(ceil(W/8), ceil(H/8), 6)  where W=H=faceSize (typically 32).

cbuffer CB : register(b0)
{
    uint  gFaceSize;
    uint3 _pad;
};

TextureCube<float4>      gEnvCube : register(t0);
RWTexture2DArray<float4> gIrrOut  : register(u0);
SamplerState             gSampler : register(s0);

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

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gFaceSize || id.y >= gFaceSize) return;

    float2 uv = (float2(id.xy) + 0.5f) / float(gFaceSize) * 2.0f - 1.0f;
    float3 N  = FaceDir(id.z, uv);

    // Build tangent frame around N
    float3 up    = (abs(N.y) < 0.999f) ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 right = normalize(cross(up, N));
    up           = cross(N, right);

    float3 irr   = 0.0f;
    float  count = 0.0f;

    // Uniform hemisphere sampling — sufficient for low-frequency irradiance
    float dPhi   = 0.025f;
    float dTheta = 0.025f;

    for (float phi = 0.0f; phi < 2.0f * PI; phi += dPhi)
    {
        for (float theta = 0.0f; theta < 0.5f * PI; theta += dTheta)
        {
            // Spherical → tangent-space
            float3 ts  = float3(sin(theta) * cos(phi),
                                sin(theta) * sin(phi),
                                cos(theta));
            float3 dir = ts.x * right + ts.y * up + ts.z * N;

            // cos(θ)·sin(θ) = Jacobian for hemisphere area element
            irr   += gEnvCube.SampleLevel(gSampler, dir, 0.0f).rgb
                     * cos(theta) * sin(theta);
            count += 1.0f;
        }
    }

    irr = PI * irr / count;
    gIrrOut[id] = float4(irr, 1.0f);
}

