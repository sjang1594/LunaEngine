#pragma once

#include <vulkan/vulkan.h>
#include <DirectXMath.h>
#include <vector>

using namespace DirectX;

namespace Luna
{

class VulkanCore;

/**
 * @brief Post-processing pipeline subsystem.
 *
 * Owns HDR intermediate, TAA history buffers, Bloom images, SSR image,
 * Motion Blur image, and all associated pipelines/descriptors.
 *
 * Pipeline flow:
 *   1. Deferred lighting writes to HDR image
 *   2. SSR compute (reads G-buffer, HDR; writes SSR image)
 *   3. Motion Blur (reads HDR + depth)
 *   4. TAA (reads MB or HDR output + history; writes new history)
 *   5. Bloom Bright (reads TAA output; writes half-res bright)
 *   6. Bloom Blur H (reads bright; writes blur)
 *   7. Bloom Blur V (reads blur; writes back to bright)
 *   8. Tonemap (reads TAA + bloom + SSR; writes swapchain)
 *
 * Thread Safety: NOT thread-safe. Call from main render thread only.
 */
class VulkanPostProcess
{
public:
    static constexpr uint32_t FRAMES_IN_FLIGHT = 3;

    // TAA constants (160 bytes, 256-byte aligned UBO)
    struct TAAConstants
    {
        float invViewProj[16];   // 64 B - jittered inverse VP
        float prevViewProj[16];  // 64 B - previous unjittered VP
        float jitter[2];         //  8 B
        float prevJitter[2];     //  8 B
        float alpha;             //  4 B
        float _pad[3];           // 12 B
    };
    static_assert(sizeof(TAAConstants) == 160, "TAAConstants must be 160 bytes");

    // Motion blur constants (152 bytes, padded to 256 in UBO)
    struct MotionBlurConstants
    {
        float invViewProj[16];   // 64 B
        float prevViewProj[16];  // 64 B
        float screenSizeX;       //  4 B
        float screenSizeY;       //  4 B
        float shutterScale;      //  4 B
        int   numSamples;        //  4 B
        float _pad[2];           //  8 B
    };
    static_assert(sizeof(MotionBlurConstants) == 152, "MotionBlurConstants must be 152 bytes");

    struct CreateInfo
    {
        VulkanCore*   core              = nullptr;
        VkExtent2D    extent            = {};
        VkFormat      swapchainFormat   = VK_FORMAT_B8G8R8A8_UNORM;
        VkImageView   depthView         = VK_NULL_HANDLE;   // G-buffer depth
        VkImageView   normalView        = VK_NULL_HANDLE;   // G-buffer normal
        VkImageView   metalRoughView    = VK_NULL_HANDLE;   // G-buffer metal/rough
        VkSampler     linearSampler     = VK_NULL_HANDLE;
        VkSampler     pointClampSampler = VK_NULL_HANDLE;
    };

    VulkanPostProcess() = default;
    ~VulkanPostProcess();

    // Non-copyable
    VulkanPostProcess(const VulkanPostProcess&) = delete;
    VulkanPostProcess& operator=(const VulkanPostProcess&) = delete;

    /**
     * @brief Create all post-processing resources.
     */
    bool Create(const CreateInfo& info);
    void Destroy();

    /**
     * @brief Recreate resolution-dependent resources after resize.
     */
    bool Resize(const CreateInfo& info);

    // === Pre-frame descriptor updates ===

    /**
     * @brief Update per-frame descriptors (TAA history ping-pong).
     * Call after command buffer fence wait in BeginFrame.
     */
    void UpdateDescriptors(uint32_t frameIndex);

    // === Pass execution ===

    /**
     * @brief Execute SSR compute pass.
     * Reads: depth, normal, metalRough, HDR
     * Writes: SSR image
     */
    void DrawSSR(VkCommandBuffer cmd, uint32_t frameIndex,
                 const XMFLOAT4X4& view, const XMFLOAT4X4& proj);

    /**
     * @brief Execute motion blur pass.
     * Reads: HDR, depth
     * Writes: Motion blur image
     */
    void DrawMotionBlur(VkCommandBuffer cmd, uint32_t frameIndex,
                        const XMFLOAT4X4& view, const XMFLOAT4X4& proj,
                        const XMFLOAT4X4& prevViewProj);

    /**
     * @brief Execute TAA pass.
     * Reads: current frame (HDR or MB), history, depth
     * Writes: TAA history[write]
     */
    void DrawTAA(VkCommandBuffer cmd, uint32_t frameIndex,
                 const TAAConstants& taaConst);

    /**
     * @brief Execute bloom bright extraction.
     * Reads: TAA output
     * Writes: half-res bloom bright
     */
    void DrawBloomBright(VkCommandBuffer cmd);

    /**
     * @brief Execute bloom blur pass (horizontal or vertical).
     * @param horizontal True for H-blur, false for V-blur
     */
    void DrawBloomBlur(VkCommandBuffer cmd, bool horizontal);

