// VulkanGI.cpp — Phase 30: Screen-Space GI + Irradiance Probes (Vulkan)
#include "LunaPCH.h"
#include "LunaEngine/Renderer/Vulkan/Public/VulkanGI.h"
#include "LunaEngine/Renderer/Vulkan/Public/VulkanCore.h"
#include "LunaEngine/Utils/FileSystemUtil.h"
#include "Logger/Logger.h"
#include <vector>
#include <cmath>

namespace Luna
{

// ---------------------------------------------------------------------------
// Shared GLSL → SPIR-V compiler (same pattern as VulkanVolumetricFog)
// ---------------------------------------------------------------------------
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

    std::wstring shaderDir = glslPath.substr(0, glslPath.find_last_of(L"\\/"));
    std::wstring cmd = L"\"" + glslcPath + L"\" --target-env=vulkan1.1 --target-spv=spv1.4"
                     + L" -fshader-stage=" + stage
                     + L" -I\"" + shaderDir + L"\""
                     + L" -o \"" + spvPath + L"\" \"" + glslPath + L"\"";

    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE hRead = INVALID_HANDLE_VALUE, hWrite = INVALID_HANDLE_VALUE;
    CreatePipe(&hRead, &hWrite, &sa, 0);
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{}; si.cb = sizeof(si); si.hStdOutput = hWrite; si.hStdError = hWrite; si.dwFlags = STARTF_USESTDHANDLES;
    PROCESS_INFORMATION pi{};
    bool started = !!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(hWrite);
    if (!started) { CloseHandle(hRead); return false; }

    char buf[4096]; DWORD nRead = 0;
    std::string errMsg;
    while (ReadFile(hRead, buf, sizeof(buf)-1, &nRead, nullptr) && nRead) { buf[nRead] = 0; errMsg += buf; }
    CloseHandle(hRead);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0; GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    if (exitCode != 0) {
        LUNA_LOG_ERROR("VK GI: shader compile failed: %s", errMsg.c_str());
        DeleteFileW(spvPath.c_str()); return false;
    }

    HANDLE hFile = CreateFileW(spvPath.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    DWORD fileSize = GetFileSize(hFile, nullptr), bytesRead = 0;
    outSpirv.resize(fileSize / 4);
    ReadFile(hFile, outSpirv.data(), fileSize, &bytesRead, nullptr);
    CloseHandle(hFile); DeleteFileW(spvPath.c_str());
    return bytesRead == fileSize;
}

static VkPipeline MakeComputePipeline(VkDevice dev, const std::vector<uint32_t>& spv, VkPipelineLayout layout)
{
    VkShaderModuleCreateInfo smci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smci.codeSize = spv.size() * 4; smci.pCode = spv.data();
    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &smci, nullptr, &mod) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkComputePipelineCreateInfo cpci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    cpci.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main" };
    cpci.layout = layout;
    VkPipeline pipe = VK_NULL_HANDLE;
    vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipe);
    vkDestroyShaderModule(dev, mod, nullptr);
    return pipe;
}

// ---------------------------------------------------------------------------

VulkanGI::~VulkanGI() { Destroy(); }

bool VulkanGI::Create(const CreateInfo& info)
{
    _core           = info.core;
    _framesInFlight = info.framesInFlight;
    _extent         = info.extent;
    _depthView      = info.depthView;
    _gbufAlbedoView = info.gbufAlbedoView;
    _gbufNormalView = info.gbufNormalView;
    _hdrView        = info.hdrView;
    _hdrImage       = info.hdrImage;
    _hiZView        = info.hiZView;
    _hiZSampler     = info.hiZSampler;
    _irrCubeView    = info.irrCubeView;
    _iblSampler     = info.iblSampler;

    if (!CreateSSGIImages())    { LUNA_LOG_ERROR("VK GI: SSGI images failed");        return false; }
    if (!CreateProbeAtlas())    { LUNA_LOG_ERROR("VK GI: probe atlas failed");         return false; }
    if (!CreateSamplers())      { LUNA_LOG_ERROR("VK GI: samplers failed");            return false; }
    if (!CreateUBOs())          { LUNA_LOG_ERROR("VK GI: UBOs failed");               return false; }
    if (!CreateDescriptors())   { LUNA_LOG_ERROR("VK GI: descriptors failed");         return false; }
    if (!CreateSSGIPipeline())  { LUNA_LOG_ERROR("VK GI: SSGI pipeline failed");       return false; }
    if (!CreateProbePipeline()) { LUNA_LOG_ERROR("VK GI: probe pipeline failed");      return false; }

    _ready = true;
    LUNA_LOG_INFO("VK GI: SSGI + probe system active");
    return true;
}

