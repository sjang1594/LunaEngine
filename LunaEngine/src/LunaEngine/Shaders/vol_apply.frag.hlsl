// vol_apply.frag.hlsl — Phase 29: Volumetric Fog — Apply Pass (DX12)
// Fullscreen pass: samples froxel accumulation volume at each pixel's depth.
// Outputs in-scattering (additive blend onto HDR RT).
//
// RootSig (VolApply):
//   b0     — VolumetricApplyParams (4 root constants: nearZ, farZ, pad×2)
//   t0     — Texture2D<float>  depthTex  (scene depth)
//   t1     — Texture3D<float4> froxelAccum (accumulated in-scattering + transmittance)
//   s0     — point-clamp
//   s1     — bilinear-clamp (3D)
//
// Pipeline: additive blend (SRC=ONE, DST=ONE), no depth test, no input layout.

static const uint FROXEL_Z = 64u;

cbuffer VolumetricApplyParams : register(b0)
{
    float nearZ;
    float farZ;
    float _pad0;
    float _pad1;
};

Texture2D<float>    depthTex    : register(t0);
Texture3D<float4>   froxelAccum : register(t1);

SamplerState pointClamp   : register(s0);
SamplerState bilinearClamp : register(s1);

struct PSInput
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

// Reconstruct linear depth (positive, in view units) from NDC depth
float LinearDepth(float ndcZ)
{
    // DX12 depth buffer is [0,1], reverse mapping through projection
    // For standard projection: z_view = (farZ * nearZ) / (farZ - ndcZ * (farZ - nearZ))
    return (farZ * nearZ) / (farZ - ndcZ * (farZ - nearZ));
}

// Map linear view depth to froxel Z [0,1]
float FroxelZFromViewDepth(float viewDepth)
{
    float logRatio = log(farZ / nearZ);
    float slice    = log(viewDepth / nearZ) / logRatio; // [0,1]
    return clamp(slice, 0.0, 1.0);
}

float4 main(PSInput input) : SV_Target
{
    float ndcDepth  = depthTex.Sample(pointClamp, input.uv).r;

    // Sky pixels: no geometry, no volumetric sample (froxel volume is bounded by farZ)
    // Still apply full-depth volumetric (last froxel slice covers the sky).
    float viewDepth = LinearDepth(ndcDepth);

    // Map to froxel UVW
    float froxelW   = FroxelZFromViewDepth(clamp(viewDepth, nearZ, farZ));
    float3 uvw      = float3(input.uv.x, input.uv.y, froxelW);

    float4 fog = froxelAccum.SampleLevel(bilinearClamp, uvw, 0);

    // fog.rgb = cumulative in-scattering, fog.a = transmittance
    // We output only in-scattering for additive blend.
    // Transmittance darkening would require read-modify-write (UAV), skipped in v1.
    return float4(fog.rgb, 0.0);
}
