// taa.frag.hlsl — Phase 10: Temporal Anti-Aliasing resolve (SM 6.0)
// Depth-based reprojection to previous frame, YCoCg 3×3 neighbourhood clamp,
// exponential blend (alpha ≈ 0.1 → 90% history weight).
//
// Root signature (RootSignatureLayout::TAA):
//   params[0] b0  — TAAConstants CBV
//   params[1]     — SRV table t0: current jittered HDR  (R16G16B16A16_FLOAT)
//   params[2]     — SRV table t1: previous TAA history  (R16G16B16A16_FLOAT)
//   params[3]     — SRV table t2: hardware depth        (R32_FLOAT)
//   s0            — bilinear-clamp (history sampling, reduces aliasing)
//   s1            — point-clamp   (depth + neighbourhood taps)

// vk::binding annotations are only valid for SPIR-V (Vulkan) targets.
// DXC targeting DXIL (DX12) ignores them with a warning — guard them out.
#ifdef __spirv__
[[vk::binding(0, 0)]]
#endif
cbuffer TAAConstants : register(b0)
{
    row_major float4x4 invViewProj;  // current frame inverse (jittered) VP
    row_major float4x4 prevViewProj; // previous frame unjittered VP
    float2             jitter;       // current jitter in NDC space
    float2             prevJitter;   // previous frame jitter
    float              alpha;        // blend weight (1=full current, ~0.1 typical)
    float              _pad[3];
};

#ifdef __spirv__
[[vk::binding(1, 0)]]
#endif
Texture2D<float4> currentFrame : register(t0);
#ifdef __spirv__
[[vk::binding(2, 0)]]
#endif
Texture2D<float4> historyFrame : register(t1);
#ifdef __spirv__
[[vk::binding(3, 0)]]
#endif
Texture2D<float>  depthTex     : register(t2);

#ifdef __spirv__
[[vk::binding(4, 0)]]
#endif
SamplerState bilinearClamp : register(s0);
#ifdef __spirv__
[[vk::binding(5, 0)]]
#endif
SamplerState pointClamp    : register(s1);

struct PSInput { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

// ---------------------------------------------------------------------------
// YCoCg colour space — better neighbourhood clamping than RGB
// ---------------------------------------------------------------------------
float3 RGBToYCoCg(float3 c)
{
    return float3(
         0.25 * c.r + 0.5 * c.g + 0.25 * c.b,
        -0.25 * c.r + 0.5 * c.g - 0.25 * c.b + 0.5,
         0.50 * c.r              - 0.50 * c.b + 0.5);
}

float3 YCoCgToRGB(float3 c)
{
    // c.x = Y, c.y = Cg + 0.5, c.z = Co + 0.5
    float Co = c.z - 0.5;
    float Cg = c.y - 0.5;
    return saturate(float3(
        c.x + Co - Cg,   // R = Y + Co - Cg
        c.x + Cg,        // G = Y + Cg
        c.x - Co - Cg    // B = Y - Co - Cg
    ));
}

// ---------------------------------------------------------------------------
// Pixel shader
// ---------------------------------------------------------------------------
float4 main(PSInput input) : SV_Target
{
    float2 uv = input.uv;

    float depth = depthTex.Sample(pointClamp, uv);
    float3 color = currentFrame.Sample(bilinearClamp, uv).rgb;

    // Background: no temporal blend
    if (depth >= 1.0)
        return float4(color, 1.0);

    // --- Reconstruct world position from depth (uses jittered invVP) ---
    float4 ndc;
    ndc.x =  uv.x * 2.0 - 1.0;
    ndc.y = (1.0 - uv.y) * 2.0 - 1.0;
    ndc.z = depth;
    ndc.w = 1.0;
    float4 worldPos = mul(ndc, invViewProj);
    worldPos.xyz /= worldPos.w;

    // --- Reproject to previous frame UV (unjittered prevVP) ---
    float4 prevClip = mul(float4(worldPos.xyz, 1.0), prevViewProj);
    float2 prevNDC  = prevClip.xy / prevClip.w;
    float2 prevUV   = float2(prevNDC.x * 0.5 + 0.5, 0.5 - prevNDC.y * 0.5);

    // Discard out-of-bounds history
    if (any(prevUV < 0.001) || any(prevUV > 0.999))
        return float4(color, 1.0);

    float3 history = historyFrame.Sample(bilinearClamp, prevUV).rgb;

    // --- Variance clipping in YCoCg (Salvi 2016) ---
    // Softer than min/max AABB: uses mean ± gamma*stddev to allow TAA to
    // smooth sub-pixel features (UV seam artifacts) while still rejecting ghosts.
    float2 texelSize;
    {
        float w, h;
        currentFrame.GetDimensions(w, h);
        texelSize = float2(1.0 / w, 1.0 / h);
    }

    float3 m1 = 0.0, m2 = 0.0;  // first and second moments
    [unroll]
    for (int x = -1; x <= 1; x++)
    {
        [unroll]
        for (int y = -1; y <= 1; y++)
        {
            float3 s = RGBToYCoCg(
                currentFrame.Sample(pointClamp, uv + float2(x, y) * texelSize).rgb);
            m1 += s;
            m2 += s * s;
        }
    }
    m1 /= 9.0;
    m2 /= 9.0;
    float3 stddev = sqrt(max(m2 - m1 * m1, 0.0));
    float  gamma  = 1.25;  // 1.0 = tight (less ghosting), 2.0 = loose (more smoothing)
    float3 clipMin = m1 - gamma * stddev;
    float3 clipMax = m1 + gamma * stddev;
    history = YCoCgToRGB(clamp(RGBToYCoCg(history), clipMin, clipMax));

    return float4(lerp(history, color, alpha), 1.0);
}