    /**
     * @brief Execute final tonemap pass.
     * Reads: TAA output, bloom, SSR
     * Writes: swapchain (via tonemap framebuffer)
     */
    void DrawTonemap(VkCommandBuffer cmd, uint32_t imageIndex);

    // === Accessors ===

    bool IsReady()            const { return _ready; }
    bool IsTAAReady()         const { return _taaPipeline != VK_NULL_HANDLE; }
    bool IsMotionBlurReady()  const { return _mbPipeline != VK_NULL_HANDLE; }
    bool IsSSRReady()         const { return _ssrPipeline != VK_NULL_HANDLE; }

    VkImageView GetHDRView()  const { return _hdrView; }
    VkImageView GetSSRView()  const { return _ssrView; }
    VkImage     GetHDRImage() const { return _hdrImage; }
    VkImage     GetSSRImage() const { return _ssrImage; }

    // Render-graph image accessors (VkImage handles for import)
    VkImage GetTAAHistoryImage(int idx) const { return _taaHistoryImage[idx]; }
    VkImage GetMotionBlurImage()       const { return _mbImage; }
    VkImage GetBloomBrightImage()      const { return _bloomBrightImage; }
    VkImage GetBloomBlurImage()        const { return _bloomBlurImage; }

    // Render passes for external framebuffer creation
    VkRenderPass GetPPRenderPass()       const { return _ppRenderPass; }
    VkRenderPass GetTonemapRenderPass()  const { return _tonemapRenderPass; }

    // Deferred HDR framebuffer (for deferred lighting target)
    VkFramebuffer GetDeferredHDRFramebuffer() const { return _deferredHDRFramebuffer; }

    // TAA history index (for ping-pong reads)
    int GetTAAHistoryWriteIndex() const { return _taaHistoryIndex; }

    // Frame count (needed for TAA warmup alpha)
    uint32_t GetFrameCount() const { return _frameCount; }

    // Jitter values for TAA (set externally by FlushDraws)
    void SetJitter(float curX, float curY, float prevX, float prevY);
    void SetUnjitteredVP(const float* vp16);
    const float* GetPrevUnjitteredVP() const { return _prevUnjitteredVP; }

    // Swapchain framebuffer for tonemap output
    void SetTonemapFramebuffers(const std::vector<VkFramebuffer>& fbs) { _tonemapFramebuffers = fbs; }

private:
    bool CreateRenderPasses();
    bool CreateImages();
    bool CreateFramebuffers();
    bool CreateDescriptorLayouts();
    bool CreateDescriptorPool();
    bool CreateDescriptorSets();
    bool CreatePipelines();
    bool CreateSSRResources();
    bool CreateMotionBlurResources();
    bool CreateTAAResources();
    bool CreateBloomResources();
    bool CreateTonemapResources();

    void DestroyImages();
    void DestroyFramebuffers();
    void DestroyPipelines();
    void DestroyDescriptors();
    void DestroySSRResources();
    void DestroyMotionBlurResources();
    void DestroyTAAResources();
    void DestroyBloomResources();
    void DestroyTonemapResources();

    VulkanCore* _core = nullptr;
    bool _ready = false;

    VkExtent2D _extent = {};
    VkFormat   _swapchainFormat = VK_FORMAT_B8G8R8A8_UNORM;

    // External refs (not owned)
    VkImageView _depthView       = VK_NULL_HANDLE;
    VkImageView _normalView      = VK_NULL_HANDLE;
    VkImageView _metalRoughView  = VK_NULL_HANDLE;
    VkSampler   _linearSampler   = VK_NULL_HANDLE;
    VkSampler   _pointClampSampler = VK_NULL_HANDLE;

    // --- Render Passes ---
    VkRenderPass _ppRenderPass      = VK_NULL_HANDLE;  // R16G16B16A16_SFLOAT → SHADER_READ_ONLY
    VkRenderPass _tonemapRenderPass = VK_NULL_HANDLE;  // swapchain format → PRESENT_SRC

    // --- HDR Image (full-res, COLOR_ATTACHMENT | SAMPLED) ---
    VkImage        _hdrImage   = VK_NULL_HANDLE;
    VkDeviceMemory _hdrMemory  = VK_NULL_HANDLE;
    VkImageView    _hdrView    = VK_NULL_HANDLE;
    VkFramebuffer  _deferredHDRFramebuffer = VK_NULL_HANDLE;

    // --- SSR Image (full-res, STORAGE | SAMPLED, kept GENERAL) ---
    VkImage        _ssrImage   = VK_NULL_HANDLE;
    VkDeviceMemory _ssrMemory  = VK_NULL_HANDLE;
    VkImageView    _ssrView    = VK_NULL_HANDLE;

