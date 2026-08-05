// ssgi.comp.hlsl — Phase 30: Screen-Space GI compute (DX12)
// Half-resolution, 8 cosine-weighted rays, Hi-Z march, temporal accumulation.

cbuffer SSGIConstants : register(b0)
{
    row_major float4x4 invViewProj;    // 64B
    row_major float4x4 prevViewProj;   // 64B
    row_major float4x4 view;           // 64B
    float2 screenSize;                 // 8B
    float2 halfResSize;                // 8B
    uint   frameCount;                 // 4B
    uint   numRays;                    // 4B
    float  maxRayDist;                 // 4B
    float  temporalAlpha;              // 4B
    float4 _pad;                       // 16B → 256B
};

Texture2D<float>    depthTex    : register(t0);
Texture2D           gbuffer0    : register(t1);  // albedo RGBA8
Texture2D           gbuffer1    : register(t2);  // normal RGBA16F
Texture2D           hdrTex      : register(t3);  // previous-frame HDR
Texture2D<float>    hiZTex      : register(t4);  // Hi-Z full pyramid
Texture2D           ssgiHistory : register(t5);  // history RGBA16F half-res
RWTexture2D<float4> ssgiOutput  : register(u0);

SamplerState pointClamp  : register(s0);
SamplerState linearClamp : register(s1);

static const float PI = 3.14159265359f;

float RadicalInverse_VdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10f;
}

float2 Hammersley(uint i, uint N)
{
    return float2(float(i) / float(N), RadicalInverse_VdC(i));
}

float3 CosineSampleHemisphere(float2 xi)
{
    float cosTheta = sqrt(max(1.0f - xi.x, 0.0f));
    float sinTheta = sqrt(xi.x);
    float phi = 2.0f * PI * xi.y;
    return float3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
}

float3x3 BuildTBN(float3 N)
{
    float3 up = (abs(N.y) < 0.999f) ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 T  = normalize(cross(up, N));
    float3 B  = cross(N, T);
    return float3x3(T, B, N);
}

float3 ReconstructWorldPos(float2 uv, float depth)
{
    float4 ndc = float4(uv.x * 2.0f - 1.0f, (1.0f - uv.y) * 2.0f - 1.0f, depth, 1.0f);
    float4 wp  = mul(ndc, invViewProj);
    return wp.xyz / wp.w;
}

float2 ReprojectUV(float3 posWS)
{
    float4 prevClip = mul(float4(posWS, 1.0f), prevViewProj);
    prevClip.xyz /= prevClip.w;
    float2 uv;
    uv.x = prevClip.x * 0.5f + 0.5f;
    uv.y = 1.0f - (prevClip.y * 0.5f + 0.5f);
    return uv;
}

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    uint2  halfPx  = dtid.xy;
    float2 halfSz  = halfResSize;
    if ((float)halfPx.x >= halfSz.x || (float)halfPx.y >= halfSz.y) return;

    float2 uv = (float2(halfPx) + 0.5f) / halfSz;

    float depth = depthTex.SampleLevel(pointClamp, uv, 0);
    if (depth >= 1.0f)
    {
        ssgiOutput[halfPx] = float4(0, 0, 0, 1);
        return;
    }

    float3 albedo    = gbuffer0.SampleLevel(pointClamp, uv, 0).rgb;
    float3 normalEnc = gbuffer1.SampleLevel(pointClamp, uv, 0).rgb;
    float3 N         = normalize(normalEnc * 2.0f - 1.0f);
    float3 posWS     = ReconstructWorldPos(uv, depth);

    float3x3 TBN = BuildTBN(N);

    // Step size in UV space — scale with maxRayDist
    float stepScale = maxRayDist / max(halfSz.x, halfSz.y);
    float2 uvPerStep = float2(stepScale, stepScale) * 0.12f;
    float  depStep   = 0.003f;

    float3 gi  = float3(0, 0, 0);
    uint Nrays = clamp(numRays, 1u, 16u);

    for (uint r = 0u; r < Nrays; ++r)
    {
        // Frame-seeded low-discrepancy sample
        uint  sid = (frameCount * 17u + r * 7u + halfPx.x * 3u + halfPx.y * 11u) & 63u;
        float2 xi = Hammersley(sid, 64u);
        xi.x = frac(xi.x + float(r) / float(Nrays));  // rotate for stratification

        float3 rayTS  = CosineSampleHemisphere(xi);
        float3 rayDir = mul(rayTS, TBN);

        float NdotR = dot(N, rayDir);
        if (NdotR <= 0.0f) { rayDir = -rayDir; NdotR = -NdotR; }

        // Project ray endpoint to get UV step direction
        float3 endWS = posWS + rayDir * stepScale * 16.0f;
        // Use view matrix to get view-space direction for UV-space projection
        float4 vsStart = mul(float4(posWS, 1.0f), view);
        float4 vsEnd   = mul(float4(endWS,  1.0f), view);

        // Simple screen-space direction from view-space positions
        float2 screenStart = (vsStart.xy / max(vsStart.z, 0.001f)) * float2(0.5f, -0.5f) + 0.5f;
        float2 screenEnd   = (vsEnd.xy   / max(vsEnd.z,   0.001f)) * float2(0.5f, -0.5f) + 0.5f;
        float2 screenDir   = screenEnd - screenStart;
        float  screenLen   = length(screenDir);
        if (screenLen < 1e-5f) continue;
        screenDir /= screenLen;

        float2 stepUV = screenDir * uvPerStep;
        float  stepD  = depStep * NdotR;

        // Hi-Z march
        float2 marchUV  = uv + stepUV;
        float  marchDep = depth + stepD;
        bool   hit      = false;

        [unroll]
        for (int step = 0; step < 16; ++step)
        {
            if (any(marchUV < 0.0f) || any(marchUV > 1.0f)) break;
            float hiZSample = hiZTex.SampleLevel(pointClamp, marchUV, 0);
            if (marchDep > hiZSample + 1e-4f && marchDep < hiZSample + 0.05f)
            {
                hit = true;
                break;
            }
            marchUV  += stepUV;
            marchDep += stepD;
        }

        if (hit)
        {
            float3 hitAlbedo   = gbuffer0.SampleLevel(linearClamp, marchUV, 0).rgb;
            hitAlbedo          = pow(max(hitAlbedo, 1e-4f), 2.2f);  // sRGB → linear
            float3 hitRadiance = hdrTex.SampleLevel(linearClamp, marchUV, 0).rgb;
            gi += hitAlbedo * hitRadiance * NdotR;
        }
    }
    gi /= float(Nrays);

    // Temporal reprojection
    float3 posWSLocal = posWS;  // already computed above
    float2 histUV = ReprojectUV(posWSLocal);
    bool   valid  = all(histUV >= 0.0f) && all(histUV <= 1.0f);
    float3 history = valid ? ssgiHistory.SampleLevel(linearClamp, histUV, 0).rgb : gi;

    float  alpha   = valid ? temporalAlpha : 1.0f;
    float3 blended = lerp(history, gi, alpha);

    ssgiOutput[halfPx] = float4(blended, 1.0f);
}
