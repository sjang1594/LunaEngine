// VulkanVolumetricFog.cpp — Phase 29: Froxel-based volumetric fog (Vulkan)
#include "LunaPCH.h"
#include "LunaEngine/Renderer/Vulkan/Public/VulkanVolumetricFog.h"
#include "LunaEngine/Renderer/Vulkan/Public/VulkanCore.h"
#include "LunaEngine/Utils/FileSystemUtil.h"
#include "Logger/Logger.h"
#include <vector>
#include <cmath>

namespace Luna
{

// ---------------------------------------------------------------------------
// Shared GLSL → SPIR-V compiler (same pattern as VulkanAtmosphere)
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
        LUNA_LOG_ERROR("VK VolFog: shader compile failed: %s", errMsg.c_str());
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

VulkanVolumetricFog::~VulkanVolumetricFog() { Destroy(); }

bool VulkanVolumetricFog::Create(const CreateInfo& info)
{
    _core           = info.core;
    _framesInFlight = info.framesInFlight;
    _extent         = info.extent;
    _depthView      = info.depthView;
    _hdrView        = info.hdrView;
    _hdrImage       = info.hdrImage;
    _hdrRenderPass  = info.hdrRenderPass;
    _csmShadowView  = info.csmShadowView;

    if (!CreateFroxelVolumes())        { LUNA_LOG_ERROR("VK VolFog: froxel volumes failed");   return false; }
    if (!CreateSampler())              { LUNA_LOG_ERROR("VK VolFog: samplers failed");         return false; }
    if (!CreateUBOs())                 { LUNA_LOG_ERROR("VK VolFog: UBOs failed");             return false; }
    if (!CreateDescriptors())          { LUNA_LOG_ERROR("VK VolFog: descriptors failed");      return false; }
    if (!CreateComputePipelines())     { LUNA_LOG_ERROR("VK VolFog: compute pipelines failed");return false; }
    if (!CreateApplyPipeline())        { LUNA_LOG_ERROR("VK VolFog: apply pipeline failed");   return false; }
    if (!CreateApplyFramebuffer())     { LUNA_LOG_ERROR("VK VolFog: framebuffer failed");      return false; }

    _ready = true;
    LUNA_LOG_INFO("VK VolFog: initialized (%ux%ux%u froxels, ~%.1f MB)",
                  FROXEL_X, FROXEL_Y, FROXEL_Z,
                  2.0f * FROXEL_X * FROXEL_Y * FROXEL_Z * 8 / (1024.0f * 1024.0f));
    return true;
}

void VulkanVolumetricFog::Destroy()
{
    if (!_core) return;
    VkDevice dev = _core->GetDevice();
    if (dev == VK_NULL_HANDLE) { _core = nullptr; return; }
    vkDeviceWaitIdle(dev);

    _ready = false;

    if (_applyFramebuffer)  { vkDestroyFramebuffer(dev, _applyFramebuffer, nullptr);  _applyFramebuffer = VK_NULL_HANDLE; }
    if (_applyPipeline)     { vkDestroyPipeline(dev, _applyPipeline, nullptr);        _applyPipeline    = VK_NULL_HANDLE; }
    if (_applyPipeLayout)   { vkDestroyPipelineLayout(dev, _applyPipeLayout, nullptr);_applyPipeLayout  = VK_NULL_HANDLE; }
    if (_applyDescPool)     { vkDestroyDescriptorPool(dev, _applyDescPool, nullptr);  _applyDescPool    = VK_NULL_HANDLE; }
    if (_applyDescLayout)   { vkDestroyDescriptorSetLayout(dev, _applyDescLayout, nullptr); _applyDescLayout = VK_NULL_HANDLE; }

    if (_scatterPipeline)   { vkDestroyPipeline(dev, _scatterPipeline, nullptr);         _scatterPipeline    = VK_NULL_HANDLE; }
    if (_scatterPipeLayout) { vkDestroyPipelineLayout(dev, _scatterPipeLayout, nullptr);  _scatterPipeLayout  = VK_NULL_HANDLE; }
    if (_scatterDescPool)   { vkDestroyDescriptorPool(dev, _scatterDescPool, nullptr);    _scatterDescPool    = VK_NULL_HANDLE; }
    if (_scatterDescLayout) { vkDestroyDescriptorSetLayout(dev, _scatterDescLayout, nullptr); _scatterDescLayout = VK_NULL_HANDLE; }

    if (_injectPipeline)    { vkDestroyPipeline(dev, _injectPipeline, nullptr);          _injectPipeline    = VK_NULL_HANDLE; }
    if (_injectPipeLayout)  { vkDestroyPipelineLayout(dev, _injectPipeLayout, nullptr);   _injectPipeLayout  = VK_NULL_HANDLE; }
    if (_injectDescPool)    { vkDestroyDescriptorPool(dev, _injectDescPool, nullptr);     _injectDescPool    = VK_NULL_HANDLE; }
    if (_injectDescLayout)  { vkDestroyDescriptorSetLayout(dev, _injectDescLayout, nullptr); _injectDescLayout = VK_NULL_HANDLE; }

    for (uint32_t i = 0; i < _framesInFlight; ++i)
    {
        if (_ubo[i])    { vkDestroyBuffer(dev, _ubo[i], nullptr);       _ubo[i]    = VK_NULL_HANDLE; }
        if (_uboMem[i]) { vkFreeMemory(dev, _uboMem[i], nullptr);       _uboMem[i] = VK_NULL_HANDLE; }
    }

    if (_bilinearClamp) { vkDestroySampler(dev, _bilinearClamp, nullptr); _bilinearClamp = VK_NULL_HANDLE; }
    if (_shadowSampler) { vkDestroySampler(dev, _shadowSampler, nullptr); _shadowSampler = VK_NULL_HANDLE; }
    if (_pointClamp)    { vkDestroySampler(dev, _pointClamp,    nullptr); _pointClamp    = VK_NULL_HANDLE; }

    if (_accumView)  { vkDestroyImageView(dev, _accumView,  nullptr); _accumView  = VK_NULL_HANDLE; }
    if (_accumImage) { vkDestroyImage(dev, _accumImage, nullptr);     _accumImage = VK_NULL_HANDLE; }
    if (_accumMem)   { vkFreeMemory(dev, _accumMem, nullptr);         _accumMem   = VK_NULL_HANDLE; }

    if (_injectView)  { vkDestroyImageView(dev, _injectView,  nullptr); _injectView  = VK_NULL_HANDLE; }
    if (_injectImage) { vkDestroyImage(dev, _injectImage, nullptr);     _injectImage = VK_NULL_HANDLE; }
    if (_injectMem)   { vkFreeMemory(dev, _injectMem, nullptr);         _injectMem   = VK_NULL_HANDLE; }

    _core = nullptr;
}


bool VulkanVolumetricFog::CreateFroxelVolumes()
{
    VkDevice         dev = _core->GetDevice();
    VkPhysicalDevice phd = _core->GetPhysicalDevice();

    auto MakeVol = [&](VkImage& img, VkDeviceMemory& mem, VkImageView& view) -> bool
    {
        VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ci.imageType     = VK_IMAGE_TYPE_3D;
        ci.format        = VK_FORMAT_R16G16B16A16_SFLOAT;
        ci.extent        = { FROXEL_X, FROXEL_Y, FROXEL_Z };
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
        vi.viewType = VK_IMAGE_VIEW_TYPE_3D;
        vi.format   = VK_FORMAT_R16G16B16A16_SFLOAT;
        vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(dev, &vi, nullptr, &view) != VK_SUCCESS) return false;

        return true;
    };

