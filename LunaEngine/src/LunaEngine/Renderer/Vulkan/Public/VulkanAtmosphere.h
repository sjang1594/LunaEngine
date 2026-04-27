#pragma once
// VulkanAtmosphere.h — Phase 28: Physically-based atmosphere rendering (Hillaire 2020)

#include <vulkan/vulkan.h>
#include <DirectXMath.h>

using namespace DirectX;

namespace Luna
{

class VulkanCore;

class VulkanAtmosphere
{
public:
    static constexpr uint32_t TRANSMITTANCE_W = 256;
    static constexpr uint32_t TRANSMITTANCE_H = 64;
    static constexpr uint32_t MULTISCATTER_W  = 32;
    static constexpr uint32_t MULTISCATTER_H  = 32;
    static constexpr uint32_t SKYVIEW_W       = 192;
    static constexpr uint32_t SKYVIEW_H       = 108;
    static constexpr uint32_t MAX_FRAMES      = 3;

    struct CreateInfo
    {
        VulkanCore*  core         = nullptr;
        VkExtent2D   extent       = {};
        VkImageView  depthView    = VK_NULL_HANDLE;
        VkImageView  hdrView      = VK_NULL_HANDLE;  // scene HDR to read in composite
        VkImage      hdrImage     = VK_NULL_HANDLE;
        VkRenderPass ppRenderPass = VK_NULL_HANDLE;   // post-process render pass (RGBA16F)
        uint32_t     framesInFlight = 3;
    };

    // Atmosphere UBO — matches GLSL AtmosphereUBO layout (std140)
    struct AtmosphereUBO
    {
        float sunDirection[3];    float sunIntensity;        // 16B
        float rayleighScat[3];    float rayleighDensityH;    // 16B
        float mieScattering;      float mieAbsorption;
        float mieDensityH;        float miePhaseG;           // 16B
        float ozoneAbsorption[3]; float ozoneCenterH;        // 16B
        float ozoneWidth;         float groundRadius;
        float atmosphereRadius;   float cameraHeight;        // 16B
        float groundAlbedo[3];    float sunAngularRadius;    // 16B
        float invViewProj[16];                                // 64B
        float screenRes[2];       float nearPlane;
        float farPlane;                                       // 16B
    };                                                        // 176B total
    static_assert(sizeof(AtmosphereUBO) == 176, "AtmosphereUBO must be 176 bytes");

    VulkanAtmosphere() = default;
    ~VulkanAtmosphere();

    bool Create(const CreateInfo& info);
    void Destroy();

    // Per-frame: update UBO + dispatch sky-view LUT compute
    void Update(VkCommandBuffer cmd, uint32_t frameIndex,
                const XMFLOAT4X4& view, const XMFLOAT4X4& proj);

    // Composite pass: render sky + sun into HDR
    void DrawComposite(VkCommandBuffer cmd, uint32_t frameIndex);

    // Sun direction control
    void SetSunElevationAzimuth(float elevDeg, float azimDeg);
    XMFLOAT3 GetSunDirection() const { return _sunDir; }
    float GetSunElevation() const { return _sunElevation; }
    float GetSunAzimuth()   const { return _sunAzimuth; }
    void  SetSunIntensity(float i) { _sunIntensity = i; }
    float GetSunIntensity() const { return _sunIntensity; }

    bool IsReady() const { return _ready; }

    VkImage     GetSkyViewImage()     const { return _skyViewImage; }
    VkImageView GetSkyViewView()      const { return _skyViewView; }

private:
    bool CreateLUTImages();
    bool CreateSampler();
    bool CreateUBO();
    bool CreateComputePipelines();
    bool CreateAtmosphereRenderPass();
    bool CreateCompositePipeline();
    bool CreateDescriptors();
    void Precompute(VkCommandBuffer cmd);

    VulkanCore* _core = nullptr;
    uint32_t    _framesInFlight = 3;
    VkExtent2D  _extent = {};
    bool        _ready = false;
    bool        _precomputed = false;
    bool        _skyViewReady = false;  // true after first Update() completes

    // Sun parameters
    XMFLOAT3 _sunDir = { 0.577f, 0.577f, 0.577f };
    float    _sunElevation = 45.0f;
    float    _sunAzimuth   = 180.0f;
    float    _sunIntensity = 20.0f;

    // LUT images
    VkImage        _transmittanceImage = VK_NULL_HANDLE;
    VkDeviceMemory _transmittanceMem   = VK_NULL_HANDLE;
    VkImageView    _transmittanceView  = VK_NULL_HANDLE;

    VkImage        _multiScatterImage  = VK_NULL_HANDLE;
    VkDeviceMemory _multiScatterMem    = VK_NULL_HANDLE;
    VkImageView    _multiScatterView   = VK_NULL_HANDLE;

    VkImage        _skyViewImage       = VK_NULL_HANDLE;
    VkDeviceMemory _skyViewMem         = VK_NULL_HANDLE;
    VkImageView    _skyViewView        = VK_NULL_HANDLE;

    // Sampler
    VkSampler _lutSampler = VK_NULL_HANDLE;

    // Per-frame UBO
    VkBuffer       _ubo[MAX_FRAMES]       = {};
    VkDeviceMemory _uboMem[MAX_FRAMES]    = {};
    void*          _uboMapped[MAX_FRAMES] = {};

    // Compute pipelines
    VkDescriptorSetLayout _transmittanceDescLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout _multiScatterDescLayout  = VK_NULL_HANDLE;
    VkDescriptorSetLayout _skyViewDescLayout       = VK_NULL_HANDLE;
    VkDescriptorPool      _computeDescPool         = VK_NULL_HANDLE;
    VkDescriptorSet       _transmittanceDescSet    = VK_NULL_HANDLE;
    VkDescriptorSet       _multiScatterDescSet     = VK_NULL_HANDLE;
    VkDescriptorSet       _skyViewDescSet[MAX_FRAMES] = {};
    VkPipelineLayout      _transmittancePipeLayout = VK_NULL_HANDLE;
    VkPipelineLayout      _multiScatterPipeLayout  = VK_NULL_HANDLE;
    VkPipelineLayout      _skyViewPipeLayout       = VK_NULL_HANDLE;
    VkPipeline            _transmittancePipeline   = VK_NULL_HANDLE;
    VkPipeline            _multiScatterPipeline    = VK_NULL_HANDLE;
    VkPipeline            _skyViewPipeline         = VK_NULL_HANDLE;

    // Composite (graphics) pipeline
    VkDescriptorSetLayout _compositeDescLayout     = VK_NULL_HANDLE;
    VkDescriptorPool      _compositeDescPool       = VK_NULL_HANDLE;
    VkDescriptorSet       _compositeDescSet[MAX_FRAMES] = {};
    VkPipelineLayout      _compositePipeLayout     = VK_NULL_HANDLE;
    VkPipeline            _compositePipeline       = VK_NULL_HANDLE;
    VkRenderPass          _compositeRenderPass     = VK_NULL_HANDLE; // borrowed, not owned
    VkRenderPass          _atmosphereRenderPass    = VK_NULL_HANDLE; // owned — LOAD op, SHADER_READ_ONLY initial/final
    VkFramebuffer         _compositeFramebuffer    = VK_NULL_HANDLE;
    VkImageView           _hdrView                 = VK_NULL_HANDLE; // borrowed
    VkImageView           _depthView               = VK_NULL_HANDLE; // borrowed
    VkImage               _hdrImage                = VK_NULL_HANDLE; // borrowed
};

} // namespace Luna

