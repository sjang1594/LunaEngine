// vol_scatter.comp.hlsl — Phase 29: Volumetric Fog — Scattering Accumulation (DX12)
// Marches along each froxel column (Z axis), accumulating in-scattering and transmittance.
// Output: froxelVolume[x,y,z] = (cumulative in-scattering.rgb, cumulative transmittance)
//
// RootSig (VolScatter):
//   b0  — VolumetricParams CBV
//   t0  — Texture3D<float4> froxelInject  (density from inject pass)
//   t1  — Texture2DArray<float> csmShadow (4 cascade shadow maps)
//   u0  — RWTexture3D<float4>  froxelAccum (accumulated output)
//   s0  — bilinear-clamp sampler (for CSM PCF)
//
// Dispatch: (ceil(FROXEL_X/8), ceil(FROXEL_Y/8), 1)

static const uint FROXEL_X = 160u;
static const uint FROXEL_Y = 90u;
static const uint FROXEL_Z = 64u;
static const uint CSM_CASCADES = 4u;
static const float PI = 3.14159265;

cbuffer VolumetricParams : register(b0)
{
    row_major float4x4 invProj;
    row_major float4x4 invView;
    row_major float4x4 lightVP[4];
    float4 cascadeSplits;
    float3 lightDir;      float _p0;
    float3 lightColor;    float lightIntensity;
    float nearZ;          float farZ;
    float screenW;        float screenH;
    float fogDensity;     float fogHeightFalloff;
    float fogBaseHeight;  float scatteringCoeff;
    float extinctionCoeff; float phaseG;
    float _pad1[2];
};

Texture3D<float4>       froxelInject : register(t0);
Texture2DArray<float>   csmShadow    : register(t1);
RWTexture3D<float4>     froxelAccum  : register(u0);
SamplerComparisonState  csmSampler   : register(s0);

// Henyey-Greenstein phase function
float PhaseHG(float cosTheta, float g)
{
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(max(1.0 + g2 - 2.0 * g * cosTheta, 0.001), 1.5));
}

// Reconstruct view-space position for froxel center
float3 FroxelViewPos(uint3 id)
{
    float2 uv = (float2(id.xy) + 0.5) / float2(FROXEL_X, FROXEL_Y);
    float tNear = nearZ * pow(farZ / nearZ, float(id.z)      / float(FROXEL_Z));
    float tFar  = nearZ * pow(farZ / nearZ, float(id.z + 1u) / float(FROXEL_Z));
    float viewZ = -(tNear + tFar) * 0.5;

    float4 ndc = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.5, 1.0);
    float4 vs  = mul(ndc, invProj);
    vs /= vs.w;
    return float3(vs.xy * ((-viewZ) / (-vs.z)), viewZ);
}

float3 ViewToWorld(float3 vsPos)
{
    float4 w = mul(float4(vsPos, 1.0), invView);
    return w.xyz;
}

float FroxelSliceThickness(uint z)
{
    float tNear = nearZ * pow(farZ / nearZ, float(z)      / float(FROXEL_Z));
    float tFar  = nearZ * pow(farZ / nearZ, float(z + 1u) / float(FROXEL_Z));
    return tFar - tNear;
}

// Sample CSM shadow — returns [0,1] where 1 = fully lit
float SampleCSM(float3 worldPos, float viewZ)
{
    float absZ = -viewZ; // positive view depth

    // Select cascade
    uint cascade = 0u;
    if      (absZ < cascadeSplits.x) cascade = 0u;
    else if (absZ < cascadeSplits.y) cascade = 1u;
    else if (absZ < cascadeSplits.z) cascade = 2u;
    else                             cascade = 3u;

    float4 lightClip = mul(float4(worldPos, 1.0), lightVP[cascade]);
    lightClip.xyz /= lightClip.w;

    // DX12 NDC → UV: x [-1,1]→[0,1], y [1,-1]→[0,1]
    float2 uv = float2(lightClip.x * 0.5 + 0.5, -lightClip.y * 0.5 + 0.5);
    float  compareDepth = lightClip.z;

    if (any(uv < 0.0) || any(uv > 1.0) || compareDepth < 0.0 || compareDepth > 1.0)
        return 1.0; // outside shadow map — assume lit

    return csmShadow.SampleCmpLevelZero(csmSampler, float3(uv, float(cascade)), compareDepth - 0.001);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= FROXEL_X || id.y >= FROXEL_Y)
        return;

    // Camera-to-pixel view direction (for phase function)
    float2 uv     = (float2(id.xy) + 0.5) / float2(FROXEL_X, FROXEL_Y);
    float4 ndcDir = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.5, 1.0);
    float4 vsDir  = mul(ndcDir, invProj); vsDir /= vsDir.w;
    float3 viewRayW = normalize(mul(float4(normalize(vsDir.xyz), 0.0), invView).xyz);
    float3 sunDirN  = normalize(-lightDir); // lightDir points from surface to sun
    float  cosTheta = dot(viewRayW, sunDirN);
    float  phase    = PhaseHG(cosTheta, phaseG);

    float3 accInscatter = float3(0.0, 0.0, 0.0);
    float  accTransmit  = 1.0;

    [loop]
    for (uint z = 0u; z < FROXEL_Z; ++z)
    {
        uint3 coord = uint3(id.xy, z);
        float4 density = froxelInject[coord]; // (scatter, scatter, scatter, extinction)

        float scatter  = density.x;
        float extinct  = density.w;
        float stepSize = FroxelSliceThickness(z);

        [branch]
        if (extinct > 1e-5)
        {
            float3 vsPos  = FroxelViewPos(coord);
            float3 wsPos  = ViewToWorld(vsPos);
            float  shadow = SampleCSM(wsPos, vsPos.z);

            // In-scattered radiance for this slice
            float3 Li = (shadow * phase + 0.1) * lightColor * lightIntensity * scatter;

            // Beer-Lambert step transmittance
            float stepT = exp(-extinct * stepSize);

            // Accumulate using analytic integration of constant-density slab
            accInscatter += accTransmit * Li * (1.0 - stepT) / max(extinct, 1e-5);
            accTransmit  *= stepT;
        }

        froxelAccum[coord] = float4(accInscatter, accTransmit);
    }
}