    if (!MakeVol(_injectImage, _injectMem, _injectView)) return false;
    if (!MakeVol(_accumImage,  _accumMem,  _accumView))  return false;

    // Transition both images to GENERAL (compute read/write via storage image)
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
        barriers[0].image = _injectImage;
        barriers[1].image = _accumImage;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 2, barriers);
    }
    _core->EndSingleTimeCommands(cmd);

    _injectLayout = VK_IMAGE_LAYOUT_GENERAL;
    _accumLayout  = VK_IMAGE_LAYOUT_GENERAL;
    return true;
}

bool VulkanVolumetricFog::CreateSampler()
{
    VkDevice dev = _core->GetDevice();

    // Bilinear clamp — apply PS (3D froxel accum)
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

    // Shadow comparison — scatter CS (PCF-filtered CSM lookup)
    {
        VkSamplerCreateInfo ci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        ci.magFilter        = VK_FILTER_LINEAR;
        ci.minFilter        = VK_FILTER_LINEAR;
        ci.mipmapMode       = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        ci.addressModeU     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.addressModeV     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.addressModeW     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.compareEnable    = VK_TRUE;
        ci.compareOp        = VK_COMPARE_OP_LESS_OR_EQUAL;
        if (vkCreateSampler(dev, &ci, nullptr, &_shadowSampler) != VK_SUCCESS) return false;
    }

    // Point clamp — apply PS (depth texture)
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

    return true;
}

