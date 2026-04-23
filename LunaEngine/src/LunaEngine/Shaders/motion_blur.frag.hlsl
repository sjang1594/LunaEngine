// motion_blur.frag.hlsl — Phase 18B: Screen-Space Motion Blur
// Algorithm: reconstruct world pos from depth + invViewProj,
//            reproject to prev clip via prevViewProj,
//            velocity = curr NDC - prev NDC,
//            sample HDR along velocity (N taps, exponential weights).

#ifdef __spirv__
[[vk::binding(0, 0)]]
#endif
cbuffer MotionBlurCB : register(b0)
{
    float4x4 invViewProj;   // inverse(view * proj)   — current frame
    float4x4 prevViewProj;  // view * proj             — previous frame
    float2   screenSize;    // viewport width, height (unused — kept for alignment)
    float    shutterScale;  // multiplier on velocity (0=off, 1=full)
    int      numSamples;    // tap count [1..16]
    float2   _pad;
};

#ifdef __spirv__
[[vk::binding(1, 0)]]
#endif
Texture2D<float4> hdrTex   : register(t0);

#ifdef __spirv__
[[vk::binding(2, 0)]]
#endif
Texture2D<float>  depthTex : register(t1);

#ifdef __spirv__
[[vk::binding(3, 0)]]
#endif
SamplerState pointClamp : register(s0);

struct PSInput
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float4 main(PSInput i) : SV_TARGET
{
    float2 uv = i.uv;
    float  d  = depthTex.SampleLevel(pointClamp, uv, 0).r;

    // Reconstruct current NDC (DX convention: Y up, Z in [0,1])
    // uv.y=0 is top → NDC.y=+1; uv.y=1 is bottom → NDC.y=-1
    float4 ndcCurr = float4(uv.x * 2.0f - 1.0f,
                            1.0f - uv.y * 2.0f,
                            d,
                            1.0f);

    // Unproject to world space
    float4 worldH = mul(ndcCurr, invViewProj);
    worldH /= worldH.w;

    // Reproject to previous clip space
    float4 prevClip = mul(worldH, prevViewProj);
    prevClip /= prevClip.w;

    // Velocity in NDC, converted to UV delta
    float2 velNDC = ndcCurr.xy - prevClip.xy;
    float2 velUV  = float2(velNDC.x * 0.5f, -velNDC.y * 0.5f) * shutterScale;

    // Early out if velocity is negligible
    if (dot(velUV, velUV) < 1e-6f)
        return hdrTex.SampleLevel(pointClamp, uv, 0);

    // Clamp velocity to avoid extreme blurs
    float velLen = length(velUV);
    if (velLen > 0.05f)
        velUV = velUV * (0.05f / velLen);

    // N-tap weighted average along velocity
    float4 col    = 0.0f;
    float  weight = 0.0f;
    int    N      = clamp(numSamples, 1, 16);

    for (int k = 0; k < N; k++)
    {
        float  t        = (N > 1) ? ((float)k / (float)(N - 1) - 0.5f) : 0.0f;
        float2 sampleUV = saturate(uv + velUV * t);
        float  w        = exp(-abs(t) * 2.0f);
        col    += hdrTex.SampleLevel(pointClamp, sampleUV, 0) * w;
        weight += w;
    }

    return col / max(weight, 1e-6f);
}
