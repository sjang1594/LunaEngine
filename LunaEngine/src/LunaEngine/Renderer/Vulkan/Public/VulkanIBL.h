#pragma once

#include <vulkan/vulkan.h>
#include <string>

namespace Luna
{

class VulkanCore;

/**
 * @brief Image-Based Lighting: equirect→cubemap, irradiance, prefilter, BRDF LUT.
 *
 * Owns all IBL cubemaps and compute pipelines. Dispatches precompute in separate
 * GPU submissions per stage to avoid TDR timeouts.
 *
 * Thread Safety: NOT thread-safe. Call from main render thread only.
 */
class VulkanIBL
{
public:
    static constexpr uint32_t ENV_CUBE_SIZE      = 512;
    static constexpr uint32_t IRR_CUBE_SIZE      = 32;
    static constexpr uint32_t PREFILTER_CUBE_SIZE = 128;
    static constexpr uint32_t PREFILTER_MIP_COUNT = 5;
    static constexpr uint32_t BRDF_LUT_SIZE      = 512;

    struct CreateInfo
    {
        VulkanCore* core = nullptr;
    };

    VulkanIBL() = default;
    ~VulkanIBL();

    // Non-copyable
    VulkanIBL(const VulkanIBL&) = delete;
    VulkanIBL& operator=(const VulkanIBL&) = delete;

    /**
     * @brief Initialize with core pointer. Must be called before LoadHDREnvironment.
     */
    bool Init(const CreateInfo& info);

    /**
     * @brief Load equirectangular HDR, create cubemaps, and run all precompute stages.
     * @return true if IBL is ready for rendering
     */
    bool LoadHDREnvironment(const std::string& hdrPath);

    void Destroy();

    // === Accessors ===

    bool        IsReady()            const { return _ready; }
    VkImageView GetIrradianceView()  const { return _irrCubemapView; }
    VkImageView GetPrefilterView()   const { return _prefilterCubemapView; }
    VkImageView GetBRDFLUTView()     const { return _brdfLUTView; }
    VkSampler   GetIBLSampler()      const { return _iblSampler; }

private:
    bool CreateCubemaps();
    bool CreateBRDFLUT();
    bool CreateSamplers();
    bool CreateComputePipelines();
    bool DispatchPrecompute(VkImage equirectImg, VkImageView equirectView);

    VulkanCore* _core = nullptr;
    bool _ready = false;

    // Env cubemap (512², 1 mip)
    VkImage        _envCubemap      = VK_NULL_HANDLE;
    VkDeviceMemory _envCubemapMem   = VK_NULL_HANDLE;
    VkImageView    _envCubemapView  = VK_NULL_HANDLE;  // CUBE view
    VkImageView    _envCubemapArray = VK_NULL_HANDLE;  // 2D_ARRAY view for compute writes

    // Irradiance cubemap (32², 1 mip)
    VkImage        _irrCubemap      = VK_NULL_HANDLE;
    VkDeviceMemory _irrCubemapMem   = VK_NULL_HANDLE;
    VkImageView    _irrCubemapView  = VK_NULL_HANDLE;
    VkImageView    _irrCubemapArray = VK_NULL_HANDLE;

    // Prefilter cubemap (128², 5 mips)
    VkImage        _prefilterCubemap      = VK_NULL_HANDLE;
    VkDeviceMemory _prefilterCubemapMem   = VK_NULL_HANDLE;
    VkImageView    _prefilterCubemapView  = VK_NULL_HANDLE;  // CUBE view, all mips
    VkImageView    _prefilterMipView[PREFILTER_MIP_COUNT] = {};  // per-mip 2D_ARRAY

    // BRDF LUT (512×512, R16G16_SFLOAT)
    VkImage        _brdfLUT     = VK_NULL_HANDLE;
    VkDeviceMemory _brdfLUTMem  = VK_NULL_HANDLE;
    VkImageView    _brdfLUTView = VK_NULL_HANDLE;

    // Samplers
    VkSampler _iblSampler  = VK_NULL_HANDLE;  // trilinear clamp, maxLOD=5

    // Compute pipelines (4 stages)
    VkDescriptorSetLayout _equirectDSL     = VK_NULL_HANDLE;
    VkPipelineLayout      _equirectPipeLayout = VK_NULL_HANDLE;
    VkPipeline            _equirectPipeline   = VK_NULL_HANDLE;

    VkDescriptorSetLayout _irrConvDSL      = VK_NULL_HANDLE;
    VkPipelineLayout      _irrConvPipeLayout  = VK_NULL_HANDLE;
    VkPipeline            _irrConvPipeline    = VK_NULL_HANDLE;

    VkDescriptorSetLayout _prefilterDSL    = VK_NULL_HANDLE;
    VkPipelineLayout      _prefilterPipeLayout = VK_NULL_HANDLE;
    VkPipeline            _prefilterPipeline   = VK_NULL_HANDLE;

    VkDescriptorSetLayout _brdfLutDSL      = VK_NULL_HANDLE;
    VkPipelineLayout      _brdfLutPipeLayout   = VK_NULL_HANDLE;
    VkPipeline            _brdfLutPipeline     = VK_NULL_HANDLE;
};

} // namespace Luna

