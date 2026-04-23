// bloom_blur.frag.hlsl — Phase 10: Separable 5-tap Gaussian blur (SM 6.0)
// Single shader used for both horizontal and vertical passes; direction is
// encoded in the root constants as the UV-space texel step.
//
// Root signature (RootSignatureLayout::BloomBlur):
//   params[0] b0 — 4 root constants: float2 texelStep (direction * 1/dim), float2 pad
//   params[1]    — SRV table t0: bloom input buffer  (R11G11B10_FLOAT)
//   s0           — bilinear-clamp

#ifdef __spirv__
struct BlurDirPush { float2 texelStep; float2 _pad; };
[[vk::push_constant]] BlurDirPush _pc;
#define texelStep _pc.texelStep
#else
cbuffer BlurDirCB : register(b0)
{
    float2 texelStep;  // (1/w, 0) for H-pass, (0, 1/h) for V-pass
    float2 _pad;
};
#endif

#ifdef __spirv__
[[vk::binding(0, 0)]]
#endif
Texture2D<float4> inputTex     : register(t0);
#ifdef __spirv__
[[vk::binding(1, 0)]]
#endif
SamplerState      bilinearClamp : register(s0);

// 5-tap Gaussian weights: [1, 4, 6, 4, 1] / 16
static const float kW[5] = { 0.0625, 0.25, 0.375, 0.25, 0.0625 };

struct PSInput { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float4 main(PSInput input) : SV_Target
{
    float4 result = 0.0;
    [unroll]
    for (int i = -2; i <= 2; i++)
        result += inputTex.Sample(bilinearClamp, input.uv + texelStep * (float)i) * kW[i + 2];
    return result;
}

