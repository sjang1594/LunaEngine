#include "LunaPCH.h"
#include "LunaEngine/Renderer/Vulkan/Public/VulkanSSAO.h"
#include "LunaEngine/Renderer/Vulkan/Public/VulkanCore.h"
#include "LunaEngine/Utils/FileSystemUtil.h"
#include "Logger/Logger.h"

#include <dxcapi.h>
#include <random>
#include <algorithm>

namespace Luna
{

// ===========================================================================
// Local shader compilation helpers (duplicated from VulkanBackend.cpp)
// TODO: Extract to shared VulkanShaderUtils
// ===========================================================================

// DXC GUIDs
static const GUID CLSID_DxcUtils_SSAO    = { 0x6245d6af, 0x66e0, 0x48fd, { 0x80, 0xb4, 0x4d, 0x27, 0x17, 0x96, 0x74, 0x8c } };
static const GUID CLSID_DxcCompiler_SSAO = { 0x73e22d93, 0xe6ce, 0x47f3, { 0xb5, 0xbf, 0xf0, 0x66, 0x4f, 0x39, 0xc1, 0xb0 } };

static bool CompileHLSLtoSPIRV(const std::wstring& path, const std::wstring& target,
                                std::vector<uint32_t>& outSpirv)
{
    ComPtr<IDxcUtils>     utils;
    ComPtr<IDxcCompiler3> compiler;
    HRESULT hr1 = DxcCreateInstance(CLSID_DxcUtils_SSAO,     IID_PPV_ARGS(&utils));
    HRESULT hr2 = DxcCreateInstance(CLSID_DxcCompiler_SSAO, IID_PPV_ARGS(&compiler));
    if (FAILED(hr1) || !utils || FAILED(hr2) || !compiler) return false;

    ComPtr<IDxcBlobEncoding> src;
    if (FAILED(utils->LoadFile(path.c_str(), nullptr, &src))) return false;

    DxcBuffer buf{ src->GetBufferPointer(), src->GetBufferSize(), DXC_CP_ACP };
    std::vector<LPCWSTR> args = {
        path.c_str(), L"-E", L"main", L"-T", target.c_str(),
        L"-spirv", L"-fvk-use-dx-layout", L"-HV", L"2021",
    };
    if (target.find(L"vs_") == 0) args.push_back(L"-fvk-invert-y");

    ComPtr<IDxcResult> result;
    compiler->Compile(&buf, args.data(), (UINT32)args.size(), nullptr, IID_PPV_ARGS(&result));

    HRESULT hr = S_OK; result->GetStatus(&hr);
    if (FAILED(hr)) return false;

    ComPtr<IDxcBlob> blob;
    result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&blob), nullptr);
    if (!blob) return false;

    outSpirv.resize(blob->GetBufferSize() / 4);
    memcpy(outSpirv.data(), blob->GetBufferPointer(), blob->GetBufferSize());
    return true;
}

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

    char buf2[512]; DWORD nRead = 0;
    while (ReadFile(hRead, buf2, sizeof(buf2)-1, &nRead, nullptr) && nRead) {}
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
// Fullscreen pipeline helper
// ===========================================================================
static VkPipeline CreateFullscreenPipeline(VkDevice dev, const wchar_t* fsPath,
                                            VkPipelineLayout layout, VkRenderPass rp)
{
    std::vector<uint32_t> vsS, fsS;
    std::wstring fsWPath = GetShaderFullPath(fsPath).wstring();
    bool fsOK = (fsWPath.find(L".glsl") != std::wstring::npos)
        ? CompileGLSLtoSPIRV(fsWPath, fsS)
        : CompileHLSLtoSPIRV(fsWPath, L"ps_6_0", fsS);
    if (!CompileHLSLtoSPIRV(GetShaderFullPath(L"fullscreen.vert.hlsl").wstring(), L"vs_6_0", vsS) || !fsOK)
        return VK_NULL_HANDLE;

    auto mkMod = [&](const std::vector<uint32_t>& sp) {
        VkShaderModuleCreateInfo si{}; si.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        si.codeSize = sp.size()*4; si.pCode = sp.data();
        VkShaderModule m = VK_NULL_HANDLE; vkCreateShaderModule(dev, &si, nullptr, &m); return m;
    };
    VkShaderModule vsM = mkMod(vsS), fsM = mkMod(fsS);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,   vsM, "main" };
    stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fsM, "main" };

    VkPipelineVertexInputStateCreateInfo vis{}; vis.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ias{}; ias.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO; ias.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vps{}; vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO; vps.viewportCount = 1; vps.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{}; rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo ms{}; ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{}; ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    VkPipelineColorBlendAttachmentState cba{}; cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cbs{}; cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO; cbs.attachmentCount = 1; cbs.pAttachments = &cba;
    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dsi{}; dsi.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO; dsi.dynamicStateCount = 2; dsi.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo gpi{}; gpi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpi.stageCount = 2; gpi.pStages = stages; gpi.pVertexInputState = &vis; gpi.pInputAssemblyState = &ias;
    gpi.pViewportState = &vps; gpi.pRasterizationState = &rs; gpi.pMultisampleState = &ms;
    gpi.pDepthStencilState = &ds; gpi.pColorBlendState = &cbs; gpi.pDynamicState = &dsi;
    gpi.layout = layout; gpi.renderPass = rp;

    VkPipeline pipe = VK_NULL_HANDLE;
    vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, nullptr, &pipe);
    vkDestroyShaderModule(dev, vsM, nullptr);
    vkDestroyShaderModule(dev, fsM, nullptr);
    return pipe;
}