bool VulkanVolumetricFog::CreateUBOs()
{
    for (uint32_t i = 0; i < _framesInFlight; ++i)
    {
        if (!_core->CreateBuffer(512,
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                _ubo[i], _uboMem[i])) return false;
        vkMapMemory(_core->GetDevice(), _uboMem[i], 0, 512, 0, &_uboMapped[i]);
    }
    return true;
}

bool VulkanVolumetricFog::CreateDescriptors()
{
    VkDevice dev = _core->GetDevice();
    const uint32_t N = _framesInFlight;

    // ── Inject: b0=UBO, b1=froxelVolume(storage write) ──
    {
        VkDescriptorSetLayoutBinding b[2]{};
        b[0] = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT };
        b[1] = { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  1, VK_SHADER_STAGE_COMPUTE_BIT };
        VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        ci.bindingCount = 2; ci.pBindings = b;
        if (vkCreateDescriptorSetLayout(dev, &ci, nullptr, &_injectDescLayout) != VK_SUCCESS) return false;
    }

    // ── Scatter: b0=UBO, b1=froxelInject(storage read), b2=csmShadow(combined sampler), b3=froxelAccum(storage write) ──
    {
        VkDescriptorSetLayoutBinding b[4]{};
        b[0] = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         1, VK_SHADER_STAGE_COMPUTE_BIT };
        b[1] = { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          1, VK_SHADER_STAGE_COMPUTE_BIT };
        b[2] = { 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT };
        b[3] = { 3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          1, VK_SHADER_STAGE_COMPUTE_BIT };
        VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        ci.bindingCount = 4; ci.pBindings = b;
        if (vkCreateDescriptorSetLayout(dev, &ci, nullptr, &_scatterDescLayout) != VK_SUCCESS) return false;
    }

    // ── Apply: b0=UBO, b1=depthTex(combined sampler), b2=froxelAccum(combined sampler) ──
    {
        VkDescriptorSetLayoutBinding b[3]{};
        b[0] = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         1, VK_SHADER_STAGE_FRAGMENT_BIT };
        b[1] = { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT };
        b[2] = { 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT };
        VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        ci.bindingCount = 3; ci.pBindings = b;
        if (vkCreateDescriptorSetLayout(dev, &ci, nullptr, &_applyDescLayout) != VK_SUCCESS) return false;
    }

    // ── Descriptor pools ──
    auto MakePool = [&](VkDescriptorSetLayout dsl, VkDescriptorPool& pool, uint32_t sets,
                        VkDescriptorPoolSize* sizes, uint32_t sizeCount) -> bool
    {
        VkDescriptorPoolCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        ci.maxSets       = sets;
        ci.poolSizeCount = sizeCount;
        ci.pPoolSizes    = sizes;
        return vkCreateDescriptorPool(dev, &ci, nullptr, &pool) == VK_SUCCESS;
    };

    {
        VkDescriptorPoolSize s[] = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, N },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  N },
        };
        if (!MakePool(_injectDescLayout, _injectDescPool, N, s, 2)) return false;
    }
    {
        VkDescriptorPoolSize s[] = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         N },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          N * 2 },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, N },
        };
        if (!MakePool(_scatterDescLayout, _scatterDescPool, N, s, 3)) return false;
    }
    {
        VkDescriptorPoolSize s[] = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         N },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, N * 2 },
        };
        if (!MakePool(_applyDescLayout, _applyDescPool, N, s, 2)) return false;
    }

    // ── Allocate + write descriptor sets ──
    // Shadow image info — if no CSM available, use accum as a dummy to avoid null view
    VkImageView csmView = (_csmShadowView != VK_NULL_HANDLE) ? _csmShadowView : _accumView;

    for (uint32_t f = 0; f < N; ++f)
    {
        // Inject
        {
            VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ai.descriptorPool = _injectDescPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &_injectDescLayout;
            if (vkAllocateDescriptorSets(dev, &ai, &_injectDescSet[f]) != VK_SUCCESS) return false;

            VkDescriptorBufferInfo bi{ _ubo[f], 0, 512 };
            VkDescriptorImageInfo  ii{ VK_NULL_HANDLE, _injectView, VK_IMAGE_LAYOUT_GENERAL };
            VkWriteDescriptorSet ws[2]{};
            ws[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _injectDescSet[f], 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &bi };
            ws[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _injectDescSet[f], 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  &ii };
            vkUpdateDescriptorSets(dev, 2, ws, 0, nullptr);
        }

        // Scatter
        {
            VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ai.descriptorPool = _scatterDescPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &_scatterDescLayout;
            if (vkAllocateDescriptorSets(dev, &ai, &_scatterDescSet[f]) != VK_SUCCESS) return false;

            VkDescriptorBufferInfo bi   { _ubo[f], 0, 512 };
            VkDescriptorImageInfo  injII{ VK_NULL_HANDLE, _injectView, VK_IMAGE_LAYOUT_GENERAL };
            VkDescriptorImageInfo  csmII{ _shadowSampler, csmView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo  accII{ VK_NULL_HANDLE, _accumView,  VK_IMAGE_LAYOUT_GENERAL };
            VkWriteDescriptorSet ws[4]{};
            ws[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _scatterDescSet[f], 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         nullptr, &bi   };
            ws[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _scatterDescSet[f], 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          &injII        };
            ws[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _scatterDescSet[f], 2, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &csmII        };
            ws[3] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _scatterDescSet[f], 3, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          &accII        };
            vkUpdateDescriptorSets(dev, 4, ws, 0, nullptr);
        }

        // Apply
        {
            VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ai.descriptorPool = _applyDescPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &_applyDescLayout;
            if (vkAllocateDescriptorSets(dev, &ai, &_applyDescSet[f]) != VK_SUCCESS) return false;

            VkDescriptorBufferInfo bi   { _ubo[f], 0, 512 };
            VkDescriptorImageInfo  depII{ _pointClamp,    _depthView,  VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo  volII{ _bilinearClamp, _accumView,  VK_IMAGE_LAYOUT_GENERAL };
            VkWriteDescriptorSet ws[3]{};
            ws[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _applyDescSet[f], 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         nullptr, &bi   };
            ws[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _applyDescSet[f], 1, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depII        };
            ws[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _applyDescSet[f], 2, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &volII        };
            vkUpdateDescriptorSets(dev, 3, ws, 0, nullptr);
        }
    }

    return true;
}