void VulkanGI::Destroy()
{
    if (!_core) return;
    VkDevice dev = _core->GetDevice();
    if (dev == VK_NULL_HANDLE) { _core = nullptr; return; }
    vkDeviceWaitIdle(dev);

    _ready = false;

    if (_probePipeline)    { vkDestroyPipeline(dev, _probePipeline, nullptr);           _probePipeline    = VK_NULL_HANDLE; }
    if (_probePipeLayout)  { vkDestroyPipelineLayout(dev, _probePipeLayout, nullptr);   _probePipeLayout  = VK_NULL_HANDLE; }
    if (_probeDescPool)    { vkDestroyDescriptorPool(dev, _probeDescPool, nullptr);     _probeDescPool    = VK_NULL_HANDLE; }
    if (_probeDescLayout)  { vkDestroyDescriptorSetLayout(dev, _probeDescLayout, nullptr); _probeDescLayout = VK_NULL_HANDLE; }

    if (_ssgiPipeline)     { vkDestroyPipeline(dev, _ssgiPipeline, nullptr);            _ssgiPipeline    = VK_NULL_HANDLE; }
    if (_ssgiPipeLayout)   { vkDestroyPipelineLayout(dev, _ssgiPipeLayout, nullptr);    _ssgiPipeLayout  = VK_NULL_HANDLE; }
    if (_ssgiDescPool)     { vkDestroyDescriptorPool(dev, _ssgiDescPool, nullptr);      _ssgiDescPool    = VK_NULL_HANDLE; }
    if (_ssgiDescLayout)   { vkDestroyDescriptorSetLayout(dev, _ssgiDescLayout, nullptr); _ssgiDescLayout = VK_NULL_HANDLE; }

    for (uint32_t i = 0; i < _framesInFlight; ++i)
    {
        if (_probeUBO[i])    { vkDestroyBuffer(dev, _probeUBO[i], nullptr);    _probeUBO[i]    = VK_NULL_HANDLE; }
        if (_probeUBOMem[i]) { vkFreeMemory(dev, _probeUBOMem[i], nullptr);    _probeUBOMem[i] = VK_NULL_HANDLE; }
        if (_ssgiUBO[i])     { vkDestroyBuffer(dev, _ssgiUBO[i], nullptr);     _ssgiUBO[i]     = VK_NULL_HANDLE; }
        if (_ssgiUBOMem[i])  { vkFreeMemory(dev, _ssgiUBOMem[i], nullptr);     _ssgiUBOMem[i]  = VK_NULL_HANDLE; }
    }

    if (_bilinearClamp) { vkDestroySampler(dev, _bilinearClamp, nullptr); _bilinearClamp = VK_NULL_HANDLE; }
    if (_pointClamp)    { vkDestroySampler(dev, _pointClamp,    nullptr); _pointClamp    = VK_NULL_HANDLE; }

    if (_probeIrrView)  { vkDestroyImageView(dev, _probeIrrView, nullptr);  _probeIrrView  = VK_NULL_HANDLE; }
    if (_probeIrrImage) { vkDestroyImage(dev, _probeIrrImage, nullptr);     _probeIrrImage = VK_NULL_HANDLE; }
    if (_probeIrrMem)   { vkFreeMemory(dev, _probeIrrMem, nullptr);         _probeIrrMem   = VK_NULL_HANDLE; }

    for (int i = 0; i < 2; ++i)
    {
        if (_ssgiView[i])  { vkDestroyImageView(dev, _ssgiView[i], nullptr);  _ssgiView[i]  = VK_NULL_HANDLE; }
        if (_ssgiImage[i]) { vkDestroyImage(dev, _ssgiImage[i], nullptr);     _ssgiImage[i] = VK_NULL_HANDLE; }
        if (_ssgiMem[i])   { vkFreeMemory(dev, _ssgiMem[i], nullptr);         _ssgiMem[i]   = VK_NULL_HANDLE; }
    }

    _core = nullptr;
}