// ===========================================================================
// Destructor
// ===========================================================================
VulkanSSAO::~VulkanSSAO()
{
    Destroy();
}

// ===========================================================================
// Create
// ===========================================================================
bool VulkanSSAO::Create(const CreateInfo& info)
{
    if (!info.core) { LUNA_LOG_ERROR("VulkanSSAO: null core"); return false; }
    if (info.extent.width == 0 || info.extent.height == 0) return false;

    _core = info.core;
    _externalPointClamp = info.pointClampSampler;
    _framesInFlight = std::min(info.framesInFlight, MAX_FRAMES);
    _halfW = std::max(1u, info.extent.width  / 2);
    _halfH = std::max(1u, info.extent.height / 2);

    VkExtent2D halfExtent{ _halfW, _halfH };

    if (!CreateRenderTargets(halfExtent))   { Destroy(); return false; }
    if (!CreateNoiseTexture())              { Destroy(); return false; }
    if (!CreateSamplers())                  { Destroy(); return false; }
    if (!CreateRenderPass())                { Destroy(); return false; }
    if (!CreateFramebuffers(halfExtent))    { Destroy(); return false; }
    if (!CreateDescriptors(info.depthView, info.normalView)) { Destroy(); return false; }
    if (!CreatePipelines())                 { Destroy(); return false; }
    GenerateKernel();

    LUNA_LOG_INFO("VulkanSSAO: created (%ux%u, %d samples)", _halfW, _halfH, SAMPLE_COUNT);
    return true;
}

