// skybox.frag.hlsl — Phase 14: environment mapping
// Samples the prefiltered environment cubemap (mip 0 = full resolution).
// Outputs raw linear HDR; tone mapping is applied downstream by tonemapping.frag.hlsl.

TextureCube<float4> gEnvCube : register(t0);
SamplerState        gSampler : register(s0);

struct PSIn
{
    float4 pos    : SV_POSITION;
    float3 rayDir : TEXCOORD0;
};

float4 main(PSIn input) : SV_Target0
{
    float3 color = gEnvCube.SampleLevel(gSampler, normalize(input.rayDir), 0.0f).rgb;
    // Exposure bake: slightly attenuate sky brightness so it doesn't blow out bloom
    color *= 0.8f;
    return float4(color, 1.0f);
}