bool VulkanVolumetricFog::CreateComputePipelines()
{
    VkDevice dev = _core->GetDevice();

    auto MakePipeLayout = [&](VkDescriptorSetLayout dsl) -> VkPipelineLayout {
        VkPipelineLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        ci.setLayoutCount = 1; ci.pSetLayouts = &dsl;
        VkPipelineLayout pl = VK_NULL_HANDLE;
        vkCreatePipelineLayout(dev, &ci, nullptr, &pl);
        return pl;
    };

    _injectPipeLayout  = MakePipeLayout(_injectDescLayout);
    _scatterPipeLayout = MakePipeLayout(_scatterDescLayout);

    std::vector<uint32_t> injSpv, scatSpv;
    if (!CompileGLSLtoSPIRV(GetShaderFullPath(L"vol_inject_vk.comp.glsl").wstring(),  injSpv))  return false;
    if (!CompileGLSLtoSPIRV(GetShaderFullPath(L"vol_scatter_vk.comp.glsl").wstring(), scatSpv)) return false;

    _injectPipeline  = MakeComputePipeline(dev, injSpv,  _injectPipeLayout);
    _scatterPipeline = MakeComputePipeline(dev, scatSpv, _scatterPipeLayout);

    return _injectPipeline && _scatterPipeline;
}

