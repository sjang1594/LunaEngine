// Phase 32: Visibility buffer — material evaluation compute shader (DX12 SM 6.0)
// For each pixel: read (objectIdx, primID) from vis buffer → reconstruct barycentrics
// from screen position → perspective-correct interpolate UV/normal/tangent → sample
// material textures → write G-buffer outputs.
//
// G-buffer layout (UAV write, matches deferred_lighting expectations):
//   u0 (GB0): R8G8B8A8_UNORM — albedo.rgb | emissive.a
//   u1 (GB1): R16G16B16A16_FLOAT — world normal.xyz | unused.w
//   u2 (GB2): R8G8B8A8_UNORM — metallic.r | roughness.g | nirReflectivity.b | unused.a

// ── Structs (must match Mesh.h) ──────────────────────────────────────────────
struct PBRVertex
{
    float3 position;  // offset  0
    float3 normal;    // offset 12
    float2 uv;        // offset 24
    float4 tangent;   // offset 32  (xyz=tangent, w=bitangent sign)
};                    // stride 48 B

struct GPUObjectData
{
    row_major float4x4 model;
    float4             boundingSphere;
    uint               meshIndex;
    uint               materialIndex;
    uint2              _matCBAddr;
};

struct MeshDrawInfo
{
    uint indexCount;
    uint firstIndex;   // offset into merged IB (in indices, not bytes)
    int  vertexOffset; // base vertex offset into merged VB
    uint _pad;
};

// ── Constants ────────────────────────────────────────────────────────────────
cbuffer VisShadeConstants : register(b0)
{
    row_major float4x4 gView;
    row_major float4x4 gProj;
    row_major float4x4 gViewProj;
    uint2              gScreenSize;   // width, height
    uint               gNumObjects;
    uint               _pad;
};

// ── Inputs ───────────────────────────────────────────────────────────────────
Texture2D<uint>                   gVisBuf     : register(t0);  // packed (objIdx<<23|primID)
StructuredBuffer<PBRVertex>       gVB         : register(t1);  // merged vertex buffer
ByteAddressBuffer                 gIB         : register(t2);  // merged index buffer (uint32)
StructuredBuffer<GPUObjectData>   gObjects    : register(t3);
StructuredBuffer<MeshDrawInfo>    gMeshInfos  : register(t4);
Texture2D<float4> gAllTextures[]              : register(t0, space1); // bindless

// ── Outputs (G-buffer as RWTexture2D) ────────────────────────────────────────
RWTexture2D<float4> gGB0 : register(u0);  // albedo + emissive.a
RWTexture2D<float4> gGB1 : register(u1);  // world normal
RWTexture2D<float4> gGB2 : register(u2);  // metallic / roughness / nirRefl

// ── Samplers ─────────────────────────────────────────────────────────────────
SamplerState gAnisoSampler : register(s0);

// ── Helpers ──────────────────────────────────────────────────────────────────
uint LoadIndex(uint byteOffset)
{
    // ByteAddressBuffer uses byte offsets; uint32 indices → multiply by 4
    return gIB.Load(byteOffset * 4u);
}

