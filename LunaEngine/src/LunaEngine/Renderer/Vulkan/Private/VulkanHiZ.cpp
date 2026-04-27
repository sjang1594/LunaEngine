#include "LunaPCH.h"
#include "LunaEngine/Renderer/Vulkan/Public/VulkanHiZ.h"
#include "LunaEngine/Renderer/Vulkan/Public/VulkanCore.h"
#include "LunaEngine/Utils/FileSystemUtil.h"
#include "Logger/Logger.h"

#include <algorithm>

namespace Luna
{

// ===========================================================================
// Local shader compilation helper (duplicated from VulkanBackend.cpp)
// TODO: Extract to shared VulkanShaderUtils
// ===========================================================================
static bool CompileGLSLtoSPIRV(const std::wstring& glslPath, std::vector<uint32_t>& outSpirv)
{
    wchar_t sdkBuf[MAX_PATH] = {};
    DWORD sdkLen = GetEnvironmentVariableW(L"VULKAN_SDK", sdkBuf, MAX_PATH);
    std::wstring glslcPath = (sdkLen > 0)
        ? (std::wstring(sdkBuf) + L"\\Bin\\glslc.exe")
        : L"glslc.exe";

    wchar_t tempDir[MAX_PATH] = {}, tempBase[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempDir);
    GetTempFileNameW(tempDir, L"spv", 0, tempBase);
    DeleteFileW(tempBase);
    std::wstring spvPath = std::wstring(tempBase) + L".spv";

    std::wstring stage;
    if      (glslPath.find(L".vert.")  != std::wstring::npos) stage = L"vertex";
    else if (glslPath.find(L".frag.")  != std::wstring::npos) stage = L"fragment";
    else if (glslPath.find(L".comp.")  != std::wstring::npos) stage = L"compute";
    else if (glslPath.find(L".rgen.")  != std::wstring::npos) stage = L"rgen";
    else if (glslPath.find(L".rmiss.") != std::wstring::npos) stage = L"rmiss";
    else if (glslPath.find(L".rchit.") != std::wstring::npos) stage = L"rchit";
    else { LUNA_LOG_ERROR("VK GLSL: cannot determine stage for %ls", glslPath.c_str()); return false; }

    std::wstring cmd = L"\"" + glslcPath + L"\" --target-env=vulkan1.1 --target-spv=spv1.4"
                     + L" -fshader-stage=" + stage
                     + L" -o \"" + spvPath + L"\" \"" + glslPath + L"\"";

    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE hRead = INVALID_HANDLE_VALUE, hWrite = INVALID_HANDLE_VALUE;
    CreatePipe(&hRead, &hWrite, &sa, 0);
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb         = sizeof(si);
    si.hStdOutput = hWrite;
    si.hStdError  = hWrite;
    si.dwFlags    = STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi{};
    bool started = !!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr,
                                    TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(hWrite);

    if (!started)
    {
        CloseHandle(hRead);
        LUNA_LOG_ERROR("VK GLSL: failed to launch glslc for %ls", glslPath.c_str());
        return false;
    }

    std::string errBuf;
    char buf[512];
    DWORD nRead = 0;
    while (ReadFile(hRead, buf, sizeof(buf) - 1, &nRead, nullptr) && nRead)
    { buf[nRead] = '\0'; errBuf += buf; }
    CloseHandle(hRead);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (!errBuf.empty())
        LUNA_LOG_ERROR("VK GLSL %ls: %s", glslPath.c_str(), errBuf.c_str());
    if (exitCode != 0) { DeleteFileW(spvPath.c_str()); return false; }

    HANDLE hFile = CreateFileW(spvPath.c_str(), GENERIC_READ, 0, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
    { LUNA_LOG_ERROR("VK GLSL: cannot open SPV for %ls", glslPath.c_str()); return false; }

    DWORD fileSize  = GetFileSize(hFile, nullptr);
    DWORD bytesRead = 0;
    outSpirv.resize(fileSize / 4);
    ReadFile(hFile, outSpirv.data(), fileSize, &bytesRead, nullptr);
    CloseHandle(hFile);
    DeleteFileW(spvPath.c_str());

    if (bytesRead != fileSize)
    { LUNA_LOG_ERROR("VK GLSL: incomplete SPV read for %ls", glslPath.c_str()); return false; }

    return true;
}

// ===========================================================================
// Destructor
// ===========================================================================
VulkanHiZ::~VulkanHiZ()
{
    Destroy();
}

// ===========================================================================
// Create
// ===========================================================================
bool VulkanHiZ::Create(const CreateInfo& info)
{
    if (!info.core)
    {
        LUNA_LOG_ERROR("VulkanHiZ: null core");
        return false;
    }
    if (info.extent.width == 0 || info.extent.height == 0) return false;

    _core = info.core;

    // Compute mip count
    uint32_t maxDim = std::max(info.extent.width, info.extent.height);
    _mipCount = 1;
    while (maxDim > 1) { maxDim >>= 1; _mipCount++; }
    _mipCount = std::min(_mipCount, MAX_MIPS);

    if (!CreateImage(info.extent))    { Destroy(); return false; }
    if (!CreateViews())               { Destroy(); return false; }
    if (!CreateSampler())             { Destroy(); return false; }
    if (!CreateDescriptors(info.depthView)) { Destroy(); return false; }
    if (!CreatePipeline())            { Destroy(); return false; }
    if (!CreateParamsUBO())           { Destroy(); return false; }

    LUNA_LOG_INFO("VulkanHiZ: created %ux%u, %u mips",
                  info.extent.width, info.extent.height, _mipCount);
    return true;
}

// ===========================================================================
// Destroy
// ===========================================================================
void VulkanHiZ::Destroy()
{
    if (!_core) return;
    VkDevice dev = _core->GetDevice();
    if (!dev) return;

    _ready = false;

    // Params UBO
    if (_paramsMapped)  { vkUnmapMemory(dev, _paramsMem);                        _paramsMapped = nullptr; }
    if (_paramsBuffer)  { vkDestroyBuffer(dev, _paramsBuffer, nullptr);           _paramsBuffer = VK_NULL_HANDLE; }
    if (_paramsMem)     { vkFreeMemory(dev, _paramsMem, nullptr);                _paramsMem    = VK_NULL_HANDLE; }

    // Pipeline
    if (_pipeline)      { vkDestroyPipeline(dev, _pipeline, nullptr);            _pipeline   = VK_NULL_HANDLE; }
    if (_pipeLayout)    { vkDestroyPipelineLayout(dev, _pipeLayout, nullptr);    _pipeLayout = VK_NULL_HANDLE; }

    // Descriptors
    if (_descPool)      { vkDestroyDescriptorPool(dev, _descPool, nullptr);      _descPool   = VK_NULL_HANDLE; }
    if (_descLayout)    { vkDestroyDescriptorSetLayout(dev, _descLayout, nullptr); _descLayout = VK_NULL_HANDLE; }
    for (uint32_t m = 0; m < MAX_MIPS; ++m) _descSet[m] = VK_NULL_HANDLE;

    // Sampler
    if (_sampler)       { vkDestroySampler(dev, _sampler, nullptr);              _sampler = VK_NULL_HANDLE; }

    // Views
    if (_fullView)      { vkDestroyImageView(dev, _fullView, nullptr);           _fullView = VK_NULL_HANDLE; }
    for (uint32_t m = 0; m < MAX_MIPS; ++m)
    {
        if (_mipView[m]) { vkDestroyImageView(dev, _mipView[m], nullptr); _mipView[m] = VK_NULL_HANDLE; }
    }

    // Image
    if (_image)         { vkDestroyImage(dev, _image, nullptr);                  _image  = VK_NULL_HANDLE; }
    if (_memory)        { vkFreeMemory(dev, _memory, nullptr);                   _memory = VK_NULL_HANDLE; }
    _mipCount = 0;
}

// ===========================================================================
// Resize
// ===========================================================================
bool VulkanHiZ::Resize(VkExtent2D extent, VkImageView depthView)
{
    VulkanCore* core = _core;
    Destroy();

    CreateInfo info;
    info.core      = core;
    info.extent    = extent;
    info.depthView = depthView;
    return Create(info);
}

// ===========================================================================
// BuildPyramid
// ===========================================================================
void VulkanHiZ::BuildPyramid(VkCommandBuffer cmd, VkImage depthImage, VkExtent2D extent)
{
    if (!_image || _mipCount < 2) return;

    // Transition depth → SHADER_READ for compute sampling
    {
        VkImageMemoryBarrier bar{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        bar.srcAccessMask    = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        bar.dstAccessMask    = VK_ACCESS_SHADER_READ_BIT;
        bar.oldLayout        = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        bar.newLayout        = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        bar.image            = depthImage;
        bar.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &bar);
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _pipeline);

    uint32_t srcW = extent.width;
    uint32_t srcH = extent.height;

    // Mip 0: 1:1 copy from depth buffer
    {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _pipeLayout,
            0, 1, &_descSet[0], 0, nullptr);
        uint32_t constants[5] = { srcW, srcH, srcW, srcH, 1 };  // copyMode = 1
        vkCmdPushConstants(cmd, _pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 20, constants);
        vkCmdDispatch(cmd, (srcW + 7) / 8, (srcH + 7) / 8, 1);

        // Barrier: mip 0 write → mip 0 read
        VkImageMemoryBarrier bar{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        bar.srcAccessMask    = VK_ACCESS_SHADER_WRITE_BIT;
        bar.dstAccessMask    = VK_ACCESS_SHADER_READ_BIT;
        bar.oldLayout        = VK_IMAGE_LAYOUT_GENERAL;
        bar.newLayout        = VK_IMAGE_LAYOUT_GENERAL;
        bar.image            = _image;
        bar.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &bar);
    }

    // Transition depth back to ATTACHMENT_OPTIMAL
    {
        VkImageMemoryBarrier bar{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        bar.srcAccessMask    = VK_ACCESS_SHADER_READ_BIT;
        bar.dstAccessMask    = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        bar.oldLayout        = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        bar.newLayout        = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        bar.image            = depthImage;
        bar.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            0, 0, nullptr, 0, nullptr, 1, &bar);
    }

    // Mips 1+: 2×2 min downsample
    for (uint32_t m = 1; m < _mipCount; ++m)
    {
        uint32_t dstW = std::max(srcW >> 1, 1u);
        uint32_t dstH = std::max(srcH >> 1, 1u);

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _pipeLayout,
            0, 1, &_descSet[m], 0, nullptr);

        uint32_t constants[5] = { srcW, srcH, dstW, dstH, 0 };  // copyMode = 0
        vkCmdPushConstants(cmd, _pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 20, constants);
        vkCmdDispatch(cmd, (dstW + 7) / 8, (dstH + 7) / 8, 1);

        // Barrier between mip dispatches
        VkImageMemoryBarrier bar{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        bar.srcAccessMask    = VK_ACCESS_SHADER_WRITE_BIT;
        bar.dstAccessMask    = VK_ACCESS_SHADER_READ_BIT;
        bar.oldLayout        = VK_IMAGE_LAYOUT_GENERAL;
        bar.newLayout        = VK_IMAGE_LAYOUT_GENERAL;
        bar.image            = _image;
        bar.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, m, 1, 0, 1 };
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &bar);

