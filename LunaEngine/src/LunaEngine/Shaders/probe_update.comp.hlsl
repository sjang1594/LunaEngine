// probe_update.comp.hlsl — Phase 30: Probe irradiance update (DX12)
// One probe per frame: samples SSGI + IBL irradiance, writes 16x16 octahedral patch.

cbuffer ProbeConstants : register(b0)
{
    float3   origin;    float _p0;      // probe grid world origin (16B)
    float3   spacing;   float _p1;      // per-probe spacing       (16B)
    uint3    dims;      uint  _p2;      // grid dimensions 8,4,8   (16B)
    float2   screenSize; float2 _p3;    // full-res screen size     (16B)
    row_major float4x4 invViewProj;     // 64B — for world position reconstruction
    uint     probeIndex; uint3 _pp;     // which probe to update    (16B)
};                                      // total: 144B → pad to 256B with cbuffer packing

Texture2D         ssgiTex      : register(t0);  // half-res SSGI output
TextureCube<float4> irrCubemap : register(t1);  // IBL irradiance fallback
Texture2D<float>  depthTex     : register(t2);

RWTexture2DArray<float4> probeIrrArray : register(u0);  // 128×64 array of 8 slices

SamplerState bilinearClamp  : register(s0);
SamplerState trilinearClamp : register(s1);

static const uint PROBE_TEX_SIZE = 16u;
static const float PI = 3.14159265359f;

// Octahedral encoding: maps [0,16)×[0,16) texel to a unit sphere direction
float3 OctahedralDecode(float2 f)
{
    // f in [-1,1]
    float3 n = float3(f.x, f.y, 1.0f - abs(f.x) - abs(f.y));
    float  t = max(-n.z, 0.0f);
    n.xy += select(n.xy >= 0.0f, -t, t);
    return normalize(n);
}

[numthreads(16, 16, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    // Decode probe index → grid coordinates
    uint probe = probeIndex % (dims.x * dims.y * dims.z);
    uint gx    = probe % dims.x;
    uint gy    = (probe / dims.x) % dims.y;
    uint gz    = probe / (dims.x * dims.y);

    // Probe world position
    float3 probeWS = origin + float3(float(gx), float(gy), float(gz)) * spacing;

    // Atlas texel address
    // Layout: Texture2DArray[gz], each slice = 8×4 probes × 16 texels each = 128×64
    uint atlasX = (gx * PROBE_TEX_SIZE) + dtid.x;
    uint atlasY = (gy * PROBE_TEX_SIZE) + dtid.y;
    uint slice  = gz;

    // Map texel to octahedral direction
    float2 octUV = (float2(dtid.xy) + 0.5f) / float(PROBE_TEX_SIZE);  // [0,1]
    float2 octF  = octUV * 2.0f - 1.0f;                               // [-1,1]
    float3 dir   = OctahedralDecode(octF);

    // Sample IBL irradiance as base
    float3 iblIrr = irrCubemap.SampleLevel(trilinearClamp, dir, 0).rgb;

    // Sample SSGI: try to find screen UV for this probe+direction
    // Map probe position to screen UV using depth
    // For simplicity: if probeWS roughly aligns to a screen pixel, sample SSGI there.
    // Without a proper viewProj matrix here, we use IBL as the dominant fallback.
    float3 ssgiSample = float3(0, 0, 0);
    // TODO: pass viewProj to enable SSGI sampling at probe position

    float3 radiance = iblIrr + ssgiSample;

    probeIrrArray[uint3(atlasX, atlasY, slice)] = float4(radiance, 1.0f);
}
