// ssao_blur_vk.frag.hlsl — SSAO 3×3 box blur (Vulkan SM 6.0)
// Half-res blur over raw SSAO R8_UNORM to reduce kernel noise.
//
// Descriptor layout:
//   set=0, binding=0 — ssaoTex    (R8_UNORM, half-res raw SSAO)
//   set=0, binding=1 — pointClamp sampler

[[vk::binding(0, 0)]] Texture2D<float> ssaoTex    : register(t0);
[[vk::binding(1, 0)]] SamplerState     pointClamp : register(s0);

struct PSInput
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float main(PSInput input) : SV_Target
{
    float2 texSize;
    ssaoTex.GetDimensions(texSize.x, texSize.y);
    float2 texelSize = 1.0 / texSize;

    float ao = 0.0;
    [unroll]
    for (int x = -1; x <= 1; x++)
    {
        [unroll]
        for (int y = -1; y <= 1; y++)
        {
            float2 offset = float2(x, y) * texelSize;
            ao += ssaoTex.Sample(pointClamp, input.uv + offset);
        }
    }
    return ao / 9.0;
}