        srcW = dstW;
        srcH = dstH;
    }

    _ready = true;
}

// ===========================================================================
// CreateImage
// ===========================================================================
bool VulkanHiZ::CreateImage(VkExtent2D extent)
{
    VkDevice dev = _core->GetDevice();

    VkImageCreateInfo ii{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType     = VK_IMAGE_TYPE_2D;
    ii.format        = VK_FORMAT_R32_SFLOAT;
    ii.extent        = { extent.width, extent.height, 1 };
    ii.mipLevels     = _mipCount;
    ii.arrayLayers   = 1;
    ii.samples       = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ii.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT
                     | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(dev, &ii, nullptr, &_image) != VK_SUCCESS)
    {
        LUNA_LOG_ERROR("VulkanHiZ: failed to create image");
        return false;
    }

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, _image, &req);
    VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize  = req.size;
    mai.memoryTypeIndex = _core->FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(dev, &mai, nullptr, &_memory) != VK_SUCCESS)
    {
        LUNA_LOG_ERROR("VulkanHiZ: failed to allocate image memory");
        return false;
    }
    vkBindImageMemory(dev, _image, _memory, 0);

    // Transition to GENERAL for storage image writes
    VkCommandBuffer cmd = _core->BeginSingleTimeCommands();
    if (!cmd) return false;

    VkImageMemoryBarrier bar{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    bar.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
    bar.newLayout     = VK_IMAGE_LAYOUT_GENERAL;
    bar.image         = _image;
    bar.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, _mipCount, 0, 1 };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &bar);
    _core->EndSingleTimeCommands(cmd);

    return true;
}

