// vol_inject.comp.hlsl — Phase 29: Volumetric Fog — Material Injection (DX12)
// Fills the 3D froxel volume with fog density and scattering coefficients.
// Each froxel stores: RGB = scattering coefficient, A = extinction coefficient.
//
// RootSig (VolInject):
//   b0 — VolumetricParams CBV
//   u0 — RWTexture3D<float4> froxelVolume (inject write)
//
// Dispatch: (ceil(FROXEL_X/8), ceil(FROXEL_Y/8), ceil(FROXEL_Z/4))

static const uint FROXEL_X = 160u;
static const uint FROXEL_Y = 90u;
static const uint FROXEL_Z = 64u;

cbuffer VolumetricParams : register(b0)
{
    row_major float4x4 invProj;          // 64B: NDC → view
    row_major float4x4 invView;          // 64B: view → world
    row_major float4x4 lightVP[4];       // 256B: CSM light ViewProj per cascade
    float4 cascadeSplits;                // 16B: view-space Z splits
    float3 lightDir;      float _p0;     // 16B: world-space sun direction
    float3 lightColor;    float lightIntensity; // 16B
    float nearZ;          float farZ;
    float screenW;        float screenH; // 16B
    float fogDensity;     float fogHeightFalloff;
    float fogBaseHeight;  float scatteringCoeff; // 16B
    float extinctionCoeff; float phaseG;
    float _pad1[2];                      // 8B pad → total 512B
};

RWTexture3D<float4> froxelVolume : register(u0);

// Reconstruct world-space position of the center of a given froxel
float3 FroxelCenterWorld(uint3 id)
{
    // Froxel UV in [0,1]
    float2 uv = (float2(id.xy) + 0.5) / float2(FROXEL_X, FROXEL_Y);

    // Exponential depth: z_slice_center is in view space (negative Z in DX convention)
    float tNear = nearZ * pow(farZ / nearZ, float(id.z)       / float(FROXEL_Z));
    float tFar  = nearZ * pow(farZ / nearZ, float(id.z + 1u)  / float(FROXEL_Z));
    float viewZ = -(tNear + tFar) * 0.5; // negative Z in view space

    // Reconstruct view-space position
    float4 ndc  = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.5, 1.0);
    float4 vs   = mul(ndc, invProj);
    vs /= vs.w;
    float3 vsPos = float3(vs.xy * ((-viewZ) / (-vs.z)), viewZ);

    // Transform to world space
    float4 world = mul(float4(vsPos, 1.0), invView);
    return world.xyz;
}

// Height-based exponential fog density
float HeightFogDensity(float3 worldPos)
{
    float h = max(0.0, worldPos.y - fogBaseHeight);
    return fogDensity * exp(-h * fogHeightFalloff);
}

[numthreads(8, 8, 4)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= FROXEL_X || id.y >= FROXEL_Y || id.z >= FROXEL_Z)
        return;

    float3 worldPos = FroxelCenterWorld(id);
    float density   = HeightFogDensity(worldPos);

    float scatter = density * scatteringCoeff;
    float extinct = density * extinctionCoeff;

    froxelVolume[id] = float4(scatter, scatter, scatter, extinct);
}