VkImageView VulkanGI::GetSSGIReadView() const
{
    // Return the last-written buffer (history = opposite of current write target)
    return _ssgiView[1 - _pingPong];
}

// ---------------------------------------------------------------------------
// Resource creation
// ---------------------------------------------------------------------------

bool VulkanGI::CreateSSGIImages()
{
    VkDevice         dev = _core->GetDevice();
    VkPhysicalDevice phd = _core->GetPhysicalDevice();

    uint32_t halfW = (_extent.width  + 1) / 2;
    uint32_t halfH = (_extent.height + 1) / 2;

    auto MakeHalfRes = [&](VkImage& img, VkDeviceMemory& mem, VkImageView& view) -> bool
    {
        VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ci.imageType     = VK_IMAGE_TYPE_2D;
        ci.format        = VK_FORMAT_R16G16B16A16_SFLOAT;
        ci.extent        = { halfW, halfH, 1 };
        ci.mipLevels     = 1;
        ci.arrayLayers   = 1;
        ci.samples       = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ci.usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(dev, &ci, nullptr, &img) != VK_SUCCESS) return false;

        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(dev, img, &mr);

        VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        ai.allocationSize  = mr.size;
        ai.memoryTypeIndex = _core->FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(dev, &ai, nullptr, &mem) != VK_SUCCESS) return false;
        vkBindImageMemory(dev, img, mem, 0);

        VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image    = img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format   = VK_FORMAT_R16G16B16A16_SFLOAT;
        vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(dev, &vi, nullptr, &view) != VK_SUCCESS) return false;

        return true;
    };

    if (!MakeHalfRes(_ssgiImage[0], _ssgiMem[0], _ssgiView[0])) return false;
    if (!MakeHalfRes(_ssgiImage[1], _ssgiMem[1], _ssgiView[1])) return false;

    // Transition both to GENERAL
    VkCommandBuffer cmd = _core->BeginSingleTimeCommands();
    {
        VkImageMemoryBarrier barriers[2]{};
        for (auto& b : barriers)
        {
            b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.srcAccessMask       = 0;
            b.dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
            b.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        }
        barriers[0].image = _ssgiImage[0];
        barriers[1].image = _ssgiImage[1];
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 2, barriers);
    }
    _core->EndSingleTimeCommands(cmd);

    _ssgiLayout[0] = VK_IMAGE_LAYOUT_GENERAL;
    _ssgiLayout[1] = VK_IMAGE_LAYOUT_GENERAL;
    return true;
}

bool VulkanGI::CreateProbeAtlas()
{
    VkDevice dev = _core->GetDevice();

    VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.imageType     = VK_IMAGE_TYPE_2D;
    ci.format        = VK_FORMAT_R16G16B16A16_SFLOAT;
    ci.extent        = { PROBE_ATLAS_W, PROBE_ATLAS_H, 1 };
    ci.mipLevels     = 1;
    ci.arrayLayers   = 8;
    ci.samples       = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ci.usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev, &ci, nullptr, &_probeIrrImage) != VK_SUCCESS) return false;

    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev, _probeIrrImage, &mr);

    VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize  = mr.size;
    ai.memoryTypeIndex = _core->FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(dev, &ai, nullptr, &_probeIrrMem) != VK_SUCCESS) return false;
    vkBindImageMemory(dev, _probeIrrImage, _probeIrrMem, 0);

    VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image    = _probeIrrImage;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vi.format   = VK_FORMAT_R16G16B16A16_SFLOAT;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 8 };
    if (vkCreateImageView(dev, &vi, nullptr, &_probeIrrView) != VK_SUCCESS) return false;

    // Transition to GENERAL
    VkCommandBuffer cmd = _core->BeginSingleTimeCommands();
    {
        VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.srcAccessMask       = 0;
        b.dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        b.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image               = _probeIrrImage;
        b.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 8 };
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);
    }
    _core->EndSingleTimeCommands(cmd);

    _probeIrrLayout = VK_IMAGE_LAYOUT_GENERAL;
    return true;
}

bool VulkanGI::CreateSamplers()
{
    VkDevice dev = _core->GetDevice();

    // Point clamp — depth reads
    {
        VkSamplerCreateInfo ci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        ci.magFilter    = VK_FILTER_NEAREST;
        ci.minFilter    = VK_FILTER_NEAREST;
        ci.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(dev, &ci, nullptr, &_pointClamp) != VK_SUCCESS) return false;
    }

    // Bilinear clamp — SSGI history + GBuffer reads
    {
        VkSamplerCreateInfo ci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        ci.magFilter    = VK_FILTER_LINEAR;
        ci.minFilter    = VK_FILTER_LINEAR;
        ci.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(dev, &ci, nullptr, &_bilinearClamp) != VK_SUCCESS) return false;
    }

    return true;
}

