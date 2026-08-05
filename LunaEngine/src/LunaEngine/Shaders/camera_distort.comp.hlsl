// camera_distort.comp.hlsl — S2: camera sensor post-processing
// Applies Brown-Conrady lens distortion, exposure, shot+read noise, and sRGB response curve.
// Input: linear HDR litRT (RGBA16F). Output: RGBA8_UNORM distortRT.
//
// Root signature (RootSignatureLayout::CameraDistort):
//   b0 = DistortConstants CBV
//   t0 = litRT SRV (RGBA16F)
//   u0 = distortRT UAV (RGBA8_UNORM / R32_UINT reinterpret)
//   s0 = bilinear-clamp

cbuffer DistortConstants : register(b0)
{
    // Brown-Conrady coefficients (k1,k2,k3 radial; p1,p2 tangential)
    float k1, k2, k3;
    float p1, p2;
    // Pinhole intrinsics (pixels)
    float fx, fy, cx, cy;
    // Radiometric
    float exposureEV100;   // exposure value at ISO 100
    float shotNoiseFactor; // scale on Poisson shot noise approximation
    float readNoiseSigma;  // Gaussian read noise std-dev
    // Resolution
    uint screenW, screenH;
    uint _pad0, _pad1;
};

Texture2D<float4>    litRT     : register(t0);
RWTexture2D<float4>  distortRT : register(u0);
SamplerState         bilinear  : register(s0);

static const float PI = 3.14159265359f;

// ---- Pseudo-random number generator (Laine-Karras hash) ----
uint lcg(uint v) { return v * 1664525u + 1013904223u; }
float uintToFloat01(uint v) { return (v >> 9u) * (1.0f / float(1u << 23u)); }

float randGaussian(uint seed0, uint seed1)
{
    // Box-Muller: two uniform samples → one Gaussian
    float u1 = max(uintToFloat01(lcg(seed0)), 1e-7f);
    float u2 = uintToFloat01(lcg(seed1));
    return sqrt(-2.0f * log(u1)) * cos(2.0f * PI * u2);
}

// ---- sRGB response ----
float linear_to_srgb(float v)
{
    v = saturate(v);
    return (v <= 0.0031308f) ? (12.92f * v) : (1.055f * pow(v, 1.0f / 2.4f) - 0.055f);
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= screenW || tid.y >= screenH) return;

    // ---------- 1. Undistorted → distorted UV mapping ----------
    // Convert output pixel to normalized sensor coords
    float xn = (float(tid.x) - cx) / fx;
    float yn = (float(tid.y) - cy) / fy;

    float r2 = xn * xn + yn * yn;
    float r4 = r2 * r2;
    float r6 = r4 * r2;

    // Radial + tangential distortion
    float radial = 1.0f + k1 * r2 + k2 * r4 + k3 * r6;
    float xd = xn * radial + 2.0f * p1 * xn * yn       + p2 * (r2 + 2.0f * xn * xn);
    float yd = yn * radial + p1 * (r2 + 2.0f * yn * yn) + 2.0f * p2 * xn * yn;

    // Back to pixel coords, then UV
    float srcX = xd * fx + cx;
    float srcY = yd * fy + cy;
    float2 uv  = float2(srcX / float(screenW), srcY / float(screenH));

    // ---------- 2. Sample litRT ----------
    float3 hdr = litRT.SampleLevel(bilinear, uv, 0.0f).rgb;

    // ---------- 3. Exposure ----------
    float ev = pow(2.0f, exposureEV100);
    hdr *= ev;

    // ---------- 4. Noise ----------
    uint seed = (tid.x * 1973u + tid.y * 9277u);

    if (shotNoiseFactor > 0.0f)
    {
        float lum = max(dot(hdr, float3(0.2126f, 0.7152f, 0.0722f)), 0.0f);
        float shotSigma = sqrt(max(lum * shotNoiseFactor, 0.0f));
        hdr.r += shotSigma * randGaussian(lcg(seed + 0u), lcg(seed + 1u));
        hdr.g += shotSigma * randGaussian(lcg(seed + 2u), lcg(seed + 3u));
        hdr.b += shotSigma * randGaussian(lcg(seed + 4u), lcg(seed + 5u));
    }

    if (readNoiseSigma > 0.0f)
    {
        hdr.r += readNoiseSigma * randGaussian(lcg(seed + 6u),  lcg(seed + 7u));
        hdr.g += readNoiseSigma * randGaussian(lcg(seed + 8u),  lcg(seed + 9u));
        hdr.b += readNoiseSigma * randGaussian(lcg(seed + 10u), lcg(seed + 11u));
    }

    // ---------- 5. sRGB response + clamp ----------
    float3 srgb = float3(
        linear_to_srgb(hdr.r),
        linear_to_srgb(hdr.g),
        linear_to_srgb(hdr.b));

    distortRT[tid.xy] = float4(srgb, 1.0f);
}
