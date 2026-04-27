#include "LunaPCH.h"
#include "LunaEngine/Renderer/Vulkan/Public/VulkanPostProcess.h"
#include "LunaEngine/Renderer/Vulkan/Public/VulkanCore.h"
#include "LunaEngine/Utils/FileSystemUtil.h"
#include "Logger/Logger.h"

#include <dxcapi.h>
#include <initguid.h>
DEFINE_GUID(CLSID_DxcUtils_PP, 0x6245d6af, 0x66e0, 0x48fd, 0x80, 0xb4, 0x4d, 0x27, 0x17, 0x96, 0x74, 0x8c);
DEFINE_GUID(CLSID_DxcCompiler_PP, 0x73e22d93, 0xe6ce, 0x47f3, 0xb5, 0xbf, 0xf0, 0x66, 0x4f, 0x39, 0xc1, 0xb0);

namespace Luna
{

// ---------------------------------------------------------------------------
// DXC helper — compile HLSL → SPIR-V
// ---------------------------------------------------------------------------
static bool CompileHLSLtoSPIRV(const std::wstring& path, const std::wstring& target,
                                 std::vector<uint32_t>& outSpirv)
{
    ComPtr<IDxcUtils>     utils;
    ComPtr<IDxcCompiler3> compiler;
    HRESULT hr1 = DxcCreateInstance(CLSID_DxcUtils_PP,     IID_PPV_ARGS(&utils));
    HRESULT hr2 = DxcCreateInstance(CLSID_DxcCompiler_PP, IID_PPV_ARGS(&compiler));
    if (FAILED(hr1) || !utils) { LUNA_LOG_ERROR("VK PP: DxcUtils creation failed (0x%08X)", hr1); return false; }
    if (FAILED(hr2) || !compiler) { LUNA_LOG_ERROR("VK PP: DxcCompiler creation failed (0x%08X)", hr2); return false; }

    ComPtr<IDxcBlobEncoding> src;
    if (FAILED(utils->LoadFile(path.c_str(), nullptr, &src)))
    { LUNA_LOG_ERROR("VK PP Shader: cannot load %ls", path.c_str()); return false; }

    DxcBuffer buf{ src->GetBufferPointer(), src->GetBufferSize(), DXC_CP_ACP };
    
    std::vector<LPCWSTR> args = {
        path.c_str(), L"-E", L"main", L"-T", target.c_str(),
        L"-spirv", L"-fvk-use-dx-layout", L"-HV", L"2021",
    };
    bool isVertexShader = (target.find(L"vs_") == 0);
    if (isVertexShader) args.push_back(L"-fvk-invert-y");

    ComPtr<IDxcResult> result;
    compiler->Compile(&buf, args.data(), (UINT32)args.size(), nullptr, IID_PPV_ARGS(&result));

    HRESULT hr = S_OK;
    result->GetStatus(&hr);
    ComPtr<IDxcBlobUtf8> errors;
    result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
    if (errors && errors->GetStringLength() > 0)
        LUNA_LOG_ERROR("VK PP Shader %ls: %s", path.c_str(), errors->GetStringPointer());
    if (FAILED(hr)) return false;

    ComPtr<IDxcBlob> blob;
    result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&blob), nullptr);
    if (!blob) return false;

    outSpirv.resize(blob->GetBufferSize() / 4);
    memcpy(outSpirv.data(), blob->GetBufferPointer(), blob->GetBufferSize());
    return true;
}

// ---------------------------------------------------------------------------
// glslc helper — compile GLSL → SPIR-V via glslc subprocess
// ---------------------------------------------------------------------------
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
    else { LUNA_LOG_ERROR("VK PP GLSL: cannot determine stage for %ls", glslPath.c_str()); return false; }

    std::wstring cmd = L"\"" + glslcPath + L"\" --target-env=vulkan1.1 --target-spv=spv1.4"
                     + L" -fshader-stage=" + stage
                     + L" -o \"" + spvPath + L"\" \"" + glslPath + L"\"";

    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE hRead = INVALID_HANDLE_VALUE, hWrite = INVALID_HANDLE_VALUE;
    CreatePipe(&hRead, &hWrite, &sa, 0);
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb          = sizeof(si);
    si.dwFlags     = STARTF_USESTDHANDLES;
    si.hStdError   = hWrite;
    si.hStdOutput  = hWrite;
    PROCESS_INFORMATION pi{};

    std::vector<wchar_t> cmdLine(cmd.begin(), cmd.end());
    cmdLine.push_back(0);

    BOOL ok = CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr,
                             TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(hWrite);

    if (!ok) {
        CloseHandle(hRead);
        LUNA_LOG_ERROR("VK PP: glslc launch failed");
        return false;
    }

    char errBuf[4096] = {}; DWORD nRead = 0;
    ReadFile(hRead, errBuf, sizeof(errBuf)-1, &nRead, nullptr);
    CloseHandle(hRead);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (exitCode != 0) {
        if (nRead > 0) LUNA_LOG_ERROR("VK PP glslc: %s", errBuf);
        return false;
    }

    HANDLE hf = CreateFileW(spvPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf == INVALID_HANDLE_VALUE) {
        LUNA_LOG_ERROR("VK PP: cannot open compiled spv %ls", spvPath.c_str());
        return false;
    }
    DWORD sz = GetFileSize(hf, nullptr);
    outSpirv.resize(sz / 4);
    ReadFile(hf, outSpirv.data(), sz, &nRead, nullptr);
    CloseHandle(hf);
    DeleteFileW(spvPath.c_str());

    return true;
}

// ===========================================================================
// VulkanPostProcess
// ===========================================================================

VulkanPostProcess::~VulkanPostProcess()
{
    Destroy();
}

bool VulkanPostProcess::Create(const CreateInfo& info)
{
    if (!info.core) return false;
    _core = info.core;
    _extent = info.extent;
    _swapchainFormat = info.swapchainFormat;
    _depthView = info.depthView;
    _normalView = info.normalView;
    _metalRoughView = info.metalRoughView;
    _linearSampler = info.linearSampler;
    _pointClampSampler = info.pointClampSampler;

    if (!CreateRenderPasses()) return false;
    if (!CreateImages()) return false;
    if (!CreateFramebuffers()) return false;
    if (!CreateSSRResources()) { LUNA_LOG_WARN("VK PP: SSR resources failed"); }
    if (!CreateMotionBlurResources()) { LUNA_LOG_WARN("VK PP: Motion Blur resources failed"); }
    if (!CreateTAAResources()) { LUNA_LOG_WARN("VK PP: TAA resources failed"); }
    if (!CreateBloomResources()) { LUNA_LOG_WARN("VK PP: Bloom resources failed"); }
    if (!CreateTonemapResources()) { LUNA_LOG_WARN("VK PP: Tonemap resources failed"); }

    _ready = true;
    LUNA_LOG_INFO("VK PP: PostProcess created (%ux%u)", _extent.width, _extent.height);
    return true;
}

