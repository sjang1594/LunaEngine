#version 450
// camera_distort_vk.comp.glsl — S2b: Vulkan camera sensor post-processing.
// Brown-Conrady distortion + exposure + shot/read noise + sRGB response.
// Mirrors camera_distort.comp.hlsl (DX12 S2).

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// set=0 — distortion constants
layout(set=0, binding=0) uniform DistortUBO {
    float k1, k2, k3;
    float p1, p2;
    float fx, fy, cx, cy;
    float exposureEV100;
    float shotNoiseFactor;
    float readNoiseSigma;
    uint  screenW, screenH;
    uint  _pad0, _pad1;
};

// set=1 — input HDR (litRT, RGBA16F, sampled)
layout(set=1, binding=0) uniform sampler2D litRT;

// set=2 — output (distortRT, RGBA8, storage)
layout(set=2, binding=0, rgba8) uniform writeonly image2D distortRT;

// ---- PRNG ------------------------------------------------------------------
uint lcg(uint v) { return v * 1664525u + 1013904223u; }
float uintToFloat01(uint v) { return float(v >> 9u) * (1.0 / float(1u << 23u)); }

float randGaussian(uint s0, uint s1)
{
    float u1 = max(uintToFloat01(lcg(s0)), 1e-7);
    float u2 = uintToFloat01(lcg(s1));
    return sqrt(-2.0 * log(u1)) * cos(6.28318530718 * u2);
}

// ---- sRGB response ---------------------------------------------------------
float linear_to_srgb(float v)
{
    v = clamp(v, 0.0, 1.0);
    return (v <= 0.0031308) ? (12.92 * v) : (1.055 * pow(v, 1.0 / 2.4) - 0.055);
}

void main()
{
    uvec2 tid = gl_GlobalInvocationID.xy;
    if (tid.x >= screenW || tid.y >= screenH) return;

    // ── 1. Undistorted → distorted UV mapping ────────────────────────────────
    float xn = (float(tid.x) - cx) / fx;
    float yn = (float(tid.y) - cy) / fy;

    float r2 = xn * xn + yn * yn;
    float r4 = r2 * r2;
    float r6 = r4 * r2;

    float radial = 1.0 + k1 * r2 + k2 * r4 + k3 * r6;
    float xd = xn * radial + 2.0 * p1 * xn * yn       + p2 * (r2 + 2.0 * xn * xn);
    float yd = yn * radial + p1 * (r2 + 2.0 * yn * yn) + 2.0 * p2 * xn * yn;

    float srcX = xd * fx + cx;
    float srcY = yd * fy + cy;
    vec2  uv   = vec2(srcX / float(screenW), srcY / float(screenH));

    // ── 2. Sample litRT ───────────────────────────────────────────────────────
    vec3 hdr = texture(litRT, uv).rgb;

    // ── 3. Exposure ───────────────────────────────────────────────────────────
    float ev = pow(2.0, exposureEV100);
    hdr *= ev;

    // ── 4. Noise ──────────────────────────────────────────────────────────────
    uint seed = tid.x * 1973u + tid.y * 9277u;

    if (shotNoiseFactor > 0.0)
    {
        float lum = max(dot(hdr, vec3(0.2126, 0.7152, 0.0722)), 0.0);
        float shotSigma = sqrt(max(lum * shotNoiseFactor, 0.0));
        hdr.r += shotSigma * randGaussian(lcg(seed + 0u), lcg(seed + 1u));
        hdr.g += shotSigma * randGaussian(lcg(seed + 2u), lcg(seed + 3u));
        hdr.b += shotSigma * randGaussian(lcg(seed + 4u), lcg(seed + 5u));
    }
    if (readNoiseSigma > 0.0)
    {
        hdr.r += readNoiseSigma * randGaussian(lcg(seed + 6u),  lcg(seed + 7u));
        hdr.g += readNoiseSigma * randGaussian(lcg(seed + 8u),  lcg(seed + 9u));
        hdr.b += readNoiseSigma * randGaussian(lcg(seed + 10u), lcg(seed + 11u));
    }

    // ── 5. sRGB response ──────────────────────────────────────────────────────
    vec4 result = vec4(
        linear_to_srgb(hdr.r),
        linear_to_srgb(hdr.g),
        linear_to_srgb(hdr.b),
        1.0);

    imageStore(distortRT, ivec2(tid.xy), result);
}
