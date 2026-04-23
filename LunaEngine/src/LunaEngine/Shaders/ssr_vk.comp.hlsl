// ssr_vk.comp.hlsl — Screen-Space Reflections compute shader (Vulkan, Phase 16C)
// Same algorithm as ssr.comp.hlsl with explicit [[vk::binding]] annotations.
// set=0: UBO + all SRVs + UAV + samplers

[[vk::binding(0, 0)]] cbuffer SSRConstants : register(b0)
{
    row_major float4x4 view;
    row_major float4x4 proj;
    row_major float4x4 invViewProj;
    float3   eyePos;       float maxDistance;
    uint2    screenSize;   uint  maxSteps; float stepSize;
    float    thickness;    float maxRoughness; float2 _pad0;
    float4   _pad1;
};

[[vk::binding(1, 0)]] Texture2D<float>    depthTex      : register(t0);
[[vk::binding(2, 0)]] Texture2D<float4>   normalTex     : register(t1);
[[vk::binding(3, 0)]] Texture2D<float4>   metalRoughTex : register(t2);
[[vk::binding(4, 0)]] Texture2D<float4>   sceneHDR      : register(t3);
[[vk::binding(5, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> ssrOut : register(u0);
[[vk::binding(6, 0)]] SamplerState        pointClamp    : register(s0);
[[vk::binding(7, 0)]] SamplerState        linearClamp   : register(s1);

float3 ReconstructWorldPos(float2 uv, float depth)
{
    float4 ndc;
    ndc.x =  uv.x * 2.0f - 1.0f;
    ndc.y = -uv.y * 2.0f + 1.0f;
    ndc.z = depth;
    ndc.w = 1.0f;
    float4 ws = mul(ndc, invViewProj);
    return ws.xyz / ws.w;
}

float3 WorldToViewPos(float3 posWS)
{
    float4 vs = mul(float4(posWS, 1.0f), view);
    return vs.xyz;
}

float2 ProjectToUV(float3 posVS)
{
    float4 clip = mul(float4(posVS, 1.0f), proj);
    clip.xyz /= clip.w;
    return float2(clip.x * 0.5f + 0.5f, -clip.y * 0.5f + 0.5f);
}

float SampleLinearDepthVS(float2 uv)
{
    float d = depthTex.SampleLevel(pointClamp, uv, 0);
    float4 ndc = float4(uv.x * 2.0f - 1.0f, -uv.y * 2.0f + 1.0f, d, 1.0f);
    float4 vs  = mul(ndc, invViewProj);
    vs.xyz /= vs.w;
    float4 vsPos = mul(float4(vs.xyz, 1.0f), view);
    return vsPos.z;
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= screenSize.x || tid.y >= screenSize.y)
    {
        ssrOut[tid.xy] = float4(0, 0, 0, 0);
        return;
    }

    float2 uv    = (float2(tid.xy) + 0.5f) / float2(screenSize);
    float  depth = depthTex.SampleLevel(pointClamp, uv, 0);

    if (depth >= 1.0f)
    {
        ssrOut[tid.xy] = float4(0, 0, 0, 0);
        return;
    }

    float4 mr        = metalRoughTex.SampleLevel(pointClamp, uv, 0);
    float  metallic  = mr.r;
    float  roughness = mr.g;

    if (roughness > maxRoughness || metallic < 0.05f)
    {
        ssrOut[tid.xy] = float4(0, 0, 0, 0);
        return;
    }

    float3 posWS   = ReconstructWorldPos(uv, depth);
    float3 V       = normalize(eyePos - posWS);

    float4 normalSample = normalTex.SampleLevel(pointClamp, uv, 0);
    float3 normalWS     = normalize(normalSample.rgb * 2.0f - 1.0f);

    float3 R        = reflect(-V, normalWS);
    float3 rayDirVS = normalize(mul(float4(R, 0.0f), view).xyz);
    float3 rayOriVS = WorldToViewPos(posWS);
    rayOriVS += rayDirVS * stepSize;

    float  hitWeight = 0.0f;
    float2 hitUV     = uv;

    [loop]
    for (uint i = 0; i < maxSteps; ++i)
    {
        float3 sampleVS = rayOriVS + rayDirVS * (stepSize * float(i));
        if (sampleVS.z > -0.01f) break;

        float2 sampleUV = ProjectToUV(sampleVS);
        if (any(sampleUV < 0.0f) || any(sampleUV > 1.0f)) break;

        float gbufDepthVS = SampleLinearDepthVS(sampleUV);
        float rayDepthVS  = sampleVS.z;

        if (rayDepthVS < gbufDepthVS && rayDepthVS > gbufDepthVS - thickness)
        {
            float stepFade  = 1.0f - (float(i) / float(maxSteps));
            float edgeFadeX = min(sampleUV.x, 1.0f - sampleUV.x) * 10.0f;
            float edgeFadeY = min(sampleUV.y, 1.0f - sampleUV.y) * 10.0f;
            float edgeFade  = saturate(edgeFadeX) * saturate(edgeFadeY);
            float roughFade = 1.0f - saturate(roughness / maxRoughness);

            hitWeight = stepFade * edgeFade * roughFade * metallic;
            hitUV     = sampleUV;
            break;
        }
    }

    float3 reflColor   = sceneHDR.SampleLevel(linearClamp, hitUV, 0).rgb;
    ssrOut[tid.xy]     = float4(reflColor * hitWeight, hitWeight);
}