bool VulkanGI::CreateUBOs()
{
    for (uint32_t i = 0; i < _framesInFlight; ++i)
    {
        if (!_core->CreateBuffer(256,
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                _ssgiUBO[i], _ssgiUBOMem[i])) return false;
        vkMapMemory(_core->GetDevice(), _ssgiUBOMem[i], 0, 256, 0, &_ssgiUBOMapped[i]);

        if (!_core->CreateBuffer(256,
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                _probeUBO[i], _probeUBOMem[i])) return false;
        vkMapMemory(_core->GetDevice(), _probeUBOMem[i], 0, 256, 0, &_probeUBOMapped[i]);
    }
    return true;
}

bool VulkanGI::CreateDescriptors()
{
    VkDevice dev = _core->GetDevice();
    const uint32_t N = _framesInFlight;

    // ── SSGI layout: b0=UBO, b1=depth, b2=gbufAlbedo, b3=gbufNormal,
    //                 b4=hdrTex, b5=hiZTex, b6=ssgiHistory, b7=ssgiOutput(storage) ──
    {
        VkDescriptorSetLayoutBinding b[8]{};
        b[0] = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         1, VK_SHADER_STAGE_COMPUTE_BIT };
        b[1] = { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT };
        b[2] = { 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT };
        b[3] = { 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT };
        b[4] = { 4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT };
        b[5] = { 5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT };
        b[6] = { 6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT };
        b[7] = { 7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          1, VK_SHADER_STAGE_COMPUTE_BIT };
        VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        ci.bindingCount = 8; ci.pBindings = b;
        if (vkCreateDescriptorSetLayout(dev, &ci, nullptr, &_ssgiDescLayout) != VK_SUCCESS) return false;
    }

    // ── Probe layout: b0=probeUBO, b1=ssgiTex, b2=irrCubemap, b3=depth, b4=probeIrrArray(storage) ──
    {
        VkDescriptorSetLayoutBinding b[5]{};
        b[0] = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         1, VK_SHADER_STAGE_COMPUTE_BIT };
        b[1] = { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT };
        b[2] = { 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT };
        b[3] = { 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT };
        b[4] = { 4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          1, VK_SHADER_STAGE_COMPUTE_BIT };
        VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        ci.bindingCount = 5; ci.pBindings = b;
        if (vkCreateDescriptorSetLayout(dev, &ci, nullptr, &_probeDescLayout) != VK_SUCCESS) return false;
    }

    // ── Descriptor pools ──
    // SSGI pool: 2 sets per frame (one per pingPong)
    {
        VkDescriptorPoolSize s[] = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         N * 2 },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, N * 2 * 6 },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          N * 2 },
        };
        VkDescriptorPoolCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        ci.maxSets       = N * 2;
        ci.poolSizeCount = 3;
        ci.pPoolSizes    = s;
        if (vkCreateDescriptorPool(dev, &ci, nullptr, &_ssgiDescPool) != VK_SUCCESS) return false;
    }

    // Probe pool: 1 set per frame
    {
        VkDescriptorPoolSize s[] = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         N },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, N * 3 },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          N },
        };
        VkDescriptorPoolCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        ci.maxSets       = N;
        ci.poolSizeCount = 3;
        ci.pPoolSizes    = s;
        if (vkCreateDescriptorPool(dev, &ci, nullptr, &_probeDescPool) != VK_SUCCESS) return false;
    }

    // ── Allocate + write descriptor sets ──
    // Fall back to a valid view if optional inputs are missing
    VkImageView hiZFallback  = (_hiZView     != VK_NULL_HANDLE) ? _hiZView     : _ssgiView[0];
    VkSampler   hiZSampFb    = (_hiZSampler  != VK_NULL_HANDLE) ? _hiZSampler  : _pointClamp;
    VkImageView irrCubeFb    = (_irrCubeView != VK_NULL_HANDLE) ? _irrCubeView : _ssgiView[0];
    VkSampler   iblSampFb    = (_iblSampler  != VK_NULL_HANDLE) ? _iblSampler  : _bilinearClamp;

    for (uint32_t f = 0; f < N; ++f)
    {
        // SSGI — two sets per frame (pingPong = 0 and pingPong = 1)
        for (int pp = 0; pp < 2; ++pp)
        {
            int histIdx  = 1 - pp;  // read from opposite buffer
            int writeIdx = pp;      // write to current buffer

            VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ai.descriptorPool     = _ssgiDescPool;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts        = &_ssgiDescLayout;
            if (vkAllocateDescriptorSets(dev, &ai, &_ssgiDescSet[f][pp]) != VK_SUCCESS) return false;

            VkDescriptorBufferInfo uboBI{ _ssgiUBO[f], 0, 256 };
            VkDescriptorImageInfo  depII  { _pointClamp,    _depthView,      VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo  albII  { _bilinearClamp, _gbufAlbedoView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo  norII  { _bilinearClamp, _gbufNormalView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo  hdrII  { _bilinearClamp, _hdrView,        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo  hiZII  { hiZSampFb,      hiZFallback,     VK_IMAGE_LAYOUT_GENERAL };
            VkDescriptorImageInfo  histII { _bilinearClamp, _ssgiView[histIdx],  VK_IMAGE_LAYOUT_GENERAL };
            VkDescriptorImageInfo  outII  { VK_NULL_HANDLE, _ssgiView[writeIdx], VK_IMAGE_LAYOUT_GENERAL };

            VkWriteDescriptorSet ws[8]{};
            ws[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _ssgiDescSet[f][pp], 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         nullptr, &uboBI };
            ws[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _ssgiDescSet[f][pp], 1, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depII        };
            ws[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _ssgiDescSet[f][pp], 2, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &albII        };
            ws[3] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _ssgiDescSet[f][pp], 3, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &norII        };
            ws[4] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _ssgiDescSet[f][pp], 4, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &hdrII        };
            ws[5] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _ssgiDescSet[f][pp], 5, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &hiZII        };
            ws[6] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _ssgiDescSet[f][pp], 6, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &histII       };
            ws[7] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _ssgiDescSet[f][pp], 7, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          &outII        };
            vkUpdateDescriptorSets(dev, 8, ws, 0, nullptr);
        }

        // Probe
        {
            VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ai.descriptorPool     = _probeDescPool;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts        = &_probeDescLayout;
            if (vkAllocateDescriptorSets(dev, &ai, &_probeDescSet[f]) != VK_SUCCESS) return false;

            VkDescriptorBufferInfo uboBI  { _probeUBO[f], 0, 256 };
            // Use ssgiView[0] as initial SSGI read source; Dispatch() uses the correct pingPong face
            VkDescriptorImageInfo  ssgiII { _bilinearClamp, _ssgiView[0],   VK_IMAGE_LAYOUT_GENERAL };
            VkDescriptorImageInfo  irrII  { iblSampFb,      irrCubeFb,      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo  depII  { _pointClamp,    _depthView,      VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo  probeOutII{ VK_NULL_HANDLE, _probeIrrView, VK_IMAGE_LAYOUT_GENERAL };

            VkWriteDescriptorSet ws[5]{};
            ws[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _probeDescSet[f], 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         nullptr, &uboBI    };
            ws[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _probeDescSet[f], 1, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ssgiII           };
            ws[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _probeDescSet[f], 2, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &irrII            };
            ws[3] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _probeDescSet[f], 3, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depII            };
            ws[4] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _probeDescSet[f], 4, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          &probeOutII       };
            vkUpdateDescriptorSets(dev, 5, ws, 0, nullptr);
        }
    }

    return true;
}