// ===========================================================================
// Destroy
// ===========================================================================
void VulkanSSAO::Destroy()
{
    if (!_core) return;
    VkDevice dev = _core->GetDevice();
    if (!dev) return;

    auto destroyImage = [&](VkImageView& v, VkImage& i, VkDeviceMemory& m) {
        if (v) { vkDestroyImageView(dev, v, nullptr); v = VK_NULL_HANDLE; }
        if (i) { vkDestroyImage(dev, i, nullptr);     i = VK_NULL_HANDLE; }
        if (m) { vkFreeMemory(dev, m, nullptr);        m = VK_NULL_HANDLE; }
    };

    destroyImage(_rawView,   _rawImage,   _rawMem);
    destroyImage(_blurView,  _blurImage,  _blurMem);
    destroyImage(_noiseView, _noiseImage, _noiseMem);

    if (_rawFramebuffer)  { vkDestroyFramebuffer(dev, _rawFramebuffer, nullptr);  _rawFramebuffer  = VK_NULL_HANDLE; }
    if (_blurFramebuffer) { vkDestroyFramebuffer(dev, _blurFramebuffer, nullptr); _blurFramebuffer = VK_NULL_HANDLE; }

    if (_pipeline)        { vkDestroyPipeline(dev, _pipeline, nullptr);           _pipeline      = VK_NULL_HANDLE; }
    if (_blurPipeline)    { vkDestroyPipeline(dev, _blurPipeline, nullptr);       _blurPipeline  = VK_NULL_HANDLE; }
    if (_pipeLayout)      { vkDestroyPipelineLayout(dev, _pipeLayout, nullptr);   _pipeLayout    = VK_NULL_HANDLE; }
    if (_blurPipeLayout)  { vkDestroyPipelineLayout(dev, _blurPipeLayout, nullptr); _blurPipeLayout = VK_NULL_HANDLE; }

    if (_sceneLayout) { vkDestroyDescriptorSetLayout(dev, _sceneLayout, nullptr); _sceneLayout = VK_NULL_HANDLE; }
    if (_texLayout)   { vkDestroyDescriptorSetLayout(dev, _texLayout, nullptr);   _texLayout   = VK_NULL_HANDLE; }
    if (_blurLayout)  { vkDestroyDescriptorSetLayout(dev, _blurLayout, nullptr);  _blurLayout  = VK_NULL_HANDLE; }
    if (_descPool)    { vkDestroyDescriptorPool(dev, _descPool, nullptr);         _descPool    = VK_NULL_HANDLE; }

    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        if (_cbMapped[i]) { vkUnmapMemory(dev, _cbMem[i]); _cbMapped[i] = nullptr; }
        if (_cb[i])       { vkDestroyBuffer(dev, _cb[i], nullptr);  _cb[i]    = VK_NULL_HANDLE; }
        if (_cbMem[i])    { vkFreeMemory(dev, _cbMem[i], nullptr); _cbMem[i] = VK_NULL_HANDLE; }
    }

    if (_pointWrap)     { vkDestroySampler(dev, _pointWrap, nullptr);     _pointWrap     = VK_NULL_HANDLE; }
    if (_bilinearClamp) { vkDestroySampler(dev, _bilinearClamp, nullptr); _bilinearClamp = VK_NULL_HANDLE; }
    if (_renderPass)    { vkDestroyRenderPass(dev, _renderPass, nullptr); _renderPass    = VK_NULL_HANDLE; }

    _halfW = _halfH = 0;
}

// ===========================================================================
// Resize
// ===========================================================================
bool VulkanSSAO::Resize(VkExtent2D extent, VkImageView depthView, VkImageView normalView)
{
    VulkanCore* core = _core;
    VkSampler pointClamp = _externalPointClamp;
    uint32_t fif = _framesInFlight;
    Destroy();

    CreateInfo info;
    info.core = core;
    info.extent = extent;
    info.depthView = depthView;
    info.normalView = normalView;
    info.pointClampSampler = pointClamp;
    info.framesInFlight = fif;
    return Create(info);
}

// ===========================================================================
// Draw
// ===========================================================================
void VulkanSSAO::Draw(VkCommandBuffer cmd, uint32_t frameIndex,
                      const XMFLOAT4X4& view, const XMFLOAT4X4& proj)
{
    if (!_pipeline) return;

    // Update UBO
    XMMATRIX projM = XMLoadFloat4x4(&proj);
    XMMATRIX viewM = XMLoadFloat4x4(&view);
    XMStoreFloat4x4(&_kernel.projection,    projM);
    XMStoreFloat4x4(&_kernel.invProjection, XMMatrixInverse(nullptr, projM));
    XMStoreFloat4x4(&_kernel.view,          viewM);
    _kernel.noiseScale = XMFLOAT2(float(_halfW) / 4.f, float(_halfH) / 4.f);
    memcpy(_cbMapped[frameIndex], &_kernel, sizeof(SSAOConstants));

    VkClearValue clear{}; clear.color.float32[0] = 1.0f;
    VkRenderPassBeginInfo rpi{}; rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpi.renderPass = _renderPass; rpi.framebuffer = _rawFramebuffer;
    rpi.renderArea.extent = { _halfW, _halfH }; rpi.clearValueCount = 1; rpi.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rpi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{ 0, 0, (float)_halfW, (float)_halfH, 0, 1 }; vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc{ {0,0}, {_halfW, _halfH} }; vkCmdSetScissor(cmd, 0, 1, &sc);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _pipeline);
    VkDescriptorSet sets[] = { _sceneDescSet[frameIndex], _texDescSet };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _pipeLayout, 0, 2, sets, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
}