bool VulkanVolumetricFog::CreateApplyPipeline()
{
    VkDevice dev = _core->GetDevice();

    {
        VkPipelineLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        ci.setLayoutCount = 1; ci.pSetLayouts = &_applyDescLayout;
        if (vkCreatePipelineLayout(dev, &ci, nullptr, &_applyPipeLayout) != VK_SUCCESS) return false;
    }

    std::vector<uint32_t> vsSpv, fsSpv;
    if (!CompileGLSLtoSPIRV(GetShaderFullPath(L"fullscreen_vk.vert.glsl").wstring(),  vsSpv)) return false;
    if (!CompileGLSLtoSPIRV(GetShaderFullPath(L"vol_apply_vk.frag.glsl").wstring(),   fsSpv)) return false;

    auto MkMod = [&](const std::vector<uint32_t>& spv) -> VkShaderModule {
        VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        ci.codeSize = spv.size() * 4; ci.pCode = spv.data();
        VkShaderModule m = VK_NULL_HANDLE;
        vkCreateShaderModule(dev, &ci, nullptr, &m); return m;
    };
    VkShaderModule vsM = MkMod(vsSpv), fsM = MkMod(fsSpv);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,   vsM, "main" };
    stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fsM, "main" };

    VkPipelineVertexInputStateCreateInfo   vis{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ias{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ias.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo      vps{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vps.viewportCount = 1; vps.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo   mss{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    mss.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo  dss{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    dss.depthTestEnable = VK_FALSE;

    // Additive blend: src ONE, dst ONE
    VkPipelineColorBlendAttachmentState cba{};
    cba.blendEnable         = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.colorBlendOp        = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    cba.alphaBlendOp        = VK_BLEND_OP_ADD;
    cba.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cbs{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cbs.attachmentCount = 1; cbs.pAttachments = &cba;

    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dyn.dynamicStateCount = 2; dyn.pDynamicStates = dynStates;

    VkGraphicsPipelineCreateInfo gpci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gpci.stageCount          = 2;
    gpci.pStages             = stages;
    gpci.pVertexInputState   = &vis;
    gpci.pInputAssemblyState = &ias;
    gpci.pViewportState      = &vps;
    gpci.pRasterizationState = &rs;
    gpci.pMultisampleState   = &mss;
    gpci.pDepthStencilState  = &dss;
    gpci.pColorBlendState    = &cbs;
    gpci.pDynamicState       = &dyn;
    gpci.layout              = _applyPipeLayout;
    gpci.renderPass          = _hdrRenderPass;
    gpci.subpass             = 0;

    VkResult r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, nullptr, &_applyPipeline);
    vkDestroyShaderModule(dev, vsM, nullptr);
    vkDestroyShaderModule(dev, fsM, nullptr);
    return r == VK_SUCCESS;
}

bool VulkanVolumetricFog::CreateApplyFramebuffer()
{
    VkFramebufferCreateInfo ci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    ci.renderPass      = _hdrRenderPass;
    ci.attachmentCount = 1;
    ci.pAttachments    = &_hdrView;
    ci.width           = _extent.width;
    ci.height          = _extent.height;
    ci.layers          = 1;
    return vkCreateFramebuffer(_core->GetDevice(), &ci, nullptr, &_applyFramebuffer) == VK_SUCCESS;
}

// ---------------------------------------------------------------------------
// Per-frame dispatch
// ---------------------------------------------------------------------------

void VulkanVolumetricFog::Dispatch(VkCommandBuffer cmd, uint32_t frameIndex,
                                   const XMFLOAT4X4& view, const XMFLOAT4X4& proj,
                                   const XMFLOAT4X4 csmLightVP[4], const XMFLOAT4& cascadeSplits)
{
    if (!_ready) return;

    // ── Update UBO ──
    VolumetricUBO ubo{};
    XMMATRIX viewM = XMLoadFloat4x4(&view);
    XMMATRIX projM = XMLoadFloat4x4(&proj);
    XMFLOAT4X4 invPf, invVf;
    XMStoreFloat4x4(&invPf, XMMatrixInverse(nullptr, projM));
    XMStoreFloat4x4(&invVf, XMMatrixInverse(nullptr, viewM));
    memcpy(ubo.invProj, &invPf, 64);
    memcpy(ubo.invView, &invVf, 64);
    for (int c = 0; c < 4; ++c) memcpy(ubo.lightVP[c], &csmLightVP[c], 64);
    ubo.cascadeSplits[0] = cascadeSplits.x;
    ubo.cascadeSplits[1] = cascadeSplits.y;
    ubo.cascadeSplits[2] = cascadeSplits.z;
    ubo.cascadeSplits[3] = cascadeSplits.w;
    ubo.lightDir[0]     = lightDir.x; ubo.lightDir[1]     = lightDir.y; ubo.lightDir[2]     = lightDir.z;
    ubo.lightColor[0]   = lightColor.x; ubo.lightColor[1]   = lightColor.y; ubo.lightColor[2]   = lightColor.z;
    ubo.lightIntensity  = lightIntensity;
    ubo.nearZ           = 0.1f;
    ubo.farZ            = 100.0f;
    ubo.screenW         = (float)_extent.width;
    ubo.screenH         = (float)_extent.height;
    ubo.fogDensity      = density;
    ubo.fogHeightFalloff= heightFalloff;
    ubo.fogBaseHeight   = baseHeight;
    ubo.scatteringCoeff = scattering;
    ubo.extinctionCoeff = extinction;
    ubo.phaseG          = phaseG;
    memcpy(_uboMapped[frameIndex], &ubo, sizeof(ubo));

    // ── UAV barrier before inject (ensure no read-after-write from last frame) ──
    VkImageMemoryBarrier preInject{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    preInject.srcAccessMask       = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    preInject.dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
    preInject.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
    preInject.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
    preInject.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    preInject.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    preInject.image               = _injectImage;
    preInject.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &preInject);

    // ── Inject dispatch ──
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _injectPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _injectPipeLayout, 0, 1,
                            &_injectDescSet[frameIndex], 0, nullptr);
    uint32_t gx = (FROXEL_X + 7) / 8;
    uint32_t gy = (FROXEL_Y + 7) / 8;
    uint32_t gz = (FROXEL_Z + 3) / 4;
    vkCmdDispatch(cmd, gx, gy, gz);

    // ── Barrier: inject write → scatter read ──
    VkImageMemoryBarrier inj2Scat{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    inj2Scat.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
    inj2Scat.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
    inj2Scat.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
    inj2Scat.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
    inj2Scat.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    inj2Scat.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    inj2Scat.image               = _injectImage;
    inj2Scat.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &inj2Scat);

    // ── Scatter dispatch ──
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _scatterPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _scatterPipeLayout, 0, 1,
                            &_scatterDescSet[frameIndex], 0, nullptr);
    vkCmdDispatch(cmd, gx, gy, 1);

    // ── Barrier: accum write → fragment read for apply pass ──
    VkImageMemoryBarrier acc2Frag{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    acc2Frag.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
    acc2Frag.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
    acc2Frag.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
    acc2Frag.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
    acc2Frag.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    acc2Frag.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    acc2Frag.image               = _accumImage;
    acc2Frag.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &acc2Frag);
}

void VulkanVolumetricFog::DrawApply(VkCommandBuffer cmd, uint32_t frameIndex)
{
    if (!_ready || _applyFramebuffer == VK_NULL_HANDLE) return;

    VkRenderPassBeginInfo rpbi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rpbi.renderPass        = _hdrRenderPass;
    rpbi.framebuffer       = _applyFramebuffer;
    rpbi.renderArea.extent = _extent;

    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{ 0, 0, (float)_extent.width, (float)_extent.height, 0, 1 };
    VkRect2D   sc{ {0,0}, _extent };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd,  0, 1, &sc);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _applyPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _applyPipeLayout, 0, 1,
                            &_applyDescSet[frameIndex], 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdEndRenderPass(cmd);
}

} // namespace Luna