// Perspective-correct barycentric reconstruction from screen position.
// Returns (b0, b1, b2) for triangle (v0, v1, v2) at pixel (px, py).
float3 ComputeBarycentrics(
    float3 p0WS, float3 p1WS, float3 p2WS,
    row_major float4x4 model,
    uint px, uint py)
{
    // Transform to clip space
    float4 c0 = mul(mul(float4(p0WS, 1.0f), model), gViewProj);
    float4 c1 = mul(mul(float4(p1WS, 1.0f), model), gViewProj);
    float4 c2 = mul(mul(float4(p2WS, 1.0f), model), gViewProj);

    // NDC [-1,1] (perspective divide)
    float2 n0 = c0.xy / c0.w;
    float2 n1 = c1.xy / c1.w;
    float2 n2 = c2.xy / c2.w;

    // Pixel centre → NDC (DX12: origin top-left, +X right, +Y down; NDC +Y up → flip Y)
    float2 pixNDC;
    pixNDC.x =  ((float(px) + 0.5f) / float(gScreenSize.x)) * 2.0f - 1.0f;
    pixNDC.y = -((float(py) + 0.5f) / float(gScreenSize.y)) * 2.0f + 1.0f;

    // 2D barycentric in NDC
    float2 e01 = n1 - n0, e02 = n2 - n0, ep = pixNDC - n0;
    float denom = e01.x * e02.y - e01.y * e02.x;
    float lam1  = (ep.x * e02.y - ep.y * e02.x) / denom;
    float lam2  = (e01.x * ep.y - e01.y * ep.x) / denom;
    float lam0  = 1.0f - lam1 - lam2;

    // Perspective-correct: weight by 1/w_clip
    float w0 = 1.0f / c0.w, w1 = 1.0f / c1.w, w2 = 1.0f / c2.w;
    float wSum = lam0 * w0 + lam1 * w1 + lam2 * w2;
    return float3(lam0 * w0, lam1 * w1, lam2 * w2) / wSum;
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint px = tid.x, py = tid.y;
    if (px >= gScreenSize.x || py >= gScreenSize.y) return;

    uint packed = gVisBuf[uint2(px, py)];

    // Sky / background pixel — cleared to 0; objectIdx stored as (objectIdx+1) so 0 = no hit
    if (packed == 0u)
    {
        gGB0[uint2(px,py)] = float4(0,0,0,0);
        gGB1[uint2(px,py)] = float4(0,0,0,0);
        gGB2[uint2(px,py)] = float4(0,0,0,0);
        return;
    }

    uint objIdx = (packed >> 23u) - 1u;  // -1 to recover original objectIdx (stored as objectIdx+1)
    uint primID = packed & 0x7FFFFFu;

    GPUObjectData obj  = gObjects[objIdx];
    MeshDrawInfo  info = gMeshInfos[obj.meshIndex];

    // Absolute triangle → fetch 3 indices from merged IB
    uint triBase  = info.firstIndex + primID * 3u;
    uint i0 = LoadIndex(triBase + 0u) + (uint)info.vertexOffset;
    uint i1 = LoadIndex(triBase + 1u) + (uint)info.vertexOffset;
    uint i2 = LoadIndex(triBase + 2u) + (uint)info.vertexOffset;

    PBRVertex v0 = gVB[i0];
    PBRVertex v1 = gVB[i1];
    PBRVertex v2 = gVB[i2];

    // Perspective-correct barycentrics
    float3 bary = ComputeBarycentrics(v0.position, v1.position, v2.position, obj.model, px, py);
    float b0 = bary.x, b1 = bary.y, b2 = bary.z;

    // Interpolate attributes
    float2 uv      = b0 * v0.uv      + b1 * v1.uv      + b2 * v2.uv;
    float3 normalOS = normalize(b0 * v0.normal  + b1 * v1.normal  + b2 * v2.normal);
    float4 tanOS   = float4(normalize(b0 * v0.tangent.xyz + b1 * v1.tangent.xyz + b2 * v2.tangent.xyz),
                            v0.tangent.w);

    // Transform normal to world space (no non-uniform scale assumed)
    float3x3 modelRS = (float3x3)obj.model;
    float3 normalWS  = normalize(mul(normalOS, modelRS));
    float3 tangentWS = normalize(mul(tanOS.xyz, modelRS));
    float3 bitangWS  = cross(normalWS, tangentWS) * tanOS.w;

    // Bindless texture fetch — materialIndex = base SRV slot
    uint matBase  = obj.materialIndex;
    float4 albedo4     = gAllTextures[matBase + 0].SampleLevel(gAnisoSampler, uv, 0);
    float3 normalMap   = gAllTextures[matBase + 1].SampleLevel(gAnisoSampler, uv, 0).xyz;
    float2 metalRough  = gAllTextures[matBase + 2].SampleLevel(gAnisoSampler, uv, 0).bg;
    float  emissiveA   = gAllTextures[matBase + 3].SampleLevel(gAnisoSampler, uv, 0).r;

    // Normal map decode (tangent space → world space)
    float3 nTS    = normalMap * 2.0f - 1.0f;
    float3 nWS    = normalize(nTS.x * tangentWS + nTS.y * bitangWS + nTS.z * normalWS);

    // Write G-buffer
    gGB0[uint2(px,py)] = float4(albedo4.rgb, emissiveA);
    gGB1[uint2(px,py)] = float4(nWS * 0.5f + 0.5f, 0.0f);
    gGB2[uint2(px,py)] = float4(metalRough.x, metalRough.y,
                                 albedo4.a,    // near-IR proxy (albedo luma if no dedicated channel)
                                 0.0f);
}