// ===========================================================================
// CreateViews
// ===========================================================================
bool VulkanHiZ::CreateViews()
{
    VkDevice dev = _core->GetDevice();

    // Per-mip views
    for (uint32_t m = 0; m < _mipCount; ++m)
    {
        VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image    = _image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format   = VK_FORMAT_R32_SFLOAT;
        vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, m, 1, 0, 1 };
        if (vkCreateImageView(dev, &vi, nullptr, &_mipView[m]) != VK_SUCCESS)
        {
            LUNA_LOG_ERROR("VulkanHiZ: failed to create mip %u view", m);
            return false;
        }
    }

    // Full-pyramid view (all mips)
    VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image    = _image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format   = VK_FORMAT_R32_SFLOAT;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, _mipCount, 0, 1 };
    if (vkCreateImageView(dev, &vi, nullptr, &_fullView) != VK_SUCCESS)
    {
        LUNA_LOG_ERROR("VulkanHiZ: failed to create full view");
        return false;
    }

    return true;
}

// ===========================================================================
// CreateSampler
// ===========================================================================
bool VulkanHiZ::CreateSampler()
{
    VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter    = VK_FILTER_NEAREST;
    si.minFilter    = VK_FILTER_NEAREST;
    si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod       = (float)_mipCount;

    if (vkCreateSampler(_core->GetDevice(), &si, nullptr, &_sampler) != VK_SUCCESS)
    {
        LUNA_LOG_ERROR("VulkanHiZ: failed to create sampler");
        return false;
    }
    return true;
}

