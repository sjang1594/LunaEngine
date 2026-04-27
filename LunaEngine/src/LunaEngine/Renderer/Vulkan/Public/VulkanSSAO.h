#pragma once

#include <vulkan/vulkan.h>
#include <DirectXMath.h>
#include <array>

using namespace DirectX;

namespace Luna
{

class VulkanCore;

/**
 * @brief Screen-Space Ambient Occlusion (half-res, R8_UNORM).
 *
 * Renders raw SSAO then applies a blur pass. Both are fullscreen triangle
 * draws into half-resolution framebuffers.
 *
 * Thread Safety: NOT thread-safe. Call from main render thread only.
 */
class VulkanSSAO
{
public:
    static constexpr uint32_t SAMPLE_COUNT    = 16;
    static constexpr uint32_t NOISE_SIZE      = 4;
    static constexpr uint32_t MAX_FRAMES      = 3;

    struct CreateInfo
    {
        VulkanCore*  core       = nullptr;
        VkExtent2D   extent     = {};        // full-res (will be halved internally)
        VkImageView  depthView  = VK_NULL_HANDLE;
        VkImageView  normalView = VK_NULL_HANDLE;
        VkSampler    pointClampSampler = VK_NULL_HANDLE;  // shared, externally owned
        uint32_t     framesInFlight = 3;
    };

    VulkanSSAO() = default;
    ~VulkanSSAO();

    // Non-copyable
    VulkanSSAO(const VulkanSSAO&) = delete;
    VulkanSSAO& operator=(const VulkanSSAO&) = delete;

    bool Create(const CreateInfo& info);
    void Destroy();
    bool Resize(VkExtent2D extent, VkImageView depthView, VkImageView normalView);

    /**
     * @brief Draw SSAO pass and blur pass.
     * @param cmd Active command buffer
     * @param frameIndex Current frame-in-flight index
     * @param view Camera view matrix
     * @param proj Camera projection matrix
     */
    void Draw(VkCommandBuffer cmd, uint32_t frameIndex,
              const XMFLOAT4X4& view, const XMFLOAT4X4& proj);

    void DrawBlur(VkCommandBuffer cmd);

    // === Accessors ===
    VkImageView GetBlurredView() const { return _blurView; }
    VkImageView GetRawView()     const { return _rawView; }
    VkImage     GetBlurredImage() const { return _blurImage; }
    VkImage     GetRawImage()     const { return _rawImage; }
    bool        IsReady()         const { return _pipeline != VK_NULL_HANDLE; }

private:
    struct SSAOConstants
    {
        XMFLOAT4   samples[SAMPLE_COUNT];  // 256
        XMFLOAT4X4 projection;             //  64
        XMFLOAT4X4 invProjection;          //  64
        XMFLOAT4X4 view;                   //  64
        XMFLOAT2   noiseScale;             //   8
        float      radius;                 //   4
        float      bias;                   //   4
    };

    bool CreateRenderTargets(VkExtent2D halfExtent);
    bool CreateNoiseTexture();
    bool CreateSamplers();
    bool CreateRenderPass();
    bool CreateFramebuffers(VkExtent2D halfExtent);
    bool CreateDescriptors(VkImageView depthView, VkImageView normalView);
    bool CreatePipelines();
    void GenerateKernel();

    VulkanCore* _core = nullptr;
    VkSampler   _externalPointClamp = VK_NULL_HANDLE;  // not owned
    uint32_t    _framesInFlight = 3;
    uint32_t    _halfW = 0, _halfH = 0;

    // Render targets
    VkImage        _rawImage  = VK_NULL_HANDLE;
    VkDeviceMemory _rawMem    = VK_NULL_HANDLE;
    VkImageView    _rawView   = VK_NULL_HANDLE;

    VkImage        _blurImage = VK_NULL_HANDLE;
    VkDeviceMemory _blurMem   = VK_NULL_HANDLE;
    VkImageView    _blurView  = VK_NULL_HANDLE;

    // Noise texture  
    VkImage        _noiseImage = VK_NULL_HANDLE;
    VkDeviceMemory _noiseMem   = VK_NULL_HANDLE;
    VkImageView    _noiseView  = VK_NULL_HANDLE;

    // Render pass + framebuffers
    VkRenderPass  _renderPass     = VK_NULL_HANDLE;
    VkFramebuffer _rawFramebuffer = VK_NULL_HANDLE;
    VkFramebuffer _blurFramebuffer = VK_NULL_HANDLE;

    // Samplers (owned)
    VkSampler _pointWrap     = VK_NULL_HANDLE;
    VkSampler _bilinearClamp = VK_NULL_HANDLE;

    // Descriptor layouts
    VkDescriptorSetLayout _sceneLayout = VK_NULL_HANDLE;  // set=0: UBO
    VkDescriptorSetLayout _texLayout   = VK_NULL_HANDLE;  // set=1: depth+normal+noise+samplers
    VkDescriptorSetLayout _blurLayout  = VK_NULL_HANDLE;  // set=0: raw ssao+sampler

    // Pipeline layouts + pipelines
    VkPipelineLayout _pipeLayout     = VK_NULL_HANDLE;
    VkPipelineLayout _blurPipeLayout = VK_NULL_HANDLE;
    VkPipeline       _pipeline       = VK_NULL_HANDLE;
    VkPipeline       _blurPipeline   = VK_NULL_HANDLE;

    // Descriptor pool + sets
    VkDescriptorPool _descPool     = VK_NULL_HANDLE;
    VkDescriptorSet  _sceneDescSet[MAX_FRAMES] = {};
    VkDescriptorSet  _texDescSet   = VK_NULL_HANDLE;
    VkDescriptorSet  _blurDescSet  = VK_NULL_HANDLE;

    // Per-frame UBO
    VkBuffer       _cb[MAX_FRAMES]       = {};
    VkDeviceMemory _cbMem[MAX_FRAMES]    = {};
    void*          _cbMapped[MAX_FRAMES] = {};

    SSAOConstants _kernel{};
};

} // namespace Luna