void VulkanPostProcess::Destroy()
{
    if (!_core) return;

    DestroyTonemapResources();
    DestroyBloomResources();
    DestroyTAAResources();
    DestroyMotionBlurResources();
    DestroySSRResources();
    DestroyFramebuffers();
    DestroyImages();

    VkDevice dev = _core->GetDevice();
    if (_ppRenderPass) { vkDestroyRenderPass(dev, _ppRenderPass, nullptr); _ppRenderPass = VK_NULL_HANDLE; }
    if (_tonemapRenderPass) { vkDestroyRenderPass(dev, _tonemapRenderPass, nullptr); _tonemapRenderPass = VK_NULL_HANDLE; }

    _ready = false;
    _core = nullptr;
}

bool VulkanPostProcess::Resize(const CreateInfo& info)
{
    Destroy();
    return Create(info);
}

// ===========================================================================
// Render Passes
// ===========================================================================

bool VulkanPostProcess::CreateRenderPasses()
{
    auto createRP = [this](VkFormat fmt, VkImageLayout finalLayout) -> VkRenderPass
    {
        VkAttachmentDescription att{};
        att.format         = fmt;
        att.samples        = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout    = finalLayout;

        VkAttachmentReference cr{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };

        VkSubpassDescription sp{};
        sp.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sp.colorAttachmentCount = 1;
        sp.pColorAttachments    = &cr;

        VkSubpassDependency dep{};
        dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass    = 0;
        dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                          | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                          | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo rpi{};
        rpi.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpi.attachmentCount = 1;
        rpi.pAttachments    = &att;
        rpi.subpassCount    = 1;
        rpi.pSubpasses      = &sp;
        rpi.dependencyCount = 1;
        rpi.pDependencies   = &dep;
        VkRenderPass rp = VK_NULL_HANDLE;
        vkCreateRenderPass(_core->GetDevice(), &rpi, nullptr, &rp);
        return rp;
    };

    _ppRenderPass = createRP(VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (!_ppRenderPass) return false;

    _tonemapRenderPass = createRP(_swapchainFormat, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    if (!_tonemapRenderPass) return false;

    return true;
}

// ===========================================================================
// Images
// ===========================================================================

bool VulkanPostProcess::CreateImages()
{
    uint32_t W = _extent.width, H = _extent.height;

    // HDR image
    if (!_core->CreateImage(W, H, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, _hdrImage, _hdrMemory))
        return false;
    _hdrView = _core->CreateImageView(_hdrImage, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!_hdrView) return false;

    // SSR image (STORAGE | SAMPLED, kept GENERAL)
    if (!_core->CreateImage(W, H, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, _ssrImage, _ssrMemory))
        return false;
    _ssrView = _core->CreateImageView(_ssrImage, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!_ssrView) return false;
    _core->TransitionImageLayout(_ssrImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

    // Motion blur image
    if (!_core->CreateImage(W, H, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, _mbImage, _mbMemory))
        return false;
    _mbView = _core->CreateImageView(_mbImage, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT);
    if (_mbView) _core->TransitionImageLayout(_mbImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // TAA history images (2×)
    for (int i = 0; i < 2; i++) {
        if (!_core->CreateImage(W, H, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, _taaHistoryImage[i], _taaHistoryMemory[i]))
            return false;
        _taaHistoryView[i] = _core->CreateImageView(_taaHistoryImage[i], VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT);
        if (!_taaHistoryView[i]) return false;
        _core->TransitionImageLayout(_taaHistoryImage[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    // Bloom images (half-res)
    uint32_t halfW = std::max(1u, W / 2), halfH = std::max(1u, H / 2);
    if (!_core->CreateImage(halfW, halfH, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, _bloomBrightImage, _bloomBrightMemory))
        return false;
    _bloomBrightView = _core->CreateImageView(_bloomBrightImage, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT);

    if (!_core->CreateImage(halfW, halfH, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, _bloomBlurImage, _bloomBlurMemory))
        return false;
    _bloomBlurView = _core->CreateImageView(_bloomBlurImage, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT);

    return true;
}

void VulkanPostProcess::DestroyImages()
{
    if (!_core) return;
    VkDevice dev = _core->GetDevice();

    auto destroyImg = [&](VkImageView& v, VkImage& i, VkDeviceMemory& m) {
        if (v) { vkDestroyImageView(dev, v, nullptr); v = VK_NULL_HANDLE; }
        if (i) { vkDestroyImage(dev, i, nullptr);     i = VK_NULL_HANDLE; }
        if (m) { vkFreeMemory(dev, m, nullptr);       m = VK_NULL_HANDLE; }
    };

    destroyImg(_bloomBlurView, _bloomBlurImage, _bloomBlurMemory);
    destroyImg(_bloomBrightView, _bloomBrightImage, _bloomBrightMemory);
    for (int i = 0; i < 2; i++)
        destroyImg(_taaHistoryView[i], _taaHistoryImage[i], _taaHistoryMemory[i]);
    destroyImg(_mbView, _mbImage, _mbMemory);
    destroyImg(_ssrView, _ssrImage, _ssrMemory);
    destroyImg(_hdrView, _hdrImage, _hdrMemory);
}

// ===========================================================================
// Framebuffers
// ===========================================================================

bool VulkanPostProcess::CreateFramebuffers()
{
    VkDevice dev = _core->GetDevice();
    uint32_t W = _extent.width, H = _extent.height;
    uint32_t halfW = std::max(1u, W / 2), halfH = std::max(1u, H / 2);

    auto createFB = [&](VkRenderPass rp, VkImageView view, uint32_t w, uint32_t h) -> VkFramebuffer {
        VkFramebufferCreateInfo fi{}; fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fi.renderPass = rp; fi.attachmentCount = 1; fi.pAttachments = &view;
        fi.width = w; fi.height = h; fi.layers = 1;
        VkFramebuffer fb = VK_NULL_HANDLE;
        vkCreateFramebuffer(dev, &fi, nullptr, &fb);
        return fb;
    };

    _deferredHDRFramebuffer = createFB(_ppRenderPass, _hdrView, W, H);
    if (!_deferredHDRFramebuffer) return false;

    if (_mbView) _mbFB = createFB(_ppRenderPass, _mbView, W, H);

    for (int i = 0; i < 2; i++) {
        _taaFramebuffer[i] = createFB(_ppRenderPass, _taaHistoryView[i], W, H);
        if (!_taaFramebuffer[i]) return false;
    }

    _bloomBrightFB = createFB(_ppRenderPass, _bloomBrightView, halfW, halfH);
    _bloomBlurFB   = createFB(_ppRenderPass, _bloomBlurView, halfW, halfH);

    return true;
}

void VulkanPostProcess::DestroyFramebuffers()
{
    if (!_core) return;
    VkDevice dev = _core->GetDevice();

    if (_bloomBlurFB)   { vkDestroyFramebuffer(dev, _bloomBlurFB, nullptr);   _bloomBlurFB = VK_NULL_HANDLE; }
    if (_bloomBrightFB) { vkDestroyFramebuffer(dev, _bloomBrightFB, nullptr); _bloomBrightFB = VK_NULL_HANDLE; }
    for (int i = 0; i < 2; i++)
        if (_taaFramebuffer[i]) { vkDestroyFramebuffer(dev, _taaFramebuffer[i], nullptr); _taaFramebuffer[i] = VK_NULL_HANDLE; }
    if (_mbFB) { vkDestroyFramebuffer(dev, _mbFB, nullptr); _mbFB = VK_NULL_HANDLE; }
    if (_deferredHDRFramebuffer) { vkDestroyFramebuffer(dev, _deferredHDRFramebuffer, nullptr); _deferredHDRFramebuffer = VK_NULL_HANDLE; }
}

// ===========================================================================
// SSR Resources
// ===========================================================================

bool VulkanPostProcess::CreateSSRResources()
{
    VkDevice dev = _core->GetDevice();

    // DSL: UBO(0) + 4 SAMPLED_IMAGE(1-4) + STORAGE_IMAGE(5) + 2 SAMPLER(6-7)
    VkDescriptorSetLayoutBinding bs[8]{};
    bs[0] = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    bs[1] = { 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,   1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    bs[2] = { 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,   1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    bs[3] = { 3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,   1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    bs[4] = { 4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,   1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    bs[5] = { 5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,   1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    bs[6] = { 6, VK_DESCRIPTOR_TYPE_SAMPLER,         1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    bs[7] = { 7, VK_DESCRIPTOR_TYPE_SAMPLER,         1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    VkDescriptorSetLayoutCreateInfo li{}; li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 8; li.pBindings = bs;
    if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &_ssrLayout) != VK_SUCCESS) return false;

    VkPipelineLayoutCreateInfo pli{}; pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1; pli.pSetLayouts = &_ssrLayout;
    if (vkCreatePipelineLayout(dev, &pli, nullptr, &_ssrPipeLayout) != VK_SUCCESS) return false;

    // Descriptor pool
    VkDescriptorPoolSize ps[] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, FRAMES_IN_FLIGHT },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  4 * FRAMES_IN_FLIGHT },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  FRAMES_IN_FLIGHT },
        { VK_DESCRIPTOR_TYPE_SAMPLER,        2 * FRAMES_IN_FLIGHT },
    };
    VkDescriptorPoolCreateInfo dpi{}; dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.poolSizeCount = 4; dpi.pPoolSizes = ps;
    dpi.maxSets = FRAMES_IN_FLIGHT;
    if (vkCreateDescriptorPool(dev, &dpi, nullptr, &_ssrDescPool) != VK_SUCCESS) return false;

    // Per-frame UBOs + descriptor sets
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
        _core->CreateBuffer(256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            _ssrCB[i], _ssrCBMem[i]);
        vkMapMemory(dev, _ssrCBMem[i], 0, 256, 0, &_ssrCBMapped[i]);

        VkDescriptorSetAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = _ssrDescPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &_ssrLayout;
        vkAllocateDescriptorSets(dev, &ai, &_ssrDescSet[i]);

        VkDescriptorBufferInfo ubi{ _ssrCB[i], 0, 256 };
        VkDescriptorImageInfo  di { VK_NULL_HANDLE, _depthView,       VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo  ni { VK_NULL_HANDLE, _normalView,      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo  mri{ VK_NULL_HANDLE, _metalRoughView,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo  hi { VK_NULL_HANDLE, _hdrView,         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo  oi { VK_NULL_HANDLE, _ssrView,         VK_IMAGE_LAYOUT_GENERAL };
        VkDescriptorImageInfo  s0 { _pointClampSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
        VkDescriptorImageInfo  s1 { _linearSampler,     VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };

        VkWriteDescriptorSet ws[8]{};
        ws[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _ssrDescSet[i], 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &ubi, nullptr };
        ws[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _ssrDescSet[i], 1, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  &di,  nullptr, nullptr };
        ws[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _ssrDescSet[i], 2, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  &ni,  nullptr, nullptr };
        ws[3] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _ssrDescSet[i], 3, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  &mri, nullptr, nullptr };
        ws[4] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _ssrDescSet[i], 4, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  &hi,  nullptr, nullptr };
        ws[5] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _ssrDescSet[i], 5, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  &oi,  nullptr, nullptr };
        ws[6] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _ssrDescSet[i], 6, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER,        &s0,  nullptr, nullptr };
        ws[7] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _ssrDescSet[i], 7, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER,        &s1,  nullptr, nullptr };
        vkUpdateDescriptorSets(dev, 8, ws, 0, nullptr);
    }

    // Compile SSR compute shader
    std::vector<uint32_t> csS;
    if (!CompileGLSLtoSPIRV(GetShaderFullPath(L"ssr_vk.comp.glsl").wstring(), csS)) {
        LUNA_LOG_WARN("VK PP: SSR compute shader compile failed — SSR will be skipped");
        return true;  // Non-fatal
    }

    VkShaderModuleCreateInfo si{}; si.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    si.codeSize = csS.size() * 4; si.pCode = csS.data();
    VkShaderModule csM = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &si, nullptr, &csM);

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = csM;
    stage.pName  = "main";

    VkComputePipelineCreateInfo cpi{}; cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpi.stage  = stage;
    cpi.layout = _ssrPipeLayout;
    vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &_ssrPipeline);
    vkDestroyShaderModule(dev, csM, nullptr);

    return true;
}

void VulkanPostProcess::DestroySSRResources()
{
    if (!_core) return;
    VkDevice dev = _core->GetDevice();

    if (_ssrPipeline)   { vkDestroyPipeline(dev, _ssrPipeline, nullptr);           _ssrPipeline   = VK_NULL_HANDLE; }
    if (_ssrPipeLayout) { vkDestroyPipelineLayout(dev, _ssrPipeLayout, nullptr);   _ssrPipeLayout = VK_NULL_HANDLE; }
    if (_ssrDescPool)   { vkDestroyDescriptorPool(dev, _ssrDescPool, nullptr);     _ssrDescPool   = VK_NULL_HANDLE; }
    if (_ssrLayout)     { vkDestroyDescriptorSetLayout(dev, _ssrLayout, nullptr);  _ssrLayout     = VK_NULL_HANDLE; }
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
        if (_ssrCBMapped[i]) { vkUnmapMemory(dev, _ssrCBMem[i]); _ssrCBMapped[i] = nullptr; }
        if (_ssrCB[i])    { vkDestroyBuffer(dev, _ssrCB[i], nullptr);  _ssrCB[i]    = VK_NULL_HANDLE; }
        if (_ssrCBMem[i]) { vkFreeMemory(dev, _ssrCBMem[i], nullptr);  _ssrCBMem[i] = VK_NULL_HANDLE; }
        _ssrDescSet[i] = VK_NULL_HANDLE;
    }
}

// ===========================================================================
// Motion Blur Resources
// ===========================================================================

bool VulkanPostProcess::CreateMotionBlurResources()
{
    VkDevice dev = _core->GetDevice();

    // DSL: UBO(0) + SAMPLED_IMAGE(1,2) + SAMPLER(3)
    VkDescriptorSetLayoutBinding bins[4]{};
    bins[0] = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,  1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    bins[1] = { 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,   1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    bins[2] = { 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,   1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    bins[3] = { 3, VK_DESCRIPTOR_TYPE_SAMPLER,         1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    VkDescriptorSetLayoutCreateInfo li{}; li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 4; li.pBindings = bins;
    if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &_mbLayout) != VK_SUCCESS) return false;

    VkDescriptorPoolSize ps[3]{ {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, FRAMES_IN_FLIGHT},
                                {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, FRAMES_IN_FLIGHT * 2},
                                {VK_DESCRIPTOR_TYPE_SAMPLER, FRAMES_IN_FLIGHT} };
    VkDescriptorPoolCreateInfo pi{}; pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = FRAMES_IN_FLIGHT; pi.poolSizeCount = 3; pi.pPoolSizes = ps;
    if (vkCreateDescriptorPool(dev, &pi, nullptr, &_mbDescPool) != VK_SUCCESS) return false;

    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
        _core->CreateBuffer(256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            _mbCB[i], _mbCBMem[i]);
        vkMapMemory(dev, _mbCBMem[i], 0, VK_WHOLE_SIZE, 0, &_mbCBMapped[i]);

        VkDescriptorSetAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = _mbDescPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &_mbLayout;
        vkAllocateDescriptorSets(dev, &ai, &_mbDescSet[i]);

        VkDescriptorBufferInfo ubi{ _mbCB[i], 0, sizeof(MotionBlurConstants) };
        VkDescriptorImageInfo hdrI { VK_NULL_HANDLE, _hdrView,   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo depI { VK_NULL_HANDLE, _depthView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo samI { _pointClampSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
        VkWriteDescriptorSet ws[4]{};
        ws[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _mbDescSet[i], 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &ubi, nullptr };
        ws[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _mbDescSet[i], 1, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &hdrI, nullptr, nullptr };
        ws[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _mbDescSet[i], 2, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &depI, nullptr, nullptr };
        ws[3] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _mbDescSet[i], 3, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER, &samI, nullptr, nullptr };
        vkUpdateDescriptorSets(dev, 4, ws, 0, nullptr);
    }

    VkPipelineLayoutCreateInfo pli{}; pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1; pli.pSetLayouts = &_mbLayout;
    if (vkCreatePipelineLayout(dev, &pli, nullptr, &_mbPipeLayout) != VK_SUCCESS) return false;

    return true;
}

void VulkanPostProcess::DestroyMotionBlurResources()
{
    if (!_core) return;
    VkDevice dev = _core->GetDevice();

    if (_mbPipeline)   { vkDestroyPipeline(dev, _mbPipeline, nullptr);           _mbPipeline   = VK_NULL_HANDLE; }
    if (_mbPipeLayout) { vkDestroyPipelineLayout(dev, _mbPipeLayout, nullptr);   _mbPipeLayout = VK_NULL_HANDLE; }
    if (_mbDescPool)   { vkDestroyDescriptorPool(dev, _mbDescPool, nullptr);     _mbDescPool   = VK_NULL_HANDLE; }
    if (_mbLayout)     { vkDestroyDescriptorSetLayout(dev, _mbLayout, nullptr);  _mbLayout     = VK_NULL_HANDLE; }
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
        if (_mbCBMapped[i]) { vkUnmapMemory(dev, _mbCBMem[i]); _mbCBMapped[i] = nullptr; }
        if (_mbCB[i])    { vkDestroyBuffer(dev, _mbCB[i], nullptr);  _mbCB[i]    = VK_NULL_HANDLE; }
        if (_mbCBMem[i]) { vkFreeMemory(dev, _mbCBMem[i], nullptr);  _mbCBMem[i] = VK_NULL_HANDLE; }
        _mbDescSet[i] = VK_NULL_HANDLE;
    }
}

// ===========================================================================
// TAA Resources
// ===========================================================================

bool VulkanPostProcess::CreateTAAResources()
{
    VkDevice dev = _core->GetDevice();

    // DSL: UBO(0) + currentFrame(1) + historyFrame(2) + depth(3) + bilinear(4) + point(5)
    VkDescriptorSetLayoutBinding bs[6]{};
    bs[0] = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    bs[1] = { 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    bs[2] = { 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    bs[3] = { 3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    bs[4] = { 4, VK_DESCRIPTOR_TYPE_SAMPLER,        1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    bs[5] = { 5, VK_DESCRIPTOR_TYPE_SAMPLER,        1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    VkDescriptorSetLayoutCreateInfo li{}; li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 6; li.pBindings = bs;
    if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &_taaLayout) != VK_SUCCESS) return false;

    VkPipelineLayoutCreateInfo pli{}; pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1; pli.pSetLayouts = &_taaLayout;
    if (vkCreatePipelineLayout(dev, &pli, nullptr, &_taaPipeLayout) != VK_SUCCESS) return false;

    // Descriptor pool
    VkDescriptorPoolSize ps[] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, FRAMES_IN_FLIGHT },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  FRAMES_IN_FLIGHT * 3 },
        { VK_DESCRIPTOR_TYPE_SAMPLER,        FRAMES_IN_FLIGHT * 2 },
    };
    VkDescriptorPoolCreateInfo dpi{}; dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.poolSizeCount = 3; dpi.pPoolSizes = ps;
    dpi.maxSets = FRAMES_IN_FLIGHT;
    if (vkCreateDescriptorPool(dev, &dpi, nullptr, &_taaDescPool) != VK_SUCCESS) return false;

    // Per-frame TAA UBOs + descriptor sets
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
        _core->CreateBuffer(sizeof(TAAConstants),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            _taaCB[i], _taaCBMem[i]);
        vkMapMemory(dev, _taaCBMem[i], 0, sizeof(TAAConstants), 0, &_taaCBMapped[i]);

        VkDescriptorSetAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = _taaDescPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &_taaLayout;
        vkAllocateDescriptorSets(dev, &ai, &_taaDescSet[i]);

        VkDescriptorBufferInfo ubi{ _taaCB[i], 0, sizeof(TAAConstants) };
        VkImageView curFrameView = (_mbView != VK_NULL_HANDLE) ? _mbView : _hdrView;
        VkDescriptorImageInfo curImg  { VK_NULL_HANDLE, curFrameView,       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo histImg { VK_NULL_HANDLE, _taaHistoryView[1], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo depthImg{ VK_NULL_HANDLE, _depthView,         VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo bilinear{ _linearSampler,     VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
        VkDescriptorImageInfo point   { _pointClampSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
        VkWriteDescriptorSet ws[6]{};
        ws[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _taaDescSet[i], 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &ubi, nullptr };
        ws[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _taaDescSet[i], 1, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &curImg, nullptr, nullptr };
        ws[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _taaDescSet[i], 2, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &histImg, nullptr, nullptr };
        ws[3] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _taaDescSet[i], 3, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &depthImg, nullptr, nullptr };
        ws[4] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _taaDescSet[i], 4, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER, &bilinear, nullptr, nullptr };
        ws[5] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _taaDescSet[i], 5, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER, &point, nullptr, nullptr };
        vkUpdateDescriptorSets(dev, 6, ws, 0, nullptr);
    }

    return true;
}

void VulkanPostProcess::DestroyTAAResources()
{
    if (!_core) return;
    VkDevice dev = _core->GetDevice();

    if (_taaPipeline)   { vkDestroyPipeline(dev, _taaPipeline, nullptr);           _taaPipeline   = VK_NULL_HANDLE; }
    if (_taaPipeLayout) { vkDestroyPipelineLayout(dev, _taaPipeLayout, nullptr);   _taaPipeLayout = VK_NULL_HANDLE; }
    if (_taaDescPool)   { vkDestroyDescriptorPool(dev, _taaDescPool, nullptr);     _taaDescPool   = VK_NULL_HANDLE; }
    if (_taaLayout)     { vkDestroyDescriptorSetLayout(dev, _taaLayout, nullptr);  _taaLayout     = VK_NULL_HANDLE; }
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
        if (_taaCBMapped[i]) { vkUnmapMemory(dev, _taaCBMem[i]); _taaCBMapped[i] = nullptr; }
        if (_taaCB[i])    { vkDestroyBuffer(dev, _taaCB[i], nullptr);  _taaCB[i]    = VK_NULL_HANDLE; }
        if (_taaCBMem[i]) { vkFreeMemory(dev, _taaCBMem[i], nullptr);  _taaCBMem[i] = VK_NULL_HANDLE; }
        _taaDescSet[i] = VK_NULL_HANDLE;
    }
}

// ===========================================================================
// Bloom Resources
// ===========================================================================

bool VulkanPostProcess::CreateBloomResources()
{
    VkDevice dev = _core->GetDevice();

    // DSL: 1 SAMPLED_IMAGE + 1 SAMPLER
    VkDescriptorSetLayoutBinding bs[2]{};
    bs[0] = { 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    bs[1] = { 1, VK_DESCRIPTOR_TYPE_SAMPLER,       1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    VkDescriptorSetLayoutCreateInfo li{}; li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 2; li.pBindings = bs;
    if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &_bloomLayout) != VK_SUCCESS) return false;

    VkPushConstantRange pcr{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16 };
    VkPipelineLayoutCreateInfo pli{}; pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1; pli.pSetLayouts = &_bloomLayout;
    pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(dev, &pli, nullptr, &_bloomPipeLayout) != VK_SUCCESS) return false;

    // Descriptor pool: 4 sets (2 bloomBright + blurH + blurV)
    VkDescriptorPoolSize ps[] = {
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4 },
        { VK_DESCRIPTOR_TYPE_SAMPLER,       4 },
    };
    VkDescriptorPoolCreateInfo dpi{}; dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.poolSizeCount = 2; dpi.pPoolSizes = ps;
    dpi.maxSets = 4;
    if (vkCreateDescriptorPool(dev, &dpi, nullptr, &_bloomDescPool) != VK_SUCCESS) return false;

    auto allocSet = [&](VkDescriptorSet& outSet, VkImageView view) {
        VkDescriptorSetAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = _bloomDescPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &_bloomLayout;
        vkAllocateDescriptorSets(dev, &ai, &outSet);
        VkDescriptorImageInfo img{ VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo sam{ _linearSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
        VkWriteDescriptorSet ws[2]{};
        ws[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, outSet, 0, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &img, nullptr, nullptr };
        ws[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, outSet, 1, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER, &sam, nullptr, nullptr };
        vkUpdateDescriptorSets(dev, 2, ws, 0, nullptr);
    };

    allocSet(_bloomBrightDescSet[0], _taaHistoryView[0]);
    allocSet(_bloomBrightDescSet[1], _taaHistoryView[1]);
    allocSet(_bloomBlurHDescSet, _bloomBrightView);
    allocSet(_bloomBlurVDescSet, _bloomBlurView);

    return true;
}

void VulkanPostProcess::DestroyBloomResources()
{
    if (!_core) return;
    VkDevice dev = _core->GetDevice();

    if (_bloomBrightPipeline) { vkDestroyPipeline(dev, _bloomBrightPipeline, nullptr); _bloomBrightPipeline = VK_NULL_HANDLE; }
    if (_bloomBlurPipeline)   { vkDestroyPipeline(dev, _bloomBlurPipeline, nullptr);   _bloomBlurPipeline   = VK_NULL_HANDLE; }
    if (_bloomPipeLayout)     { vkDestroyPipelineLayout(dev, _bloomPipeLayout, nullptr); _bloomPipeLayout = VK_NULL_HANDLE; }
    if (_bloomDescPool)       { vkDestroyDescriptorPool(dev, _bloomDescPool, nullptr);   _bloomDescPool = VK_NULL_HANDLE; }
    if (_bloomLayout)         { vkDestroyDescriptorSetLayout(dev, _bloomLayout, nullptr); _bloomLayout = VK_NULL_HANDLE; }
    for (int i = 0; i < 2; i++) _bloomBrightDescSet[i] = VK_NULL_HANDLE;
    _bloomBlurHDescSet = VK_NULL_HANDLE;
    _bloomBlurVDescSet = VK_NULL_HANDLE;
}

// ===========================================================================
// Tonemap Resources
// ===========================================================================

bool VulkanPostProcess::CreateTonemapResources()
{
    VkDevice dev = _core->GetDevice();

    // DSL: 3 SAMPLED_IMAGE + 1 SAMPLER (TAA + bloom + SSR)
    VkDescriptorSetLayoutBinding bs[4]{};
    bs[0] = { 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    bs[1] = { 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    bs[2] = { 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    bs[3] = { 3, VK_DESCRIPTOR_TYPE_SAMPLER,       1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    VkDescriptorSetLayoutCreateInfo li{}; li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 4; li.pBindings = bs;
    if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &_tonemapLayout) != VK_SUCCESS) return false;

    VkPushConstantRange pcr{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16 };
    VkPipelineLayoutCreateInfo pli{}; pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1; pli.pSetLayouts = &_tonemapLayout;
    pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(dev, &pli, nullptr, &_tonemapPipeLayout) != VK_SUCCESS) return false;

    // Descriptor pool
    VkDescriptorPoolSize ps[] = {
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 6 },  // 2 sets × 3
        { VK_DESCRIPTOR_TYPE_SAMPLER,       2 },
    };
    VkDescriptorPoolCreateInfo dpi{}; dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.poolSizeCount = 2; dpi.pPoolSizes = ps;
    dpi.maxSets = 2;
    if (vkCreateDescriptorPool(dev, &dpi, nullptr, &_tonemapDescPool) != VK_SUCCESS) return false;

    // 2 sets (ping-pong)
    for (int i = 0; i < 2; i++) {
        VkDescriptorSetAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = _tonemapDescPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &_tonemapLayout;
        vkAllocateDescriptorSets(dev, &ai, &_tonemapDescSet[i]);

        VkDescriptorImageInfo taaImg  { VK_NULL_HANDLE, _taaHistoryView[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo bloomImg{ VK_NULL_HANDLE, _bloomBrightView,   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo ssrImg  { VK_NULL_HANDLE, _ssrView,           VK_IMAGE_LAYOUT_GENERAL };
        VkDescriptorImageInfo sam     { _pointClampSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
        VkWriteDescriptorSet ws[4]{};
        ws[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _tonemapDescSet[i], 0, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &taaImg, nullptr, nullptr };
        ws[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _tonemapDescSet[i], 1, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &bloomImg, nullptr, nullptr };
        ws[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _tonemapDescSet[i], 2, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &ssrImg, nullptr, nullptr };
        ws[3] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _tonemapDescSet[i], 3, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER, &sam, nullptr, nullptr };
        vkUpdateDescriptorSets(dev, 4, ws, 0, nullptr);
    }

    // Compile and create pipelines
    std::vector<uint32_t> vsS, taaFS, bloomBrightFS, bloomBlurFS, tonemapFS, mbFS;
    bool shadersOK =
        CompileHLSLtoSPIRV(GetShaderFullPath(L"fullscreen.vert.hlsl").wstring(), L"vs_6_0", vsS) &&
        CompileHLSLtoSPIRV(GetShaderFullPath(L"taa.frag.hlsl").wstring(), L"ps_6_0", taaFS) &&
        CompileHLSLtoSPIRV(GetShaderFullPath(L"bloom_bright.frag.hlsl").wstring(), L"ps_6_0", bloomBrightFS) &&
        CompileHLSLtoSPIRV(GetShaderFullPath(L"bloom_blur.frag.hlsl").wstring(), L"ps_6_0", bloomBlurFS) &&
        CompileGLSLtoSPIRV(GetShaderFullPath(L"tonemapping_vk_full.frag.glsl").wstring(), tonemapFS);

    if (!shadersOK) {
        LUNA_LOG_WARN("VK PP: shader compile failed — TAA + bloom disabled");
        return true;  // Non-fatal
    }

    // Shader module helper
    auto mkMod = [&](const std::vector<uint32_t>& sp) -> VkShaderModule {
        VkShaderModuleCreateInfo si{}; si.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        si.codeSize = sp.size() * 4; si.pCode = sp.data();
        VkShaderModule m = VK_NULL_HANDLE;
        vkCreateShaderModule(dev, &si, nullptr, &m);
        return m;
    };

    // Pipeline builder
    auto mkGfxPipeline = [&](VkShaderModule vsM, VkShaderModule fsM,
                             VkPipelineLayout layout, VkRenderPass rp) -> VkPipeline
    {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vsM, "main" };
        stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fsM, "main" };
        VkPipelineVertexInputStateCreateInfo vis{}; vis.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo ias{}; ias.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ias.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vps{}; vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vps.viewportCount = 1; vps.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{}; rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.f;
        VkPipelineMultisampleStateCreateInfo ms{}; ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{}; ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        VkPipelineColorBlendAttachmentState cba{}; cba.colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo cbs{}; cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cbs.attachmentCount = 1; cbs.pAttachments = &cba;
        VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dsi{}; dsi.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dsi.dynamicStateCount = 2; dsi.pDynamicStates = dyn;
        VkGraphicsPipelineCreateInfo gpi{}; gpi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gpi.stageCount = 2; gpi.pStages = stages;
        gpi.pVertexInputState = &vis; gpi.pInputAssemblyState = &ias;
        gpi.pViewportState = &vps; gpi.pRasterizationState = &rs;
        gpi.pMultisampleState = &ms; gpi.pDepthStencilState = &ds;
        gpi.pColorBlendState = &cbs; gpi.pDynamicState = &dsi;
        gpi.layout = layout; gpi.renderPass = rp;
        VkPipeline p = VK_NULL_HANDLE;
        vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, nullptr, &p);
        return p;
    };

    VkShaderModule vsM            = mkMod(vsS);
    VkShaderModule taaFsM         = mkMod(taaFS);
    VkShaderModule bloomBrightFsM = mkMod(bloomBrightFS);
    VkShaderModule bloomBlurFsM   = mkMod(bloomBlurFS);
    VkShaderModule tonemapFsM     = mkMod(tonemapFS);

    _taaPipeline         = mkGfxPipeline(vsM, taaFsM,         _taaPipeLayout,     _ppRenderPass);
    _bloomBrightPipeline = mkGfxPipeline(vsM, bloomBrightFsM, _bloomPipeLayout,   _ppRenderPass);
    _bloomBlurPipeline   = mkGfxPipeline(vsM, bloomBlurFsM,   _bloomPipeLayout,   _ppRenderPass);
    _tonemapPipeline     = mkGfxPipeline(vsM, tonemapFsM,     _tonemapPipeLayout, _tonemapRenderPass);

    // Motion blur pipeline
    if (_mbPipeLayout && _mbFB) {
        if (CompileHLSLtoSPIRV(GetShaderFullPath(L"motion_blur.frag.hlsl").wstring(), L"ps_6_0", mbFS)) {
            VkShaderModule mbFsM = mkMod(mbFS);
            _mbPipeline = mkGfxPipeline(vsM, mbFsM, _mbPipeLayout, _ppRenderPass);
            vkDestroyShaderModule(dev, mbFsM, nullptr);
        } else
            LUNA_LOG_WARN("VK PP: motion_blur.frag.hlsl compile failed — motion blur disabled");
    }

    vkDestroyShaderModule(dev, vsM, nullptr);
    vkDestroyShaderModule(dev, taaFsM, nullptr);
    vkDestroyShaderModule(dev, bloomBrightFsM, nullptr);
    vkDestroyShaderModule(dev, bloomBlurFsM, nullptr);
    vkDestroyShaderModule(dev, tonemapFsM, nullptr);

    if (_taaPipeline && _bloomBrightPipeline && _bloomBlurPipeline && _tonemapPipeline) {
        uint32_t halfW = std::max(1u, _extent.width / 2), halfH = std::max(1u, _extent.height / 2);
        LUNA_LOG_INFO("VK PP: TAA + Bloom + full tonemap ready (%ux%u, bloom half=%ux%u)", 
                      _extent.width, _extent.height, halfW, halfH);
    }

    return true;
}

void VulkanPostProcess::DestroyTonemapResources()
{
    if (!_core) return;
    VkDevice dev = _core->GetDevice();

    if (_tonemapPipeline)   { vkDestroyPipeline(dev, _tonemapPipeline, nullptr); _tonemapPipeline = VK_NULL_HANDLE; }
    if (_tonemapPipeLayout) { vkDestroyPipelineLayout(dev, _tonemapPipeLayout, nullptr); _tonemapPipeLayout = VK_NULL_HANDLE; }
    if (_tonemapDescPool)   { vkDestroyDescriptorPool(dev, _tonemapDescPool, nullptr); _tonemapDescPool = VK_NULL_HANDLE; }
    if (_tonemapLayout)     { vkDestroyDescriptorSetLayout(dev, _tonemapLayout,nullptr); _tonemapLayout = VK_NULL_HANDLE; }
    for (int i = 0; i < 2; i++) _tonemapDescSet[i] = VK_NULL_HANDLE;
}

// ===========================================================================
// Descriptor Updates (per-frame ping-pong)
// ===========================================================================

void VulkanPostProcess::UpdateDescriptors(uint32_t frameIndex)
{
    if (!_taaPipeline || !_ready) { _frameCount++; return; }
    VkDevice dev = _core->GetDevice();

    _taaHistoryIndex = (int)(_frameCount & 1);
    int readIdx = _taaHistoryIndex ^ 1;

    VkDescriptorImageInfo histImg{ VK_NULL_HANDLE, _taaHistoryView[readIdx], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet w{};
    w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet          = _taaDescSet[frameIndex];
    w.dstBinding      = 2;
    w.descriptorCount = 1;
    w.descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w.pImageInfo      = &histImg;
    vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);

    _frameCount++;
}

// ===========================================================================
// Pass Execution
// ===========================================================================

void VulkanPostProcess::DrawSSR(VkCommandBuffer cmd, uint32_t frameIndex,
                                const XMFLOAT4X4& view, const XMFLOAT4X4& proj)
{
    if (!_ssrPipeline) return;

    // Upload SSR UBO (simplified; full implementation would match existing)
    struct { float vp[16]; float ivp[16]; float proj[4]; float screenSize[2]; float _pad[2]; } ub{};
    XMMATRIX V = XMLoadFloat4x4(&view);
    XMMATRIX P = XMLoadFloat4x4(&proj);
    XMMATRIX VP = XMMatrixMultiply(V, P);
    XMMATRIX iVP = XMMatrixInverse(nullptr, VP);
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(ub.vp), VP);
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(ub.ivp), iVP);
    ub.proj[0] = proj.m[0][0]; ub.proj[1] = proj.m[1][1];
    ub.proj[2] = proj.m[2][2]; ub.proj[3] = proj.m[2][3];
    ub.screenSize[0] = (float)_extent.width;
    ub.screenSize[1] = (float)_extent.height;
    memcpy(_ssrCBMapped[frameIndex], &ub, sizeof(ub));

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _ssrPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _ssrPipeLayout, 0, 1, &_ssrDescSet[frameIndex], 0, nullptr);
    uint32_t gx = (_extent.width + 7) / 8, gy = (_extent.height + 7) / 8;
    vkCmdDispatch(cmd, gx, gy, 1);
}

void VulkanPostProcess::DrawMotionBlur(VkCommandBuffer cmd, uint32_t frameIndex,
                                       const XMFLOAT4X4& view, const XMFLOAT4X4& proj,
                                       const XMFLOAT4X4& prevViewProj)
{
    if (!_mbPipeline || !_mbFB || !_mbView) return;
    uint32_t W = _extent.width, H = _extent.height;

    XMMATRIX V = XMLoadFloat4x4(&view);
    XMMATRIX P = XMLoadFloat4x4(&proj);
    XMMATRIX VP = XMMatrixMultiply(V, P);
    XMMATRIX iVP = XMMatrixInverse(nullptr, VP);

    MotionBlurConstants cb{};
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb.invViewProj), iVP);
    memcpy(cb.prevViewProj, &prevViewProj, 64);
    cb.screenSizeX  = (float)W;
    cb.screenSizeY  = (float)H;
    cb.shutterScale = 0.5f;
    cb.numSamples   = 8;
    memcpy(_mbCBMapped[frameIndex], &cb, sizeof(cb));

    VkClearValue cv{}; cv.color = { {0,0,0,1} };
    VkRenderPassBeginInfo rpi{}; rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpi.renderPass = _ppRenderPass;
    rpi.framebuffer = _mbFB;
    rpi.renderArea.extent = { W, H };
    rpi.clearValueCount = 1;
    rpi.pClearValues = &cv;
    vkCmdBeginRenderPass(cmd, &rpi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{ 0, 0, (float)W, (float)H, 0, 1 };
    VkRect2D sc{ {0,0}, {W, H} };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _mbPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _mbPipeLayout, 0, 1, &_mbDescSet[frameIndex], 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdEndRenderPass(cmd);
}

void VulkanPostProcess::DrawTAA(VkCommandBuffer cmd, uint32_t frameIndex, const TAAConstants& taaConst)
{
    if (!_taaPipeline) return;
    uint32_t W = _extent.width, H = _extent.height;

    memcpy(_taaCBMapped[frameIndex], &taaConst, sizeof(TAAConstants));

    // Store unjittered VP for next frame
    memcpy(_prevUnjitteredVP, _unjitteredVP, 64);
    _prevJitter[0] = _curJitter[0];
    _prevJitter[1] = _curJitter[1];

    VkClearValue clear{};
    VkRenderPassBeginInfo rpi{};
    rpi.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpi.renderPass        = _ppRenderPass;
    rpi.framebuffer       = _taaFramebuffer[_taaHistoryIndex];
    rpi.renderArea.extent = _extent;
    rpi.clearValueCount   = 1;
    rpi.pClearValues      = &clear;
    vkCmdBeginRenderPass(cmd, &rpi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{ 0, 0, (float)W, (float)H, 0, 1 };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc{ {0,0}, _extent };
    vkCmdSetScissor(cmd, 0, 1, &sc);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _taaPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _taaPipeLayout, 0, 1, &_taaDescSet[frameIndex], 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
}

void VulkanPostProcess::DrawBloomBright(VkCommandBuffer cmd)
{
    if (!_bloomBrightPipeline) return;
    uint32_t halfW = std::max(1u, _extent.width / 2), halfH = std::max(1u, _extent.height / 2);

    VkClearValue clear{};
    VkRenderPassBeginInfo rpi{}; rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpi.renderPass = _ppRenderPass;
    rpi.framebuffer = _bloomBrightFB;
    rpi.renderArea.extent = { halfW, halfH };
    rpi.clearValueCount = 1; rpi.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rpi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{ 0, 0, (float)halfW, (float)halfH, 0, 1 };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc{ {0,0}, {halfW, halfH} };
    vkCmdSetScissor(cmd, 0, 1, &sc);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _bloomBrightPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _bloomPipeLayout, 0, 1, &_bloomBrightDescSet[_taaHistoryIndex], 0, nullptr);
    struct { float threshold; float knee; float pad[2]; } pc{ 0.8f, 0.1f, {0.f, 0.f} };
    vkCmdPushConstants(cmd, _bloomPipeLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
}

void VulkanPostProcess::DrawBloomBlur(VkCommandBuffer cmd, bool horizontal)
{
    if (!_bloomBlurPipeline) return;
    uint32_t halfW = std::max(1u, _extent.width / 2), halfH = std::max(1u, _extent.height / 2);

    VkDescriptorSet srcSet = horizontal ? _bloomBlurHDescSet : _bloomBlurVDescSet;
    VkFramebuffer   dstFB  = horizontal ? _bloomBlurFB       : _bloomBrightFB;

    VkClearValue clear{};
    VkRenderPassBeginInfo rpi{}; rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpi.renderPass = _ppRenderPass;
    rpi.framebuffer = dstFB;
    rpi.renderArea.extent = { halfW, halfH };
    rpi.clearValueCount = 1; rpi.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rpi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{ 0, 0, (float)halfW, (float)halfH, 0, 1 };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc{ {0,0}, {halfW, halfH} };
    vkCmdSetScissor(cmd, 0, 1, &sc);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _bloomBlurPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _bloomPipeLayout, 0, 1, &srcSet, 0, nullptr);
    struct { float texelX; float texelY; float pad[2]; } pc{};
    if (horizontal) { pc.texelX = 1.0f / (float)halfW; }
    else            { pc.texelY = 1.0f / (float)halfH; }
    vkCmdPushConstants(cmd, _bloomPipeLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
}

void VulkanPostProcess::DrawTonemap(VkCommandBuffer cmd, uint32_t imageIndex)
{
    if (!_tonemapPipeline || _tonemapFramebuffers.empty()) return;

    VkClearValue clear{};
    VkRenderPassBeginInfo rpi{}; rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpi.renderPass = _tonemapRenderPass;
    rpi.framebuffer = _tonemapFramebuffers[imageIndex];
    rpi.renderArea.extent = _extent;
    rpi.clearValueCount = 1; rpi.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rpi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{ 0, 0, (float)_extent.width, (float)_extent.height, 0, 1 };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc{ {0,0}, _extent };
    vkCmdSetScissor(cmd, 0, 1, &sc);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _tonemapPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _tonemapPipeLayout, 0, 1, &_tonemapDescSet[_taaHistoryIndex], 0, nullptr);
    struct { float bloomStrength; float exposure; float pad[2]; } pc{ 0.04f, 1.0f, {0.f, 0.f} };
    vkCmdPushConstants(cmd, _tonemapPipeLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
}

// ===========================================================================
// Helpers
// ===========================================================================

void VulkanPostProcess::SetJitter(float curX, float curY, float prevX, float prevY)
{
    _curJitter[0] = curX;  _curJitter[1] = curY;
    _prevJitter[0] = prevX; _prevJitter[1] = prevY;
}

void VulkanPostProcess::SetUnjitteredVP(const float* vp16)
{
    memcpy(_unjitteredVP, vp16, 64);
}

} // namespace Luna




