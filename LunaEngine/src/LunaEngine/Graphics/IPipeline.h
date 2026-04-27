#pragma once

///** Interface **
namespace Luna
{

enum class VertexLayout
{
    Triangle,  // POSITION(RGB32F) + COLOR(RGBA32F)              — stride 28 B
    PBR,       // POSITION + NORMAL(RGB32F) + TEXCOORD(RG32F) + TANGENT(RGBA32F) — stride 48 B
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
    bool                meshShaderPipeline = false; // Phase 25: mesh shader pipeline (AS+MS+PS, no VS/IA)
};

class IPipeline
{
public:
    virtual ~IPipeline() = default;
};
} // namespace Luna