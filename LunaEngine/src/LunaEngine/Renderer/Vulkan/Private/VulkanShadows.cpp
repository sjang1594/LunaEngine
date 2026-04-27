#include "LunaPCH.h"
#include "LunaEngine/Renderer/Vulkan/Public/VulkanShadows.h"
#include "LunaEngine/Renderer/Vulkan/Public/VulkanCore.h"
#include "LunaEngine/Utils/FileSystemUtil.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <cmath>

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
VulkanShadows::~VulkanShadows()
{
    Destroy();
}

// ===========================================================================
// Create
// ===========================================================================
bool VulkanShadows::Create(const CreateInfo& info)
{
    if (!info.core) { LUNA_LOG_ERROR("VulkanShadows: null core"); return false; }
    _core = info.core;

    if (!CreateImage())        { Destroy(); return false; }
    if (!CreateViews())        { Destroy(); return false; }
    if (!CreateRenderPass())   { Destroy(); return false; }
    if (!CreateFramebuffers()) { Destroy(); return false; }
    if (!CreateSampler())      { Destroy(); return false; }
    if (!CreatePipeline())     { Destroy(); return false; }

    // Transition to DEPTH_STENCIL_READ_ONLY so first frame deferred read is valid
    VkCommandBuffer cmd = _core->BeginSingleTimeCommands();
    if (cmd)
    {
        VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout           = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image               = _image;
        b.subresourceRange    = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, CASCADE_COUNT };
        b.srcAccessMask       = 0;
        b.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);
        _core->EndSingleTimeCommands(cmd);
    }

    LUNA_LOG_INFO("VulkanShadows: created (%u×%u × %u cascades)", SHADOW_SIZE, SHADOW_SIZE, CASCADE_COUNT);
    return true;
}

// ===========================================================================
// Destroy
// ===========================================================================
void VulkanShadows::Destroy()
{
    if (!_core) return;
    VkDevice dev = _core->GetDevice();
    if (!dev) return;

    if (_pipeline)   { vkDestroyPipeline(dev, _pipeline, nullptr);              _pipeline   = VK_NULL_HANDLE; }
    if (_pipeLayout) { vkDestroyPipelineLayout(dev, _pipeLayout, nullptr);      _pipeLayout = VK_NULL_HANDLE; }
    for (uint32_t i = 0; i < CASCADE_COUNT; i++) {
        if (_framebuffers[i]) { vkDestroyFramebuffer(dev, _framebuffers[i], nullptr); _framebuffers[i] = VK_NULL_HANDLE; }
        if (_layerView[i])    { vkDestroyImageView(dev, _layerView[i], nullptr);      _layerView[i]    = VK_NULL_HANDLE; }
    }
    if (_arrayView)  { vkDestroyImageView(dev, _arrayView, nullptr);            _arrayView  = VK_NULL_HANDLE; }
    if (_renderPass) { vkDestroyRenderPass(dev, _renderPass, nullptr);          _renderPass = VK_NULL_HANDLE; }
    if (_image)      { vkDestroyImage(dev, _image, nullptr);                    _image      = VK_NULL_HANDLE; }
    if (_memory)     { vkFreeMemory(dev, _memory, nullptr);                     _memory     = VK_NULL_HANDLE; }
    if (_sampler)    { vkDestroySampler(dev, _sampler, nullptr);                _sampler    = VK_NULL_HANDLE; }
}