bool VulkanGI::CreateSSGIPipeline()
{
    VkDevice dev = _core->GetDevice();

    {
        VkPipelineLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        ci.setLayoutCount = 1; ci.pSetLayouts = &_ssgiDescLayout;
        if (vkCreatePipelineLayout(dev, &ci, nullptr, &_ssgiPipeLayout) != VK_SUCCESS) return false;
    }

    std::vector<uint32_t> spv;
    if (!CompileGLSLtoSPIRV(GetShaderFullPath(L"ssgi_vk.comp.glsl").wstring(), spv)) return false;

    _ssgiPipeline = MakeComputePipeline(dev, spv, _ssgiPipeLayout);
    return _ssgiPipeline != VK_NULL_HANDLE;
}

bool VulkanGI::CreateProbePipeline()
{
    VkDevice dev = _core->GetDevice();

    {
        VkPipelineLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        ci.setLayoutCount = 1; ci.pSetLayouts = &_probeDescLayout;
        if (vkCreatePipelineLayout(dev, &ci, nullptr, &_probePipeLayout) != VK_SUCCESS) return false;
    }

    std::vector<uint32_t> spv;
    if (!CompileGLSLtoSPIRV(GetShaderFullPath(L"probe_update_vk.comp.glsl").wstring(), spv)) return false;

    _probePipeline = MakeComputePipeline(dev, spv, _probePipeLayout);
    return _probePipeline != VK_NULL_HANDLE;
}

