#pragma once

///** Interface **
namespace Luna
{

enum class VertexLayout
{
    Triangle,   // POSITION(RGB32F) + COLOR(RGBA32F)              — stride 28 B
    PBR,        // POSITION + NORMAL(RGB32F) + TEXCOORD(RG32F) + TANGENT(RGBA32F) — stride 48 B
    PointCloud, // POSITION(RGB32F) + TEXCOORD0(R32F intensity)   — stride 16 B
};

// Phase 5B+7+8+9: root signature layout
enum class RootSignatureLayout
{
    MVP,             // b0=MVP CBV (default for triangle + mesh preview pipelines)
    PBR,             // Phase 11 bindless: b0=MVP CBV, b1=MaterialConstants CBV, b2=materialIndex root const (1 DWORD), t0+space1=unbounded SRV heap table, s0=static anisotropic sampler
    DeferredLighting,// b0=SceneConstants CBV; params[1]=SRV table t0-t4 (GB0/1/2/depth/shadow); params[2]=SRV t5 SSAO; s0=point-clamp
    CSMDepth,        // Phase 8: 16 inline root constants at b0 (light-space MVP 4×4); deny PS
    SSAO,            // Phase 9: b0=SSAOConstants CBV; params[1]=SRV t0 depth; params[2]=SRV t1 normal; params[3]=SRV t2 noise; s0=point-clamp; s1=point-wrap
    SSAOBlur,        // Phase 9: params[0]=SRV t0 raw SSAO; s0=point-clamp
    // Phase 10: post-process
    TAA,             // b0=TAAConstants CBV; params[1]=SRV t0 current HDR; params[2]=SRV t1 history; params[3]=SRV t2 depth; s0=bilinear-clamp; s1=point-clamp
    BloomBright,     // params[0]=4 root consts b0 (threshold,knee,0,0); params[1]=SRV t0 HDR; s0=point-clamp
    BloomBlur,       // params[0]=4 root consts b0 (dx,dy,0,0); params[1]=SRV t0 bloom input; s0=bilinear-clamp
    ToneMap,         // params[0]=4 root consts b0 (bloomStr,exposure,0,0); params[1]=SRV t0 resolved; params[2]=SRV t1 bloom; s0=point-clamp
    // Phase 12: GPU-driven rendering
    GPUCull,         // b0=CullConstants; t0=objects SSBO; t1=meshInfo SSBO; u0=drawArgs UAV; u1=drawCount UAV
    PBRIndirect,     // b0=ViewProj CBV; b1=MaterialConstants CBV; b2=materialIndex root const; b3=objectIndex root const; t0 space0=objects SSBO; t0+ space1=unbounded SRV; s0=anisotropic
    // Phase 14: IBL Environment Mapping
    EquirectToCube,  // compute: b0=4 root consts (faceSize,pad×3); t0=equirect; u0=cubeUAV; s0=bilinear-clamp
    IrradianceConv,  // compute: b0=4 root consts (faceSize,pad×3); t0=envCube; u0=irrUAV; s0=trilinear-clamp
    PrefilterEnv,    // compute: b0=4 root consts (faceSize,mipLevel,roughness,sampleCount); t0=envCube; u0=prefilterUAV; s0=trilinear-clamp
    BrdfLut,         // compute: u0=brdfLUT RWTexture2D<float2>; no SRV/CB
    Skybox,          // vs+ps: b0=16 root consts (invViewProj row-major); t0=envCube; s0=trilinear-clamp; noInputLayout
    DeferredLightingIBL, // b0=SceneConstants CBV; params[1]=SRV t0-t4; params[2]=SRV t5 SSAO; params[3]=SRV t6 irr; params[4]=SRV t7 prefilter; params[5]=SRV t8 brdfLUT; s0=point-clamp; s1=bilinear-clamp; s2=trilinear-clamp
    // Phase 16B: SSR
    SSRCompute,          // compute: b0=SSRConstants CBV; params[1-4]=SRV t0-t3 (depth/normal/metalRough/hdrRT); params[5]=UAV u0; s0=point-clamp; s1=linear-clamp
    SSRBlend,            // vs+ps: params[0]=SRV t0 ssrRT; s0=point-clamp; additive blend; noInputLayout
    // Phase 18B: Motion Blur
    MotionBlur,          // vs+ps: b0=MotionBlurCB CBV; params[1]=SRV t0 hdrRT; params[2]=SRV t1 depth; s0=point-clamp; noInputLayout; RGBA16F output
    // Phase 23: Hi-Z Occlusion Culling
    HiZGenerate,         // compute: b0=4 root consts (srcW,srcH,dstW,dstH); params[1]=SRV t0 source mip; params[2]=UAV u0 dest mip; s0=point-clamp
    // Phase 24: Clustered Lighting
    ClusterAssign,       // compute: b0=ClusterParams CBV; t0=lights SSBO; u0=clusterCounts; u1=clusterIndices
    // Phase 25: Mesh Shader G-buffer fill
    MeshShaderGBuffer,   // AS+MS+PS: b0=MeshShaderConstants CBV; b1=MaterialConstants CBV; b2=materialIndex root const; t0-t5 space0=object/meshlet/vertex buffers; t0+ space1=bindless SRV; s0=anisotropic
    // Phase 29: Volumetric Fog
    VolInject,           // compute: b0=VolumetricParams CBV; u0=RWTexture3D froxelVolume
    VolScatter,          // compute: b0=VolumetricParams CBV; t0=Texture3D froxelInject; t1=Texture2DArray csmShadow; u0=RWTexture3D froxelAccum; s0=comp-clamp
    VolApply,            // vs+ps: b0=4 root consts (nearZ,farZ,pad×2); t0=depthTex; t1=Texture3D froxelAccum; s0=point-clamp; s1=bilinear-clamp; additive blend; noInputLayout
    // Phase 30: Global Illumination
    SSGICompute,        // compute: b0=SSGIConstants; t0-t5 SRVs; u0=ssgiOut; s0-s1
    ProbeUpdate,        // compute: b0=ProbeConstants; t0-t2 SRVs; u0=probeIrrArray; s0-s1
    DeferredLightingGI, // vs+ps: DeferredLightingIBL params[0..9] + params[10]=ssgiTex + params[11]=probeIrrArray
    // Phase 31: Order-Independent Transparency (WBOIT)
    OITForward,         // vs+ps: b0=SceneConstants CBV; b1=alpha root const; t0=albedoTex; s0=linear-clamp; MRT 2 (RGBA16F+R8); depth read-only
    OITComposite,       // vs+ps (fullscreen): t0=accumTex; t1=revealageTex; s0=point-clamp; blend ONE_MINUS_SRC_ALPHA+SRC_ALPHA
    // Phase 32: Visibility Buffer
    VisibilityBuffer,   // vs+ps: b0=ViewProj CBV; b1=materialCB CBV; b2=materialIndex const; b3=objectIndex const; t0 space0=GPUObjectData; R32_UINT RT; depth-write; PBR vertex layout
    VisibilityShade,    // compute: b0=VisShadeConstants CBV; t0=visBuf; t1=mergedVB; t2=mergedIB; t3=objects; t4=meshInfos; u0-u2=G-buffer UAVs; t0+ space1=bindless SRVs; s0=anisotropic
    // S2: Camera Sensor
    SensorLighting,     // vs+ps (fullscreen): b0=SceneConstants; t0-t2=GB0/1/2; t3=depth; t4=irrMap; t5=prefilterMap; t6=brdfLUT; s0=point-clamp; s1=bilinear-clamp; s2=trilinear-clamp
    CameraDistort,      // compute: b0=DistortConstants; t0=litRT; u0=distortRT; s0=bilinear-clamp
    // S3: LiDAR Sensor
    LiDARRaycast,       // compute: b0=LiDARSensorCB; t0=TLAS; t1=rayDirs; t2=GPUObjectData; t3=mergedVB; t4=mergedIB; u0=output — all root descriptors (GPU VAs)
    PointCloud,         // vs+ps: b0=16 root consts (row-major VP 4x4); POSITION+TEXCOORD0 VB; POINT topology; depth-test, no depth-write
};

struct PipelineStateDesc
{
    bool                enableDepthTest  = false;
    bool                enableWireFrame  = false;
    VertexLayout        vertexLayout     = VertexLayout::Triangle;
    RootSignatureLayout rootLayout       = RootSignatureLayout::MVP;
    UINT                numRenderTargets = 1;
    DXGI_FORMAT         rtvFormats[8]   = { DXGI_FORMAT_R8G8B8A8_UNORM };
    bool                noInputLayout   = false;  // true → fullscreen pass (no vertex buffer)
    bool                depthOnlyPass   = false;  // Phase 8: NumRenderTargets=0, no PS bytecode
    DXGI_FORMAT         dsvFormat       = DXGI_FORMAT_UNKNOWN; // Phase 8: explicit DSV format override
    bool                computeShader   = false;  // Phase 12: compute pipeline (only CS, no VS/PS)
    const wchar_t*      csTarget        = L"cs_6_0"; // compute shader target (override to L"cs_6_5" for RayQuery)
    bool                meshShaderPipeline = false; // Phase 25: mesh shader pipeline (AS+MS+PS, no VS/IA)
    bool                pointTopology      = false; // S3: use POINT_LIST topology (LiDAR point cloud)
};

class IPipeline
{
public:
    virtual ~IPipeline() = default;
};
} // namespace Luna