// ===========================================================================
// UpdateMatrices — practical split scheme + orthographic light VP
// ===========================================================================
void VulkanShadows::UpdateMatrices(const XMFLOAT4X4& view, const XMFLOAT4X4& proj)
{
    const float nearZ  = 0.1f;
    const float farZ   = 100.0f;
    const float lambda = 0.5f;
    const float ratio  = farZ / nearZ;

    float splits[CASCADE_COUNT];
    for (uint32_t i = 0; i < CASCADE_COUNT; ++i)
    {
        float p       = (i + 1) / (float)CASCADE_COUNT;
        float logSpl  = nearZ * std::pow(ratio, p);
        float unifSpl = nearZ + (farZ - nearZ) * p;
        splits[i]     = lambda * (logSpl - unifSpl) + unifSpl;
    }
    for (uint32_t i = 0; i < CASCADE_COUNT; i++) _splits[i] = splits[i];

    XMVECTOR lightDirV = XMVector3Normalize(XMVectorSet(1.0f, 2.0f, 1.0f, 0.0f));
    XMMATRIX viewMat = XMLoadFloat4x4(&view);
    XMMATRIX projMat = XMLoadFloat4x4(&proj);
    XMMATRIX invView = XMMatrixInverse(nullptr, viewMat);

    float tanHalfFovX = 1.0f / XMVectorGetX(projMat.r[0]);
    float tanHalfFovY = 1.0f / XMVectorGetY(projMat.r[1]);

    float lastSplit = nearZ;
    for (uint32_t cascade = 0; cascade < CASCADE_COUNT; ++cascade)
    {
        float zN = lastSplit;
        float zF = splits[cascade];

        XMFLOAT3 vsCorners[8] = {
            {-tanHalfFovX * zN,  tanHalfFovY * zN, zN},
            { tanHalfFovX * zN,  tanHalfFovY * zN, zN},
            { tanHalfFovX * zN, -tanHalfFovY * zN, zN},
            {-tanHalfFovX * zN, -tanHalfFovY * zN, zN},
            {-tanHalfFovX * zF,  tanHalfFovY * zF, zF},
            { tanHalfFovX * zF,  tanHalfFovY * zF, zF},
            { tanHalfFovX * zF, -tanHalfFovY * zF, zF},
            {-tanHalfFovX * zF, -tanHalfFovY * zF, zF},
        };

        XMVECTOR cornersWS[8];
        XMVECTOR center = XMVectorZero();
        for (uint32_t j = 0; j < 8; ++j) {
            XMVECTOR vs = XMVectorSet(vsCorners[j].x, vsCorners[j].y, vsCorners[j].z, 1.0f);
            cornersWS[j] = XMVector4Transform(vs, invView);
            center = XMVectorAdd(center, cornersWS[j]);
        }
        center = XMVectorScale(center, 1.0f / 8.0f);

        XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        if (std::abs(XMVectorGetY(lightDirV)) > 0.99f)
            up = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
        XMVECTOR eye    = XMVectorSubtract(center, lightDirV);
        XMMATRIX lightV = XMMatrixLookAtLH(eye, center, up);

        XMVECTOR minLS = XMVectorReplicate(FLT_MAX);
        XMVECTOR maxLS = XMVectorReplicate(-FLT_MAX);
        for (uint32_t j = 0; j < 8; ++j) {
            XMVECTOR ls = XMVector4Transform(cornersWS[j], lightV);
            minLS = XMVectorMin(minLS, ls);
            maxLS = XMVectorMax(maxLS, ls);
        }

        float minX = XMVectorGetX(minLS), maxX = XMVectorGetX(maxLS);
        float minY = XMVectorGetY(minLS), maxY = XMVectorGetY(maxLS);
        float minZ = XMVectorGetZ(minLS), maxZ = XMVectorGetZ(maxLS);
        float zExtent = maxZ - minZ;
        minZ -= zExtent * 0.5f;

        XMMATRIX lightP  = XMMatrixOrthographicOffCenterLH(minX, maxX, minY, maxY, minZ, maxZ);
        XMMATRIX lightVP = XMMatrixMultiply(lightV, lightP);
        XMStoreFloat4x4(&_lightVP[cascade], lightVP);

        lastSplit = zF;
    }
}

// ===========================================================================
// DrawPass — render all meshes into the 4-cascade shadow map
// ===========================================================================
void VulkanShadows::DrawPass(VkCommandBuffer cmd, const std::vector<ShadowDraw>& draws)
{
    if (!_pipeline || draws.empty()) return;

    for (uint32_t cascade = 0; cascade < CASCADE_COUNT; ++cascade)
    {
        VkClearValue clear{};
        clear.depthStencil = { 1.0f, 0 };

        VkRenderPassBeginInfo rpi{};
        rpi.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpi.renderPass        = _renderPass;
        rpi.framebuffer       = _framebuffers[cascade];
        rpi.renderArea.extent = { SHADOW_SIZE, SHADOW_SIZE };
        rpi.clearValueCount   = 1;
        rpi.pClearValues      = &clear;
        vkCmdBeginRenderPass(cmd, &rpi, VK_SUBPASS_CONTENTS_INLINE);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _pipeline);

        XMMATRIX lvp = XMLoadFloat4x4(&_lightVP[cascade]);

        for (const auto& draw : draws)
        {
            XMMATRIX model = XMLoadFloat4x4(&draw.model);
            XMMATRIX lmvp  = XMMatrixMultiply(model, lvp);
            XMFLOAT4X4 lmvpf;
            XMStoreFloat4x4(&lmvpf, lmvp);

            vkCmdPushConstants(cmd, _pipeLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, &lmvpf);

            VkDeviceSize off = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &draw.vertexBuffer, &off);
            vkCmdBindIndexBuffer(cmd, draw.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, draw.indexCount, 1, 0, 0, 0);
        }

        vkCmdEndRenderPass(cmd);
    }
}

