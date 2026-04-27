#include "LunaPCH.h"
#include "LunaEngine/Renderer/Vulkan/Public/VulkanIBL.h"
#include "LunaEngine/Renderer/Vulkan/Public/VulkanCore.h"
#include "LunaEngine/Utils/FileSystemUtil.h"
#include "Logger/Logger.h"
#include "stb_image.h"

#include <algorithm>
#include <vector>

namespace Luna
{

// ===========================================================================
// Local shader compilation helper (duplicated — TODO: VulkanShaderUtils)
// ===========================================================================
static bool CompileGLSLtoSPIRV(const std::wstring& glslPath, std::vector<uint32_t>& outSpirv)
{
    wchar_t sdkBuf[MAX_PATH] = {};
    DWORD sdkLen = GetEnvironmentVariableW(L"VULKAN_SDK", sdkBuf, MAX_PATH);
    std::wstring glslcPath = (sdkLen > 0)
        ? (std::wstring(sdkBuf) + L"\\Bin\\glslc.exe") : L"glslc.exe";

    wchar_t tempDir[MAX_PATH] = {}, tempBase[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempDir);
    GetTempFileNameW(tempDir, L"spv", 0, tempBase);
    DeleteFileW(tempBase);
    std::wstring spvPath = std::wstring(tempBase) + L".spv";

    std::wstring stage;
    if      (glslPath.find(L".vert.")  != std::wstring::npos) stage = L"vertex";
    else if (glslPath.find(L".frag.")  != std::wstring::npos) stage = L"fragment";
    else if (glslPath.find(L".comp.")  != std::wstring::npos) stage = L"compute";
    else return false;

    std::wstring cmd = L"\"" + glslcPath + L"\" --target-env=vulkan1.1 --target-spv=spv1.4"
                     + L" -fshader-stage=" + stage
                     + L" -o \"" + spvPath + L"\" \"" + glslPath + L"\"";

    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE hRead = INVALID_HANDLE_VALUE, hWrite = INVALID_HANDLE_VALUE;
    CreatePipe(&hRead, &hWrite, &sa, 0);
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si); si.hStdOutput = hWrite; si.hStdError = hWrite; si.dwFlags = STARTF_USESTDHANDLES;
    PROCESS_INFORMATION pi{};
    bool started = !!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(hWrite);
    if (!started) { CloseHandle(hRead); return false; }

    char buf[512]; DWORD nRead = 0;
    while (ReadFile(hRead, buf, sizeof(buf)-1, &nRead, nullptr) && nRead) {}
    CloseHandle(hRead);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0; GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    if (exitCode != 0) { DeleteFileW(spvPath.c_str()); return false; }

    HANDLE hFile = CreateFileW(spvPath.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    DWORD fileSize = GetFileSize(hFile, nullptr), bytesRead = 0;
    outSpirv.resize(fileSize / 4);
    ReadFile(hFile, outSpirv.data(), fileSize, &bytesRead, nullptr);
    CloseHandle(hFile); DeleteFileW(spvPath.c_str());
    return bytesRead == fileSize;
}

// ===========================================================================
// Destructor
// ===========================================================================
VulkanIBL::~VulkanIBL()
{
    Destroy();
}

// ===========================================================================
// Init
// ===========================================================================
bool VulkanIBL::Init(const CreateInfo& info)
{
    if (!info.core) { LUNA_LOG_ERROR("VulkanIBL: null core"); return false; }
    _core = info.core;
    return true;
}

// ===========================================================================
// LoadHDREnvironment
// ===========================================================================
bool VulkanIBL::LoadHDREnvironment(const std::string& hdrPath)
{
    if (!_core) { LUNA_LOG_ERROR("VulkanIBL: null core"); return false; }

    VkDevice dev = _core->GetDevice();

    // Load equirectangular HDR via stb_image
    int w, h, c;
    float* pixels = stbi_loadf(hdrPath.c_str(), &w, &h, &c, 4);
    if (!pixels) {
        LUNA_LOG_ERROR("VulkanIBL: failed to load HDR '%s': %s", hdrPath.c_str(), stbi_failure_reason());
        return false;
    }
    VkDeviceSize imgSz = (VkDeviceSize)w * h * 4 * sizeof(float);

    // Upload to device-local 2D image (R32G32B32A32_SFLOAT)
    VkImage        equirectImg  = VK_NULL_HANDLE;
    VkDeviceMemory equirectMem  = VK_NULL_HANDLE;
    VkImageView    equirectView = VK_NULL_HANDLE;
    {
        VkBuffer stg; VkDeviceMemory stgMem;
        _core->CreateBuffer(imgSz, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stg, stgMem);
        void* p; vkMapMemory(dev, stgMem, 0, imgSz, 0, &p);
        memcpy(p, pixels, (size_t)imgSz);
        vkUnmapMemory(dev, stgMem);
        stbi_image_free(pixels);

        VkImageCreateInfo ii{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        ii.extent = { (uint32_t)w, (uint32_t)h, 1 };
        ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        vkCreateImage(dev, &ii, nullptr, &equirectImg);

        VkMemoryRequirements req; vkGetImageMemoryRequirements(dev, equirectImg, &req);
        VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = _core->FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkAllocateMemory(dev, &ai, nullptr, &equirectMem);
        vkBindImageMemory(dev, equirectImg, equirectMem, 0);

        _core->TransitionImageLayout(equirectImg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        _core->CopyBufferToImage(stg, equirectImg, (uint32_t)w, (uint32_t)h);
        _core->TransitionImageLayout(equirectImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        vkDestroyBuffer(dev, stg, nullptr); vkFreeMemory(dev, stgMem, nullptr);

        if (_core->IsDeviceLost()) {
            vkDestroyImage(dev, equirectImg, nullptr); vkFreeMemory(dev, equirectMem, nullptr);
            return false;
        }

        VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = equirectImg; vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCreateImageView(dev, &vi, nullptr, &equirectView);
    }

    // Create resources and dispatch
    bool ok = CreateCubemaps() && CreateBRDFLUT() && CreateSamplers()
           && CreateComputePipelines() && DispatchPrecompute(equirectImg, equirectView);

    vkDestroyImageView(dev, equirectView, nullptr);
    vkDestroyImage(dev, equirectImg, nullptr);
    vkFreeMemory(dev, equirectMem, nullptr);

    if (!ok) { Destroy(); return false; }

    _ready = true;
    LUNA_LOG_INFO("VulkanIBL: environment loaded - IBL active");
    return true;
}

// ===========================================================================
// Destroy
// ===========================================================================
void VulkanIBL::Destroy()
{
    if (!_core) return;
    VkDevice dev = _core->GetDevice();
    if (!dev) return;

    _ready = false;

    // Pipelines + DSLs
    auto dp = [&](VkPipeline& p, VkPipelineLayout& pl, VkDescriptorSetLayout& dsl) {
        if (p)   { vkDestroyPipeline(dev, p, nullptr);              p   = VK_NULL_HANDLE; }
        if (pl)  { vkDestroyPipelineLayout(dev, pl, nullptr);       pl  = VK_NULL_HANDLE; }
        if (dsl) { vkDestroyDescriptorSetLayout(dev, dsl, nullptr); dsl = VK_NULL_HANDLE; }
    };
    dp(_brdfLutPipeline,   _brdfLutPipeLayout,   _brdfLutDSL);
    dp(_prefilterPipeline, _prefilterPipeLayout, _prefilterDSL);
    dp(_irrConvPipeline,   _irrConvPipeLayout,   _irrConvDSL);
    dp(_equirectPipeline,  _equirectPipeLayout,  _equirectDSL);

    if (_iblSampler) { vkDestroySampler(dev, _iblSampler, nullptr); _iblSampler = VK_NULL_HANDLE; }

    // BRDF LUT
    if (_brdfLUTView) { vkDestroyImageView(dev, _brdfLUTView, nullptr); _brdfLUTView = VK_NULL_HANDLE; }
    if (_brdfLUT)     { vkDestroyImage(dev, _brdfLUT, nullptr);         _brdfLUT     = VK_NULL_HANDLE; }
    if (_brdfLUTMem)  { vkFreeMemory(dev, _brdfLUTMem, nullptr);        _brdfLUTMem  = VK_NULL_HANDLE; }

    // Prefilter
    for (uint32_t m = 0; m < PREFILTER_MIP_COUNT; m++)
        if (_prefilterMipView[m]) { vkDestroyImageView(dev, _prefilterMipView[m], nullptr); _prefilterMipView[m] = VK_NULL_HANDLE; }
    if (_prefilterCubemapView) { vkDestroyImageView(dev, _prefilterCubemapView, nullptr); _prefilterCubemapView = VK_NULL_HANDLE; }
    if (_prefilterCubemap)     { vkDestroyImage(dev, _prefilterCubemap, nullptr);         _prefilterCubemap     = VK_NULL_HANDLE; }
    if (_prefilterCubemapMem)  { vkFreeMemory(dev, _prefilterCubemapMem, nullptr);        _prefilterCubemapMem  = VK_NULL_HANDLE; }

    // Irradiance
    auto di = [&](VkImageView& av, VkImageView& cv, VkImage& img, VkDeviceMemory& mem) {
        if (av)  { vkDestroyImageView(dev, av, nullptr);  av  = VK_NULL_HANDLE; }
        if (cv)  { vkDestroyImageView(dev, cv, nullptr);  cv  = VK_NULL_HANDLE; }
        if (img) { vkDestroyImage(dev, img, nullptr);     img = VK_NULL_HANDLE; }
        if (mem) { vkFreeMemory(dev, mem, nullptr);        mem = VK_NULL_HANDLE; }
    };
    di(_irrCubemapArray, _irrCubemapView, _irrCubemap, _irrCubemapMem);
    di(_envCubemapArray, _envCubemapView, _envCubemap, _envCubemapMem);
}

// ===========================================================================
// CreateCubemaps
// ===========================================================================
bool VulkanIBL::CreateCubemaps()
{
    VkDevice dev = _core->GetDevice();

    auto createCubemap = [&](uint32_t size, uint32_t mips, VkFormat fmt,
                              VkImage& img, VkDeviceMemory& mem) -> bool {
        VkImageCreateInfo ii{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ii.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT; ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = fmt; ii.extent = { size, size, 1 }; ii.mipLevels = mips; ii.arrayLayers = 6;
        ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateImage(dev, &ii, nullptr, &img) != VK_SUCCESS) return false;
        VkMemoryRequirements req; vkGetImageMemoryRequirements(dev, img, &req);
        VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = _core->FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        return vkAllocateMemory(dev, &ai, nullptr, &mem) == VK_SUCCESS
            && vkBindImageMemory(dev, img, mem, 0) == VK_SUCCESS;
    };

    auto makeCubeView = [&](VkImage img, VkFormat fmt, uint32_t mips) -> VkImageView {
        VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = img; vi.viewType = VK_IMAGE_VIEW_TYPE_CUBE; vi.format = fmt;
        vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 6 };
        VkImageView v = VK_NULL_HANDLE; vkCreateImageView(dev, &vi, nullptr, &v); return v;
    };

    auto makeArrayView = [&](VkImage img, VkFormat fmt, uint32_t baseMip, uint32_t mipCnt) -> VkImageView {
        VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY; vi.format = fmt;
        vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, baseMip, mipCnt, 0, 6 };
        VkImageView v = VK_NULL_HANDLE; vkCreateImageView(dev, &vi, nullptr, &v); return v;
    };

    const VkFormat cubeFmt = VK_FORMAT_R16G16B16A16_SFLOAT;

    if (!createCubemap(ENV_CUBE_SIZE, 1, cubeFmt, _envCubemap, _envCubemapMem)) return false;
    _envCubemapView  = makeCubeView(_envCubemap, cubeFmt, 1);
    _envCubemapArray = makeArrayView(_envCubemap, cubeFmt, 0, 1);

    if (!createCubemap(IRR_CUBE_SIZE, 1, cubeFmt, _irrCubemap, _irrCubemapMem)) return false;
    _irrCubemapView  = makeCubeView(_irrCubemap, cubeFmt, 1);
    _irrCubemapArray = makeArrayView(_irrCubemap, cubeFmt, 0, 1);

    if (!createCubemap(PREFILTER_CUBE_SIZE, PREFILTER_MIP_COUNT, cubeFmt, _prefilterCubemap, _prefilterCubemapMem)) return false;
    _prefilterCubemapView = makeCubeView(_prefilterCubemap, cubeFmt, PREFILTER_MIP_COUNT);
    for (uint32_t m = 0; m < PREFILTER_MIP_COUNT; m++)
        _prefilterMipView[m] = makeArrayView(_prefilterCubemap, cubeFmt, m, 1);

    return true;
}

// ===========================================================================
// CreateBRDFLUT
// ===========================================================================
bool VulkanIBL::CreateBRDFLUT()
{
    VkDevice dev = _core->GetDevice();

    VkImageCreateInfo ii{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_R16G16_SFLOAT;
    ii.extent = { BRDF_LUT_SIZE, BRDF_LUT_SIZE, 1 };
    ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (vkCreateImage(dev, &ii, nullptr, &_brdfLUT) != VK_SUCCESS) return false;

    VkMemoryRequirements req; vkGetImageMemoryRequirements(dev, _brdfLUT, &req);
    VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = _core->FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(dev, &ai, nullptr, &_brdfLUTMem) != VK_SUCCESS) return false;
    vkBindImageMemory(dev, _brdfLUT, _brdfLUTMem, 0);

    _brdfLUTView = _core->CreateImageView(_brdfLUT, VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT);
    return _brdfLUTView != VK_NULL_HANDLE;
}

// ===========================================================================
// CreateSamplers
// ===========================================================================
bool VulkanIBL::CreateSamplers()
{
    VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = VK_FILTER_LINEAR; si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxAnisotropy = 1.0f; si.minLod = 0.0f; si.maxLod = (float)PREFILTER_MIP_COUNT;

    return vkCreateSampler(_core->GetDevice(), &si, nullptr, &_iblSampler) == VK_SUCCESS;
}

// ===========================================================================
// CreateComputePipelines
// ===========================================================================
bool VulkanIBL::CreateComputePipelines()
{
    VkDevice dev = _core->GetDevice();

    // Full layout: UBO + SampledImage + StorageImage + Sampler
    auto makeFullDSL = [&]() -> VkDescriptorSetLayout {
        VkDescriptorSetLayoutBinding bs[4]{};
        bs[0] = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        bs[1] = { 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        bs[2] = { 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        bs[3] = { 3, VK_DESCRIPTOR_TYPE_SAMPLER,        1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        VkDescriptorSetLayoutCreateInfo li{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        li.bindingCount = 4; li.pBindings = bs;
        VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
        vkCreateDescriptorSetLayout(dev, &li, nullptr, &dsl); return dsl;
    };

    // BRDF layout: StorageImage only
    auto makeBrdfDSL = [&]() -> VkDescriptorSetLayout {
        VkDescriptorSetLayoutBinding b{ 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        VkDescriptorSetLayoutCreateInfo li{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        li.bindingCount = 1; li.pBindings = &b;
        VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
        vkCreateDescriptorSetLayout(dev, &li, nullptr, &dsl); return dsl;
    };

    _equirectDSL  = makeFullDSL();
    _irrConvDSL   = makeFullDSL();
    _prefilterDSL = makeFullDSL();
    _brdfLutDSL   = makeBrdfDSL();

    auto makeCompute = [&](const wchar_t* shader, VkDescriptorSetLayout dsl,
                            VkPipelineLayout& outLayout, VkPipeline& outPipeline) -> bool {
        VkPipelineLayoutCreateInfo pli{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pli.setLayoutCount = 1; pli.pSetLayouts = &dsl;
        vkCreatePipelineLayout(dev, &pli, nullptr, &outLayout);

        std::vector<uint32_t> csS;
        if (!CompileGLSLtoSPIRV(GetShaderFullPath(shader).wstring(), csS)) return false;
        VkShaderModuleCreateInfo smi{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        smi.codeSize = csS.size() * 4; smi.pCode = csS.data();
        VkShaderModule csM = VK_NULL_HANDLE;
        vkCreateShaderModule(dev, &smi, nullptr, &csM);
        VkComputePipelineCreateInfo cpi{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpi.stage.module = csM; cpi.stage.pName = "main";
        cpi.layout = outLayout;
        VkResult r = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &outPipeline);
        vkDestroyShaderModule(dev, csM, nullptr);
        return r == VK_SUCCESS;
    };

    return makeCompute(L"equirect_to_cube_vk.comp.glsl", _equirectDSL, _equirectPipeLayout, _equirectPipeline)
        && makeCompute(L"irradiance_conv_vk.comp.glsl",  _irrConvDSL,  _irrConvPipeLayout,  _irrConvPipeline)
        && makeCompute(L"prefilter_env_vk.comp.glsl",    _prefilterDSL, _prefilterPipeLayout, _prefilterPipeline)
        && makeCompute(L"brdf_lut_vk.comp.glsl",         _brdfLutDSL,  _brdfLutPipeLayout,  _brdfLutPipeline);
}

// ===========================================================================
// DispatchPrecompute — separate submissions per stage to avoid TDR
// ===========================================================================
bool VulkanIBL::DispatchPrecompute(VkImage equirectImg, VkImageView equirectView)
{
    VkDevice dev = _core->GetDevice();

    // Create a dedicated command pool for IBL precompute.
    // Destroying this pool before cleanupTemp ensures the validation
    // layer drops all CB→resource tracking, eliminating false
    // "in use" errors when staging resources are later destroyed.
    VkCommandPool localPool = VK_NULL_HANDLE;
    {
        VkCommandPoolCreateInfo ci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        ci.queueFamilyIndex = _core->GetGraphicsQueueFamily();
        ci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        if (vkCreateCommandPool(dev, &ci, nullptr, &localPool) != VK_SUCCESS) {
            LUNA_LOG_ERROR("VulkanIBL: failed to create local command pool");
            return false;
        }
    }

    auto beginCmd = [&]() -> VkCommandBuffer {
        if (_core->IsDeviceLost()) return VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = localPool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(dev, &ai, &cmd) != VK_SUCCESS) return VK_NULL_HANDLE;
        VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
        return cmd;
    };

    auto endCmd = [&](VkCommandBuffer cmd) {
        if (!cmd || _core->IsDeviceLost()) return;
        vkEndCommandBuffer(cmd);
        VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        VkResult r = vkQueueSubmit(_core->GetGraphicsQueue(), 1, &si, VK_NULL_HANDLE);
        if (r == VK_ERROR_DEVICE_LOST) { _core->SetDeviceLost(); return; }
        r = vkQueueWaitIdle(_core->GetGraphicsQueue());
        if (r == VK_ERROR_DEVICE_LOST) _core->SetDeviceLost();
    };

    VkSampler linearSampler = VK_NULL_HANDLE;
    {
        VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        si.magFilter = VK_FILTER_LINEAR; si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxAnisotropy = 1.0f; si.maxLod = 1.0f;
        vkCreateSampler(dev, &si, nullptr, &linearSampler);
    }

    auto allocSet = [&](VkDescriptorPool& pool, const std::vector<VkDescriptorPoolSize>& sizes,
                         VkDescriptorSetLayout dsl) -> VkDescriptorSet {
        VkDescriptorPoolCreateInfo pi{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pi.maxSets = 1; pi.poolSizeCount = (uint32_t)sizes.size(); pi.pPoolSizes = sizes.data();
        vkCreateDescriptorPool(dev, &pi, nullptr, &pool);
        VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &dsl;
        VkDescriptorSet ds = VK_NULL_HANDLE; vkAllocateDescriptorSets(dev, &ai, &ds); return ds;
    };

    auto makeCB = [&](const void* data, VkDeviceSize sz, VkBuffer& buf, VkDeviceMemory& mem) {
        _core->CreateBuffer(sz, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, buf, mem);
        void* p; vkMapMemory(dev, mem, 0, sz, 0, &p); memcpy(p, data, (size_t)sz); vkUnmapMemory(dev, mem);
    };

    std::vector<VkDescriptorPool> tempPools;
    std::vector<VkBuffer> tempBufs;
    std::vector<VkDeviceMemory> tempMems;

    auto cleanupTemp = [&]() {
        for (auto p : tempPools) vkDestroyDescriptorPool(dev, p, nullptr);
        for (auto b : tempBufs)  vkDestroyBuffer(dev, b, nullptr);
        for (auto m : tempMems)  vkFreeMemory(dev, m, nullptr);
        tempPools.clear(); tempBufs.clear(); tempMems.clear();
    };

    auto checkDeviceLost = [&](const char* stage) -> bool {
        if (_core->IsDeviceLost()) {
            vkDestroyCommandPool(dev, localPool, nullptr); localPool = VK_NULL_HANDLE;
            cleanupTemp(); vkDestroySampler(dev, linearSampler, nullptr);
            LUNA_LOG_ERROR("VulkanIBL: device lost during %s", stage);
            return true;
        }
        return false;
    };

    std::vector<VkDescriptorPoolSize> fullPSZ = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 }, { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1 },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 },  { VK_DESCRIPTOR_TYPE_SAMPLER, 1 },
    };

    auto writeFullDS = [&](VkDescriptorSet ds, VkBuffer cbBuf, VkDeviceSize cbSz,
                            VkImageView srcView, VkImageLayout srcLayout,
                            VkImageView dstView, VkSampler smp) {
        VkDescriptorBufferInfo cbBI{ cbBuf, 0, cbSz };
        VkDescriptorImageInfo srcII{ VK_NULL_HANDLE, srcView, srcLayout };
        VkDescriptorImageInfo dstII{ VK_NULL_HANDLE, dstView, VK_IMAGE_LAYOUT_GENERAL };
        VkDescriptorImageInfo smpII{ smp, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
        VkWriteDescriptorSet ws[4]{};
        ws[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ds, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &cbBI, nullptr };
        ws[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ds, 1, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &srcII, nullptr, nullptr };
        ws[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ds, 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &dstII, nullptr, nullptr };
        ws[3] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ds, 3, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER, &smpII, nullptr, nullptr };
        vkUpdateDescriptorSets(dev, 4, ws, 0, nullptr);
    };

    auto transitionToGeneral = [](VkCommandBuffer cmd, VkImage img, uint32_t baseMip, uint32_t mips, uint32_t layers) {
        VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img; b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, baseMip, mips, 0, layers };
        b.srcAccessMask = 0; b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);
    };

    auto transitionToRead = [](VkCommandBuffer cmd, VkImage img, uint32_t baseMip, uint32_t mips, uint32_t layers) {
        VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.oldLayout = VK_IMAGE_LAYOUT_GENERAL; b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img; b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, baseMip, mips, 0, layers };
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);
    };

    // Stage 1: Equirect → EnvCube
    {
        VkCommandBuffer cmd = beginCmd();
        if (!cmd) { vkDestroyCommandPool(dev, localPool, nullptr); vkDestroySampler(dev, linearSampler, nullptr); return false; }

        transitionToGeneral(cmd, _envCubemap, 0, 1, 6);

        uint32_t cbData[4] = { ENV_CUBE_SIZE, 0, 0, 0 };
        VkBuffer cbBuf; VkDeviceMemory cbMem;
        makeCB(cbData, 16, cbBuf, cbMem);
        tempBufs.push_back(cbBuf); tempMems.push_back(cbMem);

        VkDescriptorPool pool;
        VkDescriptorSet ds = allocSet(pool, fullPSZ, _equirectDSL);
        tempPools.push_back(pool);
        writeFullDS(ds, cbBuf, 16, equirectView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, _envCubemapArray, linearSampler);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _equirectPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _equirectPipeLayout, 0, 1, &ds, 0, nullptr);
        uint32_t g = (ENV_CUBE_SIZE + 7) / 8;
        vkCmdDispatch(cmd, g, g, 6);

        transitionToRead(cmd, _envCubemap, 0, 1, 6);
        endCmd(cmd);
        if (checkDeviceLost("Stage 1")) return false;
        LUNA_LOG_INFO("VulkanIBL: Stage 1 done (equirect → envCube)");
    }

    // Stage 2: Irradiance convolution
    {
        VkCommandBuffer cmd = beginCmd();
        if (!cmd) { vkDestroyCommandPool(dev, localPool, nullptr); vkDestroySampler(dev, linearSampler, nullptr); return false; }

        transitionToGeneral(cmd, _irrCubemap, 0, 1, 6);

        uint32_t cbData[4] = { IRR_CUBE_SIZE, 0, 0, 0 };
        VkBuffer cbBuf; VkDeviceMemory cbMem;
        makeCB(cbData, 16, cbBuf, cbMem);
        tempBufs.push_back(cbBuf); tempMems.push_back(cbMem);

        VkDescriptorPool pool;
        VkDescriptorSet ds = allocSet(pool, fullPSZ, _irrConvDSL);
        tempPools.push_back(pool);
        writeFullDS(ds, cbBuf, 16, _envCubemapView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, _irrCubemapArray, linearSampler);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _irrConvPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _irrConvPipeLayout, 0, 1, &ds, 0, nullptr);
        uint32_t g = (IRR_CUBE_SIZE + 7) / 8;
        vkCmdDispatch(cmd, g, g, 6);

        transitionToRead(cmd, _irrCubemap, 0, 1, 6);
        endCmd(cmd);
        if (checkDeviceLost("Stage 2")) return false;
        LUNA_LOG_INFO("VulkanIBL: Stage 2 done (irradiance convolution)");
    }

    // Stage 3: Prefilter — one mip per submission
    for (uint32_t mip = 0; mip < PREFILTER_MIP_COUNT; mip++)
    {
        VkCommandBuffer cmd = beginCmd();
        if (!cmd) { vkDestroyCommandPool(dev, localPool, nullptr); vkDestroySampler(dev, linearSampler, nullptr); return false; }

        uint32_t mipSize = std::max(1u, PREFILTER_CUBE_SIZE >> mip);
        float roughness = (float)mip / float(PREFILTER_MIP_COUNT - 1u);

        transitionToGeneral(cmd, _prefilterCubemap, mip, 1, 6);

        struct { uint32_t faceSize, mipLevel, numMips; float roughness; } cbData{ mipSize, mip, PREFILTER_MIP_COUNT, roughness };
        VkBuffer cbBuf; VkDeviceMemory cbMem;
        makeCB(&cbData, 16, cbBuf, cbMem);
        tempBufs.push_back(cbBuf); tempMems.push_back(cbMem);

        VkDescriptorPool pool;
        VkDescriptorSet ds = allocSet(pool, fullPSZ, _prefilterDSL);
        tempPools.push_back(pool);
        writeFullDS(ds, cbBuf, 16, _envCubemapView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, _prefilterMipView[mip], linearSampler);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _prefilterPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _prefilterPipeLayout, 0, 1, &ds, 0, nullptr);
        uint32_t g = std::max(1u, (mipSize + 7) / 8);
        vkCmdDispatch(cmd, g, g, 6);

        transitionToRead(cmd, _prefilterCubemap, mip, 1, 6);
        endCmd(cmd);
        if (checkDeviceLost("Stage 3 mip")) return false;
    }
    LUNA_LOG_INFO("VulkanIBL: Stage 3 done (prefilter %u mips)", PREFILTER_MIP_COUNT);

    // Stage 4: BRDF LUT
    {
        VkCommandBuffer cmd = beginCmd();
        if (!cmd) { vkDestroyCommandPool(dev, localPool, nullptr); vkDestroySampler(dev, linearSampler, nullptr); return false; }

        transitionToGeneral(cmd, _brdfLUT, 0, 1, 1);

        VkDescriptorPool pool;
        std::vector<VkDescriptorPoolSize> brdfPSZ = { { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 } };
        VkDescriptorSet ds = allocSet(pool, brdfPSZ, _brdfLutDSL);
        tempPools.push_back(pool);

        VkDescriptorImageInfo dstII{ VK_NULL_HANDLE, _brdfLUTView, VK_IMAGE_LAYOUT_GENERAL };
        VkWriteDescriptorSet ws{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ds, 0, 0, 1,
                                 VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &dstII, nullptr, nullptr };
        vkUpdateDescriptorSets(dev, 1, &ws, 0, nullptr);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _brdfLutPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _brdfLutPipeLayout, 0, 1, &ds, 0, nullptr);
        uint32_t g = (BRDF_LUT_SIZE + 15) / 16;
        vkCmdDispatch(cmd, g, g, 1);

        transitionToRead(cmd, _brdfLUT, 0, 1, 1);
        endCmd(cmd);
        if (checkDeviceLost("Stage 4")) return false;
        LUNA_LOG_INFO("VulkanIBL: Stage 4 done (BRDF LUT)");
    }

    vkDestroySampler(dev, linearSampler, nullptr);

    VkResult idleRes = vkDeviceWaitIdle(dev);
    if (idleRes == VK_ERROR_DEVICE_LOST) {
        _core->SetDeviceLost();
        vkDestroyCommandPool(dev, localPool, nullptr);
        cleanupTemp();
        LUNA_LOG_ERROR("VulkanIBL: device lost during final sync");
        return false;
    }

    // Destroy the local command pool FIRST — this frees all CB objects,
    // clearing the validation layer's resource-reference tracking.
    // Then cleanupTemp destroys staging buffers/descriptor pools safely.
    vkDestroyCommandPool(dev, localPool, nullptr);
    cleanupTemp();

    LUNA_LOG_INFO("VulkanIBL: precompute done (envCube + irrCube + prefilterCube + brdfLUT)");
    return true;
}

} // namespace Luna

