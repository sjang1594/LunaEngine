// hiz_generate.comp.hlsl — Hi-Z depth pyramid mip generation (SM 6.0)
// Phase 23: Reads source mip (SRV t0), writes destination mip (UAV u0).
// Each dispatch generates one mip level: dst[xy] = min(src[2x,2y], src[2x+1,2y], src[2x,2y+1], src[2x+1,2y+1])
//
// Root signature (HiZGenerate layout):
//   b0 — 4 inline root constants: srcWidth, srcHeight, dstWidth, dstHeight
//   t0 — Texture2D<float> source mip (SRV)
//   u0 — RWTexture2D<float> destination mip (UAV)
//   s0 — point-clamp sampler (static)

cbuffer HiZConstants : register(b0)
{
    uint srcWidth;
    uint srcHeight;
    uint dstWidth;
    uint dstHeight;
};

Texture2D<float>   gSrcMip : register(t0);
RWTexture2D<float> gDstMip : register(u0);
SamplerState       gSampler : register(s0);

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= dstWidth || dtid.y >= dstHeight)
        return;

    // Compute corresponding source texels (2×2 block)
    uint2 srcBase = dtid.xy * 2;

    // Gather the 2×2 block, clamping to source dimensions
    float d00 = gSrcMip[min(srcBase + uint2(0, 0), uint2(srcWidth - 1, srcHeight - 1))];
    float d10 = gSrcMip[min(srcBase + uint2(1, 0), uint2(srcWidth - 1, srcHeight - 1))];
    float d01 = gSrcMip[min(srcBase + uint2(0, 1), uint2(srcWidth - 1, srcHeight - 1))];
    float d11 = gSrcMip[min(srcBase + uint2(1, 1), uint2(srcWidth - 1, srcHeight - 1))];

    // Min reduction (standard depth: closer = smaller, so occluder = min)
    float result = min(min(d00, d10), min(d01, d11));

    gDstMip[dtid.xy] = result;
}