// ===========================================================================
// CreateImage
// ===========================================================================
bool VulkanShadows::CreateImage()
{
    VkDevice dev = _core->GetDevice();

    VkImageCreateInfo ii{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType   = VK_IMAGE_TYPE_2D;
    ii.format      = VK_FORMAT_D32_SFLOAT;
    ii.extent      = { SHADOW_SIZE, SHADOW_SIZE, 1 };
    ii.mipLevels   = 1;
    ii.arrayLayers = CASCADE_COUNT;
    ii.samples     = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling      = VK_IMAGE_TILING_OPTIMAL;
    ii.usage       = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(dev, &ii, nullptr, &_image) != VK_SUCCESS)
    {
        LUNA_LOG_ERROR("VulkanShadows: failed to create CSM image");
        return false;
    }

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, _image, &req);
    VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize  = req.size;
    ai.memoryTypeIndex = _core->FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(dev, &ai, nullptr, &_memory) != VK_SUCCESS)
    {
        LUNA_LOG_ERROR("VulkanShadows: failed to allocate CSM memory");
        return false;
    }
    vkBindImageMemory(dev, _image, _memory, 0);

    return true;
}

// ===========================================================================
// CreateViews
// ===========================================================================
bool VulkanShadows::CreateViews()
{
    VkDevice dev = _core->GetDevice();

    // Per-layer views (for framebuffer depth attachment)
    for (uint32_t i = 0; i < CASCADE_COUNT; i++)
    {
        VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image    = _image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format   = VK_FORMAT_D32_SFLOAT;
        vi.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, i, 1 };
        if (vkCreateImageView(dev, &vi, nullptr, &_layerView[i]) != VK_SUCCESS)
        {
            LUNA_LOG_ERROR("VulkanShadows: failed to create layer view %u", i);
            return false;
        }
    }

    // Array view (for sampling in deferred lighting)
    VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image    = _image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vi.format   = VK_FORMAT_D32_SFLOAT;
    vi.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, CASCADE_COUNT };
    if (vkCreateImageView(dev, &vi, nullptr, &_arrayView) != VK_SUCCESS)
    {
        LUNA_LOG_ERROR("VulkanShadows: failed to create array view");
        return false;
    }

    return true;
}