// ===========================================================================
// DrawBlur
// ===========================================================================
void VulkanSSAO::DrawBlur(VkCommandBuffer cmd)
{
    if (!_blurPipeline) return;

    VkClearValue clear{}; clear.color.float32[0] = 1.0f;
    VkRenderPassBeginInfo rpi{}; rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpi.renderPass = _renderPass; rpi.framebuffer = _blurFramebuffer;
    rpi.renderArea.extent = { _halfW, _halfH }; rpi.clearValueCount = 1; rpi.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rpi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{ 0, 0, (float)_halfW, (float)_halfH, 0, 1 }; vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc{ {0,0}, {_halfW, _halfH} }; vkCmdSetScissor(cmd, 0, 1, &sc);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _blurPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _blurPipeLayout, 0, 1, &_blurDescSet, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
}

// ===========================================================================
// CreateRenderTargets
// ===========================================================================
bool VulkanSSAO::CreateRenderTargets(VkExtent2D halfExtent)
{
    auto mkRT = [&](VkImage& img, VkDeviceMemory& mem, VkImageView& view) -> bool {
        if (!_core->CreateImage(halfExtent.width, halfExtent.height, VK_FORMAT_R8_UNORM,
                VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, img, mem))
            return false;
        view = _core->CreateImageView(img, VK_FORMAT_R8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);
        return view != VK_NULL_HANDLE;
    };

    if (!mkRT(_rawImage, _rawMem, _rawView)) return false;
    if (!mkRT(_blurImage, _blurMem, _blurView)) return false;

    _core->TransitionImageLayout(_rawImage,  VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    _core->TransitionImageLayout(_blurImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    return true;
}

// ===========================================================================
// CreateNoiseTexture
// ===========================================================================
bool VulkanSSAO::CreateNoiseTexture()
{
    VkDevice dev = _core->GetDevice();

    std::mt19937 rng(123);
    std::uniform_real_distribution<float> dist(-1.f, 1.f);
    struct { uint8_t r, g; } pixels[NOISE_SIZE * NOISE_SIZE];
    for (auto& p : pixels) {
        p.r = uint8_t((dist(rng) * 0.5f + 0.5f) * 255.f);
        p.g = uint8_t((dist(rng) * 0.5f + 0.5f) * 255.f);
    }

    if (!_core->CreateImage(NOISE_SIZE, NOISE_SIZE, VK_FORMAT_R8G8_UNORM,
            VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, _noiseImage, _noiseMem))
        return false;

    _noiseView = _core->CreateImageView(_noiseImage, VK_FORMAT_R8G8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!_noiseView) return false;

    // Upload via staging buffer
    VkDeviceSize sz = NOISE_SIZE * NOISE_SIZE * 2;
    VkBuffer staging; VkDeviceMemory stagingMem;
    if (!_core->CreateBuffer(sz, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            staging, stagingMem))
        return false;

    void* data; vkMapMemory(dev, stagingMem, 0, sz, 0, &data);
    memcpy(data, pixels, sz);
    vkUnmapMemory(dev, stagingMem);

    _core->TransitionImageLayout(_noiseImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    _core->CopyBufferToImage(staging, _noiseImage, NOISE_SIZE, NOISE_SIZE);
    _core->TransitionImageLayout(_noiseImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    vkDestroyBuffer(dev, staging, nullptr);
    vkFreeMemory(dev, stagingMem, nullptr);

    return true;
}

// ===========================================================================
// CreateSamplers
// ===========================================================================
bool VulkanSSAO::CreateSamplers()
{
    VkDevice dev = _core->GetDevice();

    {
        VkSamplerCreateInfo si{}; si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = si.minFilter = VK_FILTER_NEAREST;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        if (vkCreateSampler(dev, &si, nullptr, &_pointWrap) != VK_SUCCESS) return false;
    }
    {
        VkSamplerCreateInfo si{}; si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(dev, &si, nullptr, &_bilinearClamp) != VK_SUCCESS) return false;
    }

    return true;
}

// ===========================================================================
// CreateRenderPass
// ===========================================================================
bool VulkanSSAO::CreateRenderPass()
{
    VkAttachmentDescription att{};
    att.format = VK_FORMAT_R8_UNORM; att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout   = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference cr{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp{}; sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1; sp.pColorAttachments = &cr;

    VkRenderPassCreateInfo rpi{}; rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpi.attachmentCount = 1; rpi.pAttachments = &att;
    rpi.subpassCount = 1; rpi.pSubpasses = &sp;

    return vkCreateRenderPass(_core->GetDevice(), &rpi, nullptr, &_renderPass) == VK_SUCCESS;
}

// ===========================================================================
// CreateFramebuffers
// ===========================================================================
bool VulkanSSAO::CreateFramebuffers(VkExtent2D halfExtent)
{
    VkDevice dev = _core->GetDevice();

    auto mkFB = [&](VkImageView v) -> VkFramebuffer {
        VkFramebufferCreateInfo fi{}; fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fi.renderPass = _renderPass; fi.attachmentCount = 1; fi.pAttachments = &v;
        fi.width = halfExtent.width; fi.height = halfExtent.height; fi.layers = 1;
        VkFramebuffer fb = VK_NULL_HANDLE;
        vkCreateFramebuffer(dev, &fi, nullptr, &fb);
        return fb;
    };

    _rawFramebuffer  = mkFB(_rawView);
    _blurFramebuffer = mkFB(_blurView);

    return _rawFramebuffer && _blurFramebuffer;
}

// ===========================================================================
// CreateDescriptors
// ===========================================================================
bool VulkanSSAO::CreateDescriptors(VkImageView depthView, VkImageView normalView)
{
    VkDevice dev = _core->GetDevice();

    // -- Layouts --
    // set=0: UBO
    {
        VkDescriptorSetLayoutBinding b{ 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        VkDescriptorSetLayoutCreateInfo li{}; li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 1; li.pBindings = &b;
        if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &_sceneLayout) != VK_SUCCESS) return false;
    }
    // set=1: depth + normal + noise + 2 samplers
    {
        VkDescriptorSetLayoutBinding bs[5]{};
        bs[0] = { 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        bs[1] = { 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        bs[2] = { 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        bs[3] = { 3, VK_DESCRIPTOR_TYPE_SAMPLER,       1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        bs[4] = { 4, VK_DESCRIPTOR_TYPE_SAMPLER,       1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        VkDescriptorSetLayoutCreateInfo li{}; li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 5; li.pBindings = bs;
        if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &_texLayout) != VK_SUCCESS) return false;
    }
    // blur set=0: raw ssao + sampler
    {
        VkDescriptorSetLayoutBinding bs[2]{};
        bs[0] = { 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        bs[1] = { 1, VK_DESCRIPTOR_TYPE_SAMPLER,       1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        VkDescriptorSetLayoutCreateInfo li{}; li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 2; li.pBindings = bs;
        if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &_blurLayout) != VK_SUCCESS) return false;
    }

    // -- Pipeline layouts --
    {
        VkDescriptorSetLayout sl[] = { _sceneLayout, _texLayout };
        VkPipelineLayoutCreateInfo pli{}; pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = 2; pli.pSetLayouts = sl;
        if (vkCreatePipelineLayout(dev, &pli, nullptr, &_pipeLayout) != VK_SUCCESS) return false;
    }
    {
        VkPipelineLayoutCreateInfo pli{}; pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = 1; pli.pSetLayouts = &_blurLayout;
        if (vkCreatePipelineLayout(dev, &pli, nullptr, &_blurPipeLayout) != VK_SUCCESS) return false;
    }

    // -- Pool --
    VkDescriptorPoolSize ps[] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, _framesInFlight },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  4 },
        { VK_DESCRIPTOR_TYPE_SAMPLER,        3 },
    };
    VkDescriptorPoolCreateInfo dpi{}; dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.poolSizeCount = (uint32_t)std::size(ps); dpi.pPoolSizes = ps;
    dpi.maxSets = _framesInFlight + 2;
    if (vkCreateDescriptorPool(dev, &dpi, nullptr, &_descPool) != VK_SUCCESS) return false;

    // -- Per-frame scene UBOs + descriptor sets --
    for (uint32_t i = 0; i < _framesInFlight; i++)
    {
        if (!_core->CreateBuffer(512, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                _cb[i], _cbMem[i]))
            return false;
        if (vkMapMemory(dev, _cbMem[i], 0, 512, 0, &_cbMapped[i]) != VK_SUCCESS) return false;

        VkDescriptorSetAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = _descPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &_sceneLayout;
        if (vkAllocateDescriptorSets(dev, &ai, &_sceneDescSet[i]) != VK_SUCCESS) return false;

        VkDescriptorBufferInfo bi{ _cb[i], 0, sizeof(SSAOConstants) };
        VkWriteDescriptorSet wr{}; wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr.dstSet = _sceneDescSet[i]; wr.dstBinding = 0;
        wr.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; wr.descriptorCount = 1; wr.pBufferInfo = &bi;
        vkUpdateDescriptorSets(dev, 1, &wr, 0, nullptr);
    }

    // -- Texture descriptor set --
    {
        VkDescriptorSetAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = _descPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &_texLayout;
        if (vkAllocateDescriptorSets(dev, &ai, &_texDescSet) != VK_SUCCESS) return false;

        VkDescriptorImageInfo di { _externalPointClamp, depthView,   VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo ni { _externalPointClamp, normalView,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo noi{ _pointWrap,          _noiseView,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo s0 { _externalPointClamp, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
        VkDescriptorImageInfo s1 { _pointWrap,          VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };

        VkWriteDescriptorSet ws[5]{};
        ws[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _texDescSet, 0, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &di,  nullptr, nullptr };
        ws[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _texDescSet, 1, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &ni,  nullptr, nullptr };
        ws[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _texDescSet, 2, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &noi, nullptr, nullptr };
        ws[3] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _texDescSet, 3, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER,       &s0,  nullptr, nullptr };
        ws[4] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _texDescSet, 4, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER,       &s1,  nullptr, nullptr };
        vkUpdateDescriptorSets(dev, 5, ws, 0, nullptr);
    }

    // -- Blur descriptor set --
    {
        VkDescriptorSetAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = _descPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &_blurLayout;
        if (vkAllocateDescriptorSets(dev, &ai, &_blurDescSet) != VK_SUCCESS) return false;

        VkDescriptorImageInfo ri { _externalPointClamp, _rawView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo si { _externalPointClamp, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };

        VkWriteDescriptorSet ws[2]{};
        ws[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _blurDescSet, 0, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &ri, nullptr, nullptr };
        ws[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _blurDescSet, 1, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER,       &si, nullptr, nullptr };
        vkUpdateDescriptorSets(dev, 2, ws, 0, nullptr);
    }

    return true;
}

// ===========================================================================
// CreatePipelines
// ===========================================================================
bool VulkanSSAO::CreatePipelines()
{
    VkDevice dev = _core->GetDevice();
    _pipeline     = CreateFullscreenPipeline(dev, L"ssao_vk.frag.glsl",      _pipeLayout,     _renderPass);
    _blurPipeline = CreateFullscreenPipeline(dev, L"ssao_blur_vk.frag.glsl", _blurPipeLayout, _renderPass);

    if (!_pipeline || !_blurPipeline)
    {
        LUNA_LOG_ERROR("VulkanSSAO: pipeline creation failed");
        return false;
    }
    return true;
}

// ===========================================================================
// GenerateKernel
// ===========================================================================
void VulkanSSAO::GenerateKernel()
{
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);
    for (int i = 0; i < SAMPLE_COUNT; ++i)
    {
        XMFLOAT3 s{ dist(rng)*2.f-1.f, dist(rng)*2.f-1.f, dist(rng) };
        XMVECTOR v = XMVector3Normalize(XMLoadFloat3(&s));
        float scale = float(i) / float(SAMPLE_COUNT);
        scale = 0.1f + scale * scale * 0.9f;
        v = XMVectorScale(v, scale * dist(rng));
        XMFLOAT3 r; XMStoreFloat3(&r, v);
        _kernel.samples[i] = XMFLOAT4(r.x, r.y, r.z, 0.f);
    }
    _kernel.radius = 0.5f;
    _kernel.bias   = 0.025f;
    _kernel.noiseScale = XMFLOAT2(float(_halfW) / 4.f, float(_halfH) / 4.f);
}

} // namespace Luna

