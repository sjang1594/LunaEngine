// ssao_blur.frag.hlsl — Phase 9: SSAO 3×3 box blur
// Runs at half-res over the raw SSAO R8_UNORM result to reduce kernel noise.
//
// Root signature (RootSignatureLayout::SSAOBlur):
//   params[0] — SRV table t0: raw SSAO texture (R8_UNORM, half-res)
//   s0        — point-clamp

Texture2D<float> ssaoTex   : register(t0);
SamplerState     pointClamp : register(s0);

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