    // SSR compute pipeline
    VkDescriptorSetLayout _ssrLayout     = VK_NULL_HANDLE;
    VkDescriptorPool      _ssrDescPool   = VK_NULL_HANDLE;
    VkDescriptorSet       _ssrDescSet[FRAMES_IN_FLIGHT] = {};
    VkPipelineLayout      _ssrPipeLayout = VK_NULL_HANDLE;
    VkPipeline            _ssrPipeline   = VK_NULL_HANDLE;
    VkBuffer              _ssrCB[FRAMES_IN_FLIGHT]       = {};
    VkDeviceMemory        _ssrCBMem[FRAMES_IN_FLIGHT]    = {};
    void*                 _ssrCBMapped[FRAMES_IN_FLIGHT] = {};

    // --- Motion Blur Image (full-res, COLOR_ATTACHMENT | SAMPLED) ---
    VkImage        _mbImage  = VK_NULL_HANDLE;
    VkDeviceMemory _mbMemory = VK_NULL_HANDLE;
    VkImageView    _mbView   = VK_NULL_HANDLE;
    VkFramebuffer  _mbFB     = VK_NULL_HANDLE;

    VkDescriptorSetLayout _mbLayout   = VK_NULL_HANDLE;
    VkDescriptorPool      _mbDescPool = VK_NULL_HANDLE;
    VkDescriptorSet       _mbDescSet[FRAMES_IN_FLIGHT] = {};
    VkPipelineLayout      _mbPipeLayout = VK_NULL_HANDLE;
    VkPipeline            _mbPipeline   = VK_NULL_HANDLE;
    VkBuffer              _mbCB[FRAMES_IN_FLIGHT]       = {};
    VkDeviceMemory        _mbCBMem[FRAMES_IN_FLIGHT]    = {};
    void*                 _mbCBMapped[FRAMES_IN_FLIGHT] = {};

    // --- TAA History Images (full-res × 2, ping-pong) ---
    VkImage        _taaHistoryImage[2]  = {};
    VkDeviceMemory _taaHistoryMemory[2] = {};
    VkImageView    _taaHistoryView[2]   = {};
    VkFramebuffer  _taaFramebuffer[2]   = {};

    VkDescriptorSetLayout _taaLayout       = VK_NULL_HANDLE;
    VkDescriptorPool      _taaDescPool     = VK_NULL_HANDLE;
    VkDescriptorSet       _taaDescSet[FRAMES_IN_FLIGHT] = {};
    VkPipelineLayout      _taaPipeLayout   = VK_NULL_HANDLE;
    VkPipeline            _taaPipeline     = VK_NULL_HANDLE;
    VkBuffer              _taaCB[FRAMES_IN_FLIGHT]       = {};
    VkDeviceMemory        _taaCBMem[FRAMES_IN_FLIGHT]    = {};
    void*                 _taaCBMapped[FRAMES_IN_FLIGHT] = {};

    int      _taaHistoryIndex = 0;     // ping-pong write index
    uint32_t _frameCount      = 0;
    float    _curJitter[2]    = {};
    float    _prevJitter[2]   = {};
    float    _unjitteredVP[16]     = {};
    float    _prevUnjitteredVP[16] = {};

    // --- Bloom Images (half-res) ---
    VkImage        _bloomBrightImage  = VK_NULL_HANDLE;
    VkDeviceMemory _bloomBrightMemory = VK_NULL_HANDLE;
    VkImageView    _bloomBrightView   = VK_NULL_HANDLE;
    VkFramebuffer  _bloomBrightFB     = VK_NULL_HANDLE;

    VkImage        _bloomBlurImage  = VK_NULL_HANDLE;
    VkDeviceMemory _bloomBlurMemory = VK_NULL_HANDLE;
    VkImageView    _bloomBlurView   = VK_NULL_HANDLE;
    VkFramebuffer  _bloomBlurFB     = VK_NULL_HANDLE;

    VkDescriptorSetLayout _bloomLayout     = VK_NULL_HANDLE;  // 1 SRV + 1 sampler
    VkDescriptorPool      _bloomDescPool   = VK_NULL_HANDLE;
    VkDescriptorSet       _bloomBrightDescSet[2] = {};  // [i] reads taaHistory[i]
    VkDescriptorSet       _bloomBlurHDescSet = VK_NULL_HANDLE;
    VkDescriptorSet       _bloomBlurVDescSet = VK_NULL_HANDLE;
    VkPipelineLayout      _bloomPipeLayout = VK_NULL_HANDLE;
    VkPipeline            _bloomBrightPipeline = VK_NULL_HANDLE;
    VkPipeline            _bloomBlurPipeline   = VK_NULL_HANDLE;

    // --- Tonemap ---
    VkDescriptorSetLayout _tonemapLayout     = VK_NULL_HANDLE;  // 3 SRV + 1 sampler
    VkDescriptorPool      _tonemapDescPool   = VK_NULL_HANDLE;
    VkDescriptorSet       _tonemapDescSet[2] = {};  // [i] reads taaHistory[i]
    VkPipelineLayout      _tonemapPipeLayout = VK_NULL_HANDLE;
    VkPipeline            _tonemapPipeline   = VK_NULL_HANDLE;

    // Swapchain framebuffers (external, set via SetTonemapFramebuffers)
    std::vector<VkFramebuffer> _tonemapFramebuffers;
};

} // namespace Luna

