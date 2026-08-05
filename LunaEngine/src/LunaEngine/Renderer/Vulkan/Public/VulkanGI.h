#pragma once
// VulkanGI.h — Phase 30: Screen-Space GI + Irradiance Probes (Vulkan)

#include <vulkan/vulkan.h>
#include <DirectXMath.h>

using namespace DirectX;

namespace Luna
{

class VulkanCore;

class VulkanGI
{
public:
    static constexpr uint32_t PROBE_GRID_X   = 8u;
    static constexpr uint32_t PROBE_GRID_Y   = 4u;
    static constexpr uint32_t PROBE_GRID_Z   = 8u;
    static constexpr uint32_t PROBE_COUNT    = 256u;
    static constexpr uint32_t PROBE_TEX_SIZE = 16u;
    static constexpr uint32_t PROBE_ATLAS_W  = 128u;
    static constexpr uint32_t PROBE_ATLAS_H  = 64u;
    static constexpr uint32_t MAX_FRAMES     = 3;

    struct CreateInfo
    {
        VulkanCore*  core           = nullptr;
        VkExtent2D   extent         = {};
        VkImageView  depthView      = VK_NULL_HANDLE;
        VkImageView  gbufAlbedoView = VK_NULL_HANDLE;
        VkImageView  gbufNormalView = VK_NULL_HANDLE;
        VkImageView  hdrView        = VK_NULL_HANDLE;
        VkImage      hdrImage       = VK_NULL_HANDLE;
        VkImageView  hiZView        = VK_NULL_HANDLE;
        VkSampler    hiZSampler     = VK_NULL_HANDLE;
        VkImageView  irrCubeView    = VK_NULL_HANDLE;
        VkSampler    iblSampler     = VK_NULL_HANDLE;
        uint32_t     framesInFlight = 3;
    };

    // GPU-side UBO — must match GLSL SSGIConstants (std140, 256B)
    struct SSGIUBO
    {
        float invViewProj[16];   //  64B
        float prevViewProj[16];  //  64B
        float view[16];          //  64B
        float screenSize[2];     //   8B
        float halfResSize[2];    //   8B
        uint32_t frameCount;     //   4B
        uint32_t numRays;        //   4B
        float maxRayDist;        //   4B
        float temporalAlpha;     //   4B
        float _pad[8];           //  32B → 256B
    };
    static_assert(sizeof(SSGIUBO) == 256, "SSGIUBO must be 256B");

    // GPU-side UBO — must match GLSL ProbeConstants (std140, 256B)
    struct ProbeUBO
    {
        float    origin[4];       //  16B  (xyz + pad)
        float    spacing[4];      //  16B
        uint32_t dims[3];         //  12B
        uint32_t _p0;             //   4B
        float    screenSize[2];   //   8B
        float    _p1[2];          //   8B
        float    invViewProj[16]; //  64B
        uint32_t probeIndex;      //   4B
        uint32_t _pp[3];          //  12B
        float    _pad[28];        // 112B → 256B total
    };
    static_assert(sizeof(ProbeUBO) == 256, "ProbeUBO must be 256B");

    VulkanGI() = default;
    ~VulkanGI();

    bool Create(const CreateInfo& info);
    void Destroy();

    // Per-frame: dispatch SSGI compute + probe update compute
    void Dispatch(VkCommandBuffer cmd, uint32_t frameIndex,
                  const XMFLOAT4X4& invVP, const XMFLOAT4X4& prevVP, const XMFLOAT4X4& view);

    bool        IsReady()          const { return _ready; }
    VkImageView GetSSGIReadView()  const;   // SSGI output for current frame
    VkImageView GetProbeIrrView()  const { return _probeIrrView; }

    // Runtime settings
    float    temporalAlpha  = 0.1f;
    uint32_t numSSGIRays    = 8u;
    float    maxRayDist     = 5.0f;
    float    probeOrigin[3] = { -8.0f, 0.0f, -8.0f };
    float    probeSpacing[3]= {  2.0f, 2.0f,  2.0f };

private:
    bool CreateSSGIImages();
    bool CreateProbeAtlas();
    bool CreateSamplers();
    bool CreateUBOs();
    bool CreateSSGIPipeline();
    bool CreateProbePipeline();
    bool CreateDescriptors();

    VulkanCore* _core          = nullptr;
    uint32_t    _framesInFlight = 3;
    VkExtent2D  _extent         = {};
    bool        _ready          = false;
    uint32_t    _frameCount     = 0;
    uint32_t    _probeIdx       = 0;
    int         _pingPong       = 0;  // 0=write, 1=history

    // Borrowed input views
    VkImageView _depthView      = VK_NULL_HANDLE;
    VkImageView _gbufAlbedoView = VK_NULL_HANDLE;
    VkImageView _gbufNormalView = VK_NULL_HANDLE;
    VkImageView _hdrView        = VK_NULL_HANDLE;
    VkImage     _hdrImage       = VK_NULL_HANDLE;
    VkImageView _hiZView        = VK_NULL_HANDLE;
    VkSampler   _hiZSampler     = VK_NULL_HANDLE;
    VkImageView _irrCubeView    = VK_NULL_HANDLE;
    VkSampler   _iblSampler     = VK_NULL_HANDLE;

    // Half-res RGBA16F ping-pong images (×2)
    VkImage        _ssgiImage[2]  = {};
    VkDeviceMemory _ssgiMem[2]    = {};
    VkImageView    _ssgiView[2]   = {};
    VkImageLayout  _ssgiLayout[2] = { VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_UNDEFINED };

    // Probe irradiance atlas (2D array: 128×64 × 8 slices)
    VkImage        _probeIrrImage  = VK_NULL_HANDLE;
    VkDeviceMemory _probeIrrMem    = VK_NULL_HANDLE;
    VkImageView    _probeIrrView   = VK_NULL_HANDLE;
    VkImageLayout  _probeIrrLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    // Samplers
    VkSampler _pointClamp    = VK_NULL_HANDLE;
    VkSampler _bilinearClamp = VK_NULL_HANDLE;

    // Per-frame UBOs
    VkBuffer       _ssgiUBO[MAX_FRAMES]       = {};
    VkDeviceMemory _ssgiUBOMem[MAX_FRAMES]    = {};
    void*          _ssgiUBOMapped[MAX_FRAMES] = {};

    VkBuffer       _probeUBO[MAX_FRAMES]       = {};
    VkDeviceMemory _probeUBOMem[MAX_FRAMES]    = {};
    void*          _probeUBOMapped[MAX_FRAMES] = {};

    // SSGI compute pipeline
    VkDescriptorSetLayout _ssgiDescLayout  = VK_NULL_HANDLE;
    VkDescriptorPool      _ssgiDescPool    = VK_NULL_HANDLE;
    VkDescriptorSet       _ssgiDescSet[MAX_FRAMES][2] = {};  // [frame][pingPong]
    VkPipelineLayout      _ssgiPipeLayout  = VK_NULL_HANDLE;
    VkPipeline            _ssgiPipeline    = VK_NULL_HANDLE;

    // Probe update compute pipeline
    VkDescriptorSetLayout _probeDescLayout  = VK_NULL_HANDLE;
    VkDescriptorPool      _probeDescPool    = VK_NULL_HANDLE;
    VkDescriptorSet       _probeDescSet[MAX_FRAMES] = {};
    VkPipelineLayout      _probePipeLayout  = VK_NULL_HANDLE;
    VkPipeline            _probePipeline    = VK_NULL_HANDLE;
};

} // namespace Luna
