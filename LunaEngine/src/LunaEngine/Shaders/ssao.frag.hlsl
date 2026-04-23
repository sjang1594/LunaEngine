// ssao.frag.hlsl — Phase 9: Screen-Space Ambient Occlusion
// Half-res pass: reconstructs view-space positions + normals, samples a 16-tap
// hemisphere kernel, writes raw occlusion to an R8_UNORM render target.
//
// Root signature (RootSignatureLayout::SSAO):
//   params[0] b0 — SSAOConstants CBV
//   params[1]    — SRV table t0: hardware depth (R32_FLOAT)
//   params[2]    — SRV table t1: world-space normal GB1 (R16G16B16A16_FLOAT, encoded [0,1])
//   params[3]    — SRV table t2: 4×4 noise texture (R8G8_UNORM, tiled random rotations)
//   s0           — point-clamp (for depth and normal sampling)
//   s1           — point-wrap  (for tiled noise)

static const int SAMPLE_COUNT = 16;

cbuffer SSAOConstants : register(b0)
{
    float4             samples[SAMPLE_COUNT];  // view-space hemisphere kernel (z >= 0)
    row_major float4x4 projection;             // camera projection matrix
    row_major float4x4 invProjection;          // inverse camera projection
    row_major float4x4 view;                   // camera view (world → view space)
    float2             noiseScale;             // halfScreenRes / noiseTexSize → tiling UV
    float              radius;                 // hemisphere sample radius (view-space units)
    float              bias;                   // depth comparison bias (prevents self-shadowing)
};

Texture2D<float>  depthTex  : register(t0);  // hardware depth [0, 1]
Texture2D         normalTex : register(t1);  // world-space normal encoded as [0,1] in GB1
Texture2D         noiseTex  : register(t2);  // 4×4 random rotation vectors (RG8_UNORM)

SamplerState pointClamp : register(s0);
SamplerState pointWrap  : register(s1);

struct PSInput
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

// ---------------------------------------------------------------------------
// Reconstruct view-space position from clip-space UV and hardware depth.
// DirectX LH convention: projection produces Z in [0,1] and Z increases into
// the scene (positive Z = further from camera).
// ---------------------------------------------------------------------------
float3 ReconstructVSPos(float2 uv, float depth)
{
    // UV(0,0)=top-left, UV(1,1)=bottom-right in texture space.
    // NDC: X in [-1,+1] left→right, Y in [-1,+1] bottom→top.
    float2 ndc  = float2(uv.x * 2.0 - 1.0, (1.0 - uv.y) * 2.0 - 1.0);
    float4 clip = float4(ndc, depth, 1.0);
    float4 vsP  = mul(clip, invProjection);
    return vsP.xyz / vsP.w;
}

// ---------------------------------------------------------------------------
// SSAO pixel shader
// ---------------------------------------------------------------------------
float main(PSInput input) : SV_Target
{
    float depth = depthTex.Sample(pointClamp, input.uv);
    if (depth >= 1.0) return 1.0;  // background pixel — no occlusion

    // ---- Reconstruct view-space position ----
    float3 posVS = ReconstructVSPos(input.uv, depth);

    // ---- World-space normal → view space ----
    float3 normalWS = normalize(normalTex.Sample(pointClamp, input.uv).rgb * 2.0 - 1.0);
    float3 normalVS = normalize(mul(float4(normalWS, 0.0), view).xyz);

    // ---- Random rotation from tiled 4×4 noise ----
    float2 noiseVec  = noiseTex.Sample(pointWrap, input.uv * noiseScale).rg * 2.0 - 1.0;
    float3 randomVec = normalize(float3(noiseVec, 0.0));

    // ---- Build TBN from random vector + surface normal (Gram-Schmidt) ----
    float3 tangent   = normalize(randomVec - normalVS * dot(randomVec, normalVS));
    float3 bitangent = cross(normalVS, tangent);
    float3x3 TBN     = float3x3(tangent, bitangent, normalVS);

    // ---- Accumulate occlusion from hemisphere samples ----
    float occlusion = 0.0;
    [unroll]
    for (int i = 0; i < SAMPLE_COUNT; i++)
    {
        // Rotate kernel sample into surface TBN frame, offset by radius
        float3 sampleDir = mul(samples[i].xyz, TBN);
        float3 samplePos = posVS + sampleDir * radius;

        // Project sample to clip space → screen UV
        float4 projPos  = mul(float4(samplePos, 1.0), projection);
        projPos.xyz    /= projPos.w;
        float2 sampleUV = saturate(float2(projPos.x * 0.5 + 0.5,
                                          0.5 - projPos.y * 0.5));

        // Depth at projected sample position → view-space Z
        float storedDepth = depthTex.Sample(pointClamp, sampleUV);
        float3 storedVS   = ReconstructVSPos(sampleUV, storedDepth);

        // Range check: weight down contribution from geometrically distant surfaces.
        // Prevents thin objects from casting occlusion far behind them.
        float rangeCheck = smoothstep(0.0, 1.0,
                            radius / max(abs(posVS.z - storedVS.z), 0.0001));

        // Occlusion condition (LH view space, Z+ into scene):
        //   stored surface closer to camera (smaller Z) than sample → sample is
        //   "inside" geometry → occluded.
        //   i.e. samplePos.z >= storedVS.z + bias
        occlusion += (samplePos.z >= storedVS.z + bias) ? rangeCheck : 0.0;
    }

    // Invert: 0 = fully occluded, 1 = unoccluded
    return 1.0 - occlusion / float(SAMPLE_COUNT);
}

