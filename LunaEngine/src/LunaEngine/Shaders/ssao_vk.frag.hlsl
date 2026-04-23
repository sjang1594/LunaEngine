// ssao_vk.frag.hlsl — Screen-Space Ambient Occlusion (Vulkan SM 6.0)
// Half-res pass: 16-tap hemisphere kernel, writes raw occlusion to R8_UNORM.
//
// Descriptor layout:
//   set=0, binding=0 — SSAOConstants UBO (samples + proj + invProj + view + noiseScale + radius + bias)
//   set=1, binding=0 — depthTex   (D32_SFLOAT as R32_SFLOAT)
//   set=1, binding=1 — normalTex  (RGBA16F, world-space normal encoded [0,1])
//   set=1, binding=2 — noiseTex   (R8G8_UNORM, 4×4 random rotations)
//   set=1, binding=3 — pointClamp sampler
//   set=1, binding=4 — pointWrap  sampler (for tiled noise)

static const int SAMPLE_COUNT = 16;

[[vk::binding(0, 0)]]
cbuffer SSAOConstants : register(b0)
{
    float4             samples[SAMPLE_COUNT];
    row_major float4x4 projection;
    row_major float4x4 invProjection;
    row_major float4x4 view;
    float2             noiseScale;
    float              radius;
    float              bias;
};

[[vk::binding(0, 1)]] Texture2D<float> depthTex   : register(t0);
[[vk::binding(1, 1)]] Texture2D        normalTex  : register(t1);
[[vk::binding(2, 1)]] Texture2D        noiseTex   : register(t2);
[[vk::binding(3, 1)]] SamplerState     pointClamp : register(s0);
[[vk::binding(4, 1)]] SamplerState     pointWrap  : register(s1);

struct PSInput
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float3 ReconstructVSPos(float2 uv, float depth)
{
    float2 ndc  = float2(uv.x * 2.0 - 1.0, (1.0 - uv.y) * 2.0 - 1.0);
    float4 clip = float4(ndc, depth, 1.0);
    float4 vsP  = mul(clip, invProjection);
    return vsP.xyz / vsP.w;
}

float main(PSInput input) : SV_Target
{
    float depth = depthTex.Sample(pointClamp, input.uv);
    if (depth >= 1.0) return 1.0;

    float3 posVS    = ReconstructVSPos(input.uv, depth);
    float3 normalWS = normalize(normalTex.Sample(pointClamp, input.uv).rgb * 2.0 - 1.0);
    float3 normalVS = normalize(mul(float4(normalWS, 0.0), view).xyz);

    float2 noiseVec  = noiseTex.Sample(pointWrap, input.uv * noiseScale).rg * 2.0 - 1.0;
    float3 randomVec = normalize(float3(noiseVec, 0.0));

    float3 tangent   = normalize(randomVec - normalVS * dot(randomVec, normalVS));
    float3 bitangent = cross(normalVS, tangent);
    float3x3 TBN     = float3x3(tangent, bitangent, normalVS);

    float occlusion = 0.0;
    [unroll]
    for (int i = 0; i < SAMPLE_COUNT; i++)
    {
        float3 sampleDir = mul(samples[i].xyz, TBN);
        float3 samplePos = posVS + sampleDir * radius;

        float4 projPos  = mul(float4(samplePos, 1.0), projection);
        projPos.xyz    /= projPos.w;
        float2 sampleUV = saturate(float2(projPos.x * 0.5 + 0.5,
                                          0.5 - projPos.y * 0.5));

        float storedDepth = depthTex.Sample(pointClamp, sampleUV);
        float3 storedVS   = ReconstructVSPos(sampleUV, storedDepth);

        float rangeCheck = smoothstep(0.0, 1.0,
                            radius / max(abs(posVS.z - storedVS.z), 0.0001));
        occlusion += (samplePos.z >= storedVS.z + bias) ? rangeCheck : 0.0;
    }

    return 1.0 - occlusion / float(SAMPLE_COUNT);
}