// ===========================================================================
// CreateRenderPass
// ===========================================================================
bool VulkanShadows::CreateRenderPass()
{
    VkAttachmentDescription att{};
    att.format         = VK_FORMAT_D32_SFLOAT;
    att.samples        = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    VkAttachmentReference dr{ 0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp{};
    sp.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.pDepthStencilAttachment = &dr;

    VkSubpassDependency dep{};
    dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass    = 0;
    dep.srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep.dstStageMask  = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dep.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpi{};
    rpi.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpi.attachmentCount = 1;
    rpi.pAttachments    = &att;
    rpi.subpassCount    = 1;
    rpi.pSubpasses      = &sp;
    rpi.dependencyCount = 1;
    rpi.pDependencies   = &dep;

    return vkCreateRenderPass(_core->GetDevice(), &rpi, nullptr, &_renderPass) == VK_SUCCESS;
}

// ===========================================================================
// CreateFramebuffers
// ===========================================================================
bool VulkanShadows::CreateFramebuffers()
{
    VkDevice dev = _core->GetDevice();

    for (uint32_t i = 0; i < CASCADE_COUNT; i++)
    {
        VkFramebufferCreateInfo fi{};
        fi.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fi.renderPass      = _renderPass;
        fi.attachmentCount = 1;
        fi.pAttachments    = &_layerView[i];
        fi.width  = SHADOW_SIZE;
        fi.height = SHADOW_SIZE;
        fi.layers = 1;

        if (vkCreateFramebuffer(dev, &fi, nullptr, &_framebuffers[i]) != VK_SUCCESS)
        {
            LUNA_LOG_ERROR("VulkanShadows: failed to create framebuffer %u", i);
            return false;
        }
    }
    return true;
}

// ===========================================================================
// CreateSampler
// ===========================================================================
bool VulkanShadows::CreateSampler()
{
    VkSamplerCreateInfo si{};
    si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter    = VK_FILTER_NEAREST;
    si.minFilter    = VK_FILTER_NEAREST;
    si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxAnisotropy = 1.0f;

    return vkCreateSampler(_core->GetDevice(), &si, nullptr, &_sampler) == VK_SUCCESS;
}

// ===========================================================================
// CreatePipeline
// ===========================================================================
bool VulkanShadows::CreatePipeline()
{
    VkDevice dev = _core->GetDevice();

    // Pipeline layout: push constant (64B lightMVP, vertex stage)
    {
        VkPushConstantRange pcr{};
        pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pcr.offset     = 0;
        pcr.size       = 64;  // float4x4
        VkPipelineLayoutCreateInfo pli{};
        pli.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges    = &pcr;
        if (vkCreatePipelineLayout(dev, &pli, nullptr, &_pipeLayout) != VK_SUCCESS) return false;
    }

    // Compile shader
    std::vector<uint32_t> vsS;
    if (!CompileGLSLtoSPIRV(GetShaderFullPath(L"csm_depth_vk.vert.glsl").wstring(), vsS))
    {
        LUNA_LOG_ERROR("VulkanShadows: csm_depth shader compile failed");
        return false;
    }

    VkShaderModuleCreateInfo smi{};
    smi.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smi.codeSize = vsS.size() * 4;
    smi.pCode    = vsS.data();
    VkShaderModule vsM = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &smi, nullptr, &vsM) != VK_SUCCESS) return false;

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stage.module = vsM;
    stage.pName  = "main";

    // Vertex layout: stride 48 (PBR vertex), only position consumed
    VkVertexInputBindingDescription vbd{ 0, 48, VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription vad[1]{};
    vad[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 };

    VkPipelineVertexInputStateCreateInfo vis{};
    vis.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vis.vertexBindingDescriptionCount   = 1;
    vis.pVertexBindingDescriptions      = &vbd;
    vis.vertexAttributeDescriptionCount = 1;
    vis.pVertexAttributeDescriptions    = vad;

    VkPipelineInputAssemblyStateCreateInfo ias{};
    ias.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ias.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport vp{ 0, 0, (float)SHADOW_SIZE, (float)SHADOW_SIZE, 0, 1 };
    VkRect2D   sc{ {0,0}, {SHADOW_SIZE, SHADOW_SIZE} };
    VkPipelineViewportStateCreateInfo vps{};
    vps.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1; vps.pViewports = &vp;
    vps.scissorCount  = 1; vps.pScissors = &sc;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode             = VK_POLYGON_MODE_FILL;
    rs.cullMode                = VK_CULL_MODE_FRONT_BIT;   // front-face culling reduces acne
    rs.frontFace               = VK_FRONT_FACE_CLOCKWISE;  // matches -fvk-invert-y winding
    rs.lineWidth               = 1.0f;
    rs.depthBiasEnable         = VK_TRUE;
    rs.depthBiasConstantFactor = 1.25f;
    rs.depthBiasSlopeFactor    = 1.75f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable  = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp   = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendStateCreateInfo cbs{};
    cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;

    VkGraphicsPipelineCreateInfo gpi{};
    gpi.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpi.stageCount          = 1;
    gpi.pStages             = &stage;
    gpi.pVertexInputState   = &vis;
    gpi.pInputAssemblyState = &ias;
    gpi.pViewportState      = &vps;
    gpi.pRasterizationState = &rs;
    gpi.pMultisampleState   = &ms;
    gpi.pDepthStencilState  = &ds;
    gpi.pColorBlendState    = &cbs;
    gpi.layout              = _pipeLayout;
    gpi.renderPass          = _renderPass;
    gpi.subpass             = 0;

    VkResult r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, nullptr, &_pipeline);
    vkDestroyShaderModule(dev, vsM, nullptr);

    if (r != VK_SUCCESS)
    {
        LUNA_LOG_ERROR("VulkanShadows: pipeline creation failed: %d", (int)r);
        return false;
    }

    return true;
}

} // namespace Luna