// ===========================================================================
// CreateDescriptors
// ===========================================================================
bool VulkanHiZ::CreateDescriptors(VkImageView depthView)
{
    VkDevice dev = _core->GetDevice();

    // Layout: binding 0 = combined image sampler, binding 1 = storage image
    {
        VkDescriptorSetLayoutBinding bs[2]{};
        bs[0] = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        bs[1] = { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        VkDescriptorSetLayoutCreateInfo li{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        li.bindingCount = 2;
        li.pBindings    = bs;
        if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &_descLayout) != VK_SUCCESS)
        {
            LUNA_LOG_ERROR("VulkanHiZ: failed to create descriptor set layout");
            return false;
        }
    }

    // Pool
    {
        VkDescriptorPoolSize psz[] = {
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, _mipCount },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          _mipCount },
        };
        VkDescriptorPoolCreateInfo pi{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pi.maxSets       = _mipCount;
        pi.poolSizeCount = 2;
        pi.pPoolSizes    = psz;
        if (vkCreateDescriptorPool(dev, &pi, nullptr, &_descPool) != VK_SUCCESS)
        {
            LUNA_LOG_ERROR("VulkanHiZ: failed to create descriptor pool");
            return false;
        }
    }

    // Mip 0: reads from depth, writes to Hi-Z mip 0
    {
        VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool     = _descPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts        = &_descLayout;
        if (vkAllocateDescriptorSets(dev, &ai, &_descSet[0]) != VK_SUCCESS)
        {
            LUNA_LOG_ERROR("VulkanHiZ: failed to allocate descriptor set 0");
            return false;
        }

        VkDescriptorImageInfo srcII{ _sampler, depthView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo dstII{ VK_NULL_HANDLE, _mipView[0], VK_IMAGE_LAYOUT_GENERAL };

        VkWriteDescriptorSet ws[2]{};
        ws[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _descSet[0], 0, 0, 1,
                  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &srcII, nullptr, nullptr };
        ws[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _descSet[0], 1, 0, 1,
                  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &dstII, nullptr, nullptr };
        vkUpdateDescriptorSets(dev, 2, ws, 0, nullptr);
    }

    // Mips 1+: reads from Hi-Z mip N-1, writes to Hi-Z mip N
    for (uint32_t m = 1; m < _mipCount; ++m)
    {
        VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool     = _descPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts        = &_descLayout;
        if (vkAllocateDescriptorSets(dev, &ai, &_descSet[m]) != VK_SUCCESS)
        {
            LUNA_LOG_ERROR("VulkanHiZ: failed to allocate descriptor set %u", m);
            return false;
        }

        VkDescriptorImageInfo srcII{ _sampler, _mipView[m - 1], VK_IMAGE_LAYOUT_GENERAL };
        VkDescriptorImageInfo dstII{ VK_NULL_HANDLE, _mipView[m], VK_IMAGE_LAYOUT_GENERAL };

        VkWriteDescriptorSet ws[2]{};
        ws[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _descSet[m], 0, 0, 1,
                  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &srcII, nullptr, nullptr };
        ws[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _descSet[m], 1, 0, 1,
                  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &dstII, nullptr, nullptr };
        vkUpdateDescriptorSets(dev, 2, ws, 0, nullptr);
    }

    return true;
}