// ---------------------------------------------------------------------------
// Per-frame dispatch
// ---------------------------------------------------------------------------

void VulkanGI::Dispatch(VkCommandBuffer cmd, uint32_t frameIndex,
                        const XMFLOAT4X4& invVP, const XMFLOAT4X4& prevVP, const XMFLOAT4X4& view)
{
    if (!_ready) return;

    uint32_t halfW = (_extent.width  + 1) / 2;
    uint32_t halfH = (_extent.height + 1) / 2;

    // ── Upload SSGI UBO ──
    {
        SSGIUBO ubo{};
        memcpy(ubo.invViewProj, &invVP,  64);
        memcpy(ubo.prevViewProj, &prevVP, 64);
        memcpy(ubo.view,         &view,   64);
        ubo.screenSize[0] = (float)_extent.width;
        ubo.screenSize[1] = (float)_extent.height;
        ubo.halfResSize[0] = (float)halfW;
        ubo.halfResSize[1] = (float)halfH;
        ubo.frameCount    = _frameCount;
        ubo.numRays       = numSSGIRays;
        ubo.maxRayDist    = maxRayDist;
        ubo.temporalAlpha = temporalAlpha;
        memcpy(_ssgiUBOMapped[frameIndex], &ubo, sizeof(ubo));
    }

    // ── Upload Probe UBO ──
    {
        ProbeUBO ubo{};
        ubo.origin[0] = probeOrigin[0]; ubo.origin[1] = probeOrigin[1];
        ubo.origin[2] = probeOrigin[2]; ubo.origin[3] = 0.0f;
        ubo.spacing[0] = probeSpacing[0]; ubo.spacing[1] = probeSpacing[1];
        ubo.spacing[2] = probeSpacing[2]; ubo.spacing[3] = 0.0f;
        ubo.dims[0]    = PROBE_GRID_X;
        ubo.dims[1]    = PROBE_GRID_Y;
        ubo.dims[2]    = PROBE_GRID_Z;
        ubo.screenSize[0] = (float)_extent.width;
        ubo.screenSize[1] = (float)_extent.height;
        memcpy(ubo.invViewProj, &invVP, 64);
        ubo.probeIndex = _probeIdx % PROBE_COUNT;
        memcpy(_probeUBOMapped[frameIndex], &ubo, sizeof(ubo));
    }

    // ── Execution barrier on SSGI history before dispatch ──
    {
        int histIdx = 1 - _pingPong;
        VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        b.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        b.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
        b.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image               = _ssgiImage[histIdx];
        b.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);
    }

    // ── SSGI dispatch ──
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _ssgiPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _ssgiPipeLayout, 0, 1,
                            &_ssgiDescSet[frameIndex][_pingPong], 0, nullptr);
    uint32_t gx = (halfW + 7) / 8;
    uint32_t gy = (halfH + 7) / 8;
    vkCmdDispatch(cmd, gx, gy, 1);

    // ── Barrier: SSGI write → probe read ──
    {
        VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        b.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
        b.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image               = _ssgiImage[_pingPong];
        b.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);
    }

    // ── Probe update dispatch (one probe per frame) ──
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _probePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _probePipeLayout, 0, 1,
                            &_probeDescSet[frameIndex], 0, nullptr);
    vkCmdDispatch(cmd, 1, 1, 1);

    // ── Barrier: probe atlas write → fragment shader read ──
    {
        VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        b.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
        b.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image               = _probeIrrImage;
        b.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 8 };
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);
    }

    _pingPong ^= 1;
    _frameCount++;
    _probeIdx++;
}

} // namespace Luna
