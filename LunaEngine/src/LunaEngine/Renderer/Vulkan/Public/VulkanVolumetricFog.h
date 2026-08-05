#pragma once
// VulkanVolumetricFog.h — Phase 29: Froxel-based volumetric fog (Vulkan)

#include <vulkan/vulkan.h>
#include <DirectXMath.h>

using namespace DirectX;

namespace Luna
{

class VulkanCore;

class VulkanVolumetricFog
{
public:
    static constexpr uint32_t FROXEL_X     = 160u;
    static constexpr uint32_t FROXEL_Y     = 90u;
    static constexpr uint32_t FROXEL_Z     = 64u;
    static constexpr uint32_t MAX_FRAMES   = 3;

    struct CreateInfo
    {
        VulkanCore*  core             = nullptr;
        VkExtent2D   extent           = {};
        VkImageView  depthView        = VK_NULL_HANDLE;   // scene depth (sampled)
        VkImageView  hdrView          = VK_NULL_HANDLE;   // HDR target for apply pass
        VkImage      hdrImage         = VK_NULL_HANDLE;
        VkRenderPass hdrRenderPass    = VK_NULL_HANDLE;   // ppRenderPass (RGBA16F)
        VkImageView  csmShadowView    = VK_NULL_HANDLE;   // Texture2DArray shadow
        uint32_t     framesInFlight   = 3;
    };

    // GPU-side UBO — must match GLSL VolumetricParams (std140)
    struct VolumetricUBO                              // 512 bytes
    {
        float invProj[16];                            //  64B
        float invView[16];                            //  64B
        float lightVP[4][16];                         // 256B
        float cascadeSplits[4];                       //  16B
        float lightDir[3];    float _p0;              //  16B
        float lightColor[3];  float lightIntensity;   //  16B
        float nearZ;          float farZ;
        float screenW;        float screenH;           //  16B
        float fogDensity;     float fogHeightFalloff;
        float fogBaseHeight;  float scatteringCoeff;   //  16B
        float extinctionCoeff; float phaseG;
        float _pad1[2];                               //   8B → 512B
    };
    static_assert(sizeof(VolumetricUBO) == 480, "VolumetricUBO must be 480 bytes");

    VulkanVolumetricFog() = default;
    ~VulkanVolumetricFog();

    bool Create(const CreateInfo& info);
    void Destroy();

    // Per-frame: update UBO with camera / fog params, dispatch inject + scatter
    void Dispatch(VkCommandBuffer cmd, uint32_t frameIndex,
                  const XMFLOAT4X4& view, const XMFLOAT4X4& proj,
                  const XMFLOAT4X4 csmLightVP[4], const XMFLOAT4& cascadeSplits);

    // Apply pass: render in-scattering into HDR (additive blend)
    void DrawApply(VkCommandBuffer cmd, uint32_t frameIndex);

    bool IsReady() const { return _ready; }

    // Runtime fog settings
    float density       = 0.15f;
    float heightFalloff = 0.0f;   // 0 = uniform fog everywhere
    float baseHeight    = 0.0f;
    float scattering    = 1.0f;
    float extinction    = 1.0f;
    float phaseG        = 0.3f;
    XMFLOAT3 lightDir   = { 0.0f, -1.0f, 0.5f };
    XMFLOAT3 lightColor = { 1.0f, 0.95f, 0.85f };
    float lightIntensity = 3.0f;

private:
    bool CreateFroxelVolumes();
    bool CreateSampler();
    bool CreateUBOs();
    bool CreateComputePipelines();
    bool CreateApplyPipeline();
    bool CreateDescriptors();
    bool CreateApplyFramebuffer();

    VulkanCore* _core          = nullptr;
    uint32_t    _framesInFlight = 3;
    VkExtent2D  _extent        = {};
    bool        _ready         = false;

    // Input views (borrowed — not owned)
    VkImageView  _depthView     = VK_NULL_HANDLE;
    VkImageView  _hdrView       = VK_NULL_HANDLE;
    VkImage      _hdrImage      = VK_NULL_HANDLE;
    VkRenderPass _hdrRenderPass = VK_NULL_HANDLE;
    VkImageView  _csmShadowView = VK_NULL_HANDLE;

    // 3D inject volume (RGBA16F, written by inject CS, read by scatter CS)
    VkImage        _injectImage  = VK_NULL_HANDLE;
    VkDeviceMemory _injectMem    = VK_NULL_HANDLE;
    VkImageView    _injectView   = VK_NULL_HANDLE; // GENERAL layout storage image

    // 3D accumulation volume (RGBA16F, written by scatter CS, read by apply PS)
    VkImage        _accumImage   = VK_NULL_HANDLE;
    VkDeviceMemory _accumMem     = VK_NULL_HANDLE;
    VkImageView    _accumView    = VK_NULL_HANDLE; // GENERAL layout for compute write, SHADER_READ_ONLY for apply

    // Samplers
    VkSampler _bilinearClamp = VK_NULL_HANDLE;  // 3D bilinear clamp (apply PS)
    VkSampler _shadowSampler = VK_NULL_HANDLE;  // shadow comparison sampler (scatter CS)
    VkSampler _pointClamp    = VK_NULL_HANDLE;  // depth read (apply PS, point)

    // Per-frame UBO
    VkBuffer       _ubo[MAX_FRAMES]       = {};
    VkDeviceMemory _uboMem[MAX_FRAMES]    = {};
    void*          _uboMapped[MAX_FRAMES] = {};

    // ── Inject compute pipeline ──
    VkDescriptorSetLayout _injectDescLayout  = VK_NULL_HANDLE;
    VkDescriptorPool      _injectDescPool    = VK_NULL_HANDLE;
    VkDescriptorSet       _injectDescSet[MAX_FRAMES] = {};
    VkPipelineLayout      _injectPipeLayout  = VK_NULL_HANDLE;
    VkPipeline            _injectPipeline    = VK_NULL_HANDLE;

    // ── Scatter compute pipeline ──
    VkDescriptorSetLayout _scatterDescLayout = VK_NULL_HANDLE;
    VkDescriptorPool      _scatterDescPool   = VK_NULL_HANDLE;
    VkDescriptorSet       _scatterDescSet[MAX_FRAMES] = {};
    VkPipelineLayout      _scatterPipeLayout = VK_NULL_HANDLE;
    VkPipeline            _scatterPipeline   = VK_NULL_HANDLE;

    // ── Apply graphics pipeline ──
    VkDescriptorSetLayout _applyDescLayout   = VK_NULL_HANDLE;
    VkDescriptorPool      _applyDescPool     = VK_NULL_HANDLE;
    VkDescriptorSet       _applyDescSet[MAX_FRAMES] = {};
    VkPipelineLayout      _applyPipeLayout   = VK_NULL_HANDLE;
    VkPipeline            _applyPipeline     = VK_NULL_HANDLE;
    VkFramebuffer         _applyFramebuffer  = VK_NULL_HANDLE;

    // Track current image layouts
    VkImageLayout _injectLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout _accumLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
};

} // namespace Luna