// ===========================================================================
// CreatePipeline
// ===========================================================================
bool VulkanHiZ::CreatePipeline()
{
    VkDevice dev = _core->GetDevice();

    // Pipeline layout with push constants (5 × uint32)
    {
        VkPushConstantRange pcr{};
        pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pcr.offset     = 0;
        pcr.size       = 20;  // srcW, srcH, dstW, dstH, copyMode

        VkPipelineLayoutCreateInfo pli{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pli.setLayoutCount         = 1;
        pli.pSetLayouts            = &_descLayout;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges    = &pcr;
        if (vkCreatePipelineLayout(dev, &pli, nullptr, &_pipeLayout) != VK_SUCCESS)
        {
            LUNA_LOG_ERROR("VulkanHiZ: failed to create pipeline layout");
            return false;
        }
    }

    // Compile shader
    std::vector<uint32_t> csS;
    if (!CompileGLSLtoSPIRV(GetShaderFullPath(L"hiz_generate_vk.comp.glsl").wstring(), csS))
    {
        LUNA_LOG_ERROR("VulkanHiZ: hiz_generate compile failed");
        return false;
    }

    VkShaderModuleCreateInfo smi{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smi.codeSize = csS.size() * 4;
    smi.pCode    = csS.data();
    VkShaderModule csM = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &smi, nullptr, &csM) != VK_SUCCESS)
    {
        LUNA_LOG_ERROR("VulkanHiZ: failed to create shader module");
        return false;
    }

    VkComputePipelineCreateInfo cpi{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    cpi.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = csM;
    cpi.stage.pName  = "main";
    cpi.layout       = _pipeLayout;

    VkResult result = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &_pipeline);
    vkDestroyShaderModule(dev, csM, nullptr);

    if (result != VK_SUCCESS)
    {
        LUNA_LOG_ERROR("VulkanHiZ: failed to create compute pipeline");
        return false;
    }

    return true;
}

// ===========================================================================
// CreateParamsUBO
// ===========================================================================
bool VulkanHiZ::CreateParamsUBO()
{
    if (!_core->CreateBuffer(128, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        _paramsBuffer, _paramsMem))
    {
        LUNA_LOG_ERROR("VulkanHiZ: failed to create params UBO");
        return false;
    }

    if (vkMapMemory(_core->GetDevice(), _paramsMem, 0, 128, 0, &_paramsMapped) != VK_SUCCESS)
    {
        LUNA_LOG_ERROR("VulkanHiZ: failed to map params UBO");
        return false;
    }

    return true;
}

} // namespace Luna

