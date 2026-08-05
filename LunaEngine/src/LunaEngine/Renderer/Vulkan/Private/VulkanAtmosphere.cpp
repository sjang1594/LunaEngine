// VulkanAtmosphere.cpp — Phase 28: Physically-based atmosphere (Hillaire 2020)
#include "LunaPCH.h"
#include "LunaEngine/Renderer/Vulkan/Public/VulkanAtmosphere.h"
#include "LunaEngine/Renderer/Vulkan/Public/VulkanCore.h"
#include "LunaEngine/Utils/FileSystemUtil.h"
#include "Logger/Logger.h"
#include <vector>
#include <cmath>

namespace Luna
{

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
                     + L" -I\"" + shaderDir + L"\""  // for atmosphere_common.glsl include
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
        LUNA_LOG_ERROR("VK Atmo: shader compile failed: %s", errMsg.c_str());
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

VulkanAtmosphere::~VulkanAtmosphere() { Destroy(); }

bool VulkanAtmosphere::Create(const CreateInfo& info)
{
    _core = info.core;
    _framesInFlight = info.framesInFlight;
    _extent = info.extent;
    _hdrView = info.hdrView;
    _hdrImage = info.hdrImage;
    _depthView = info.depthView;
    _compositeRenderPass = info.ppRenderPass;

    SetSunElevationAzimuth(45.0f, 180.0f);

    auto fail = [&](const char* msg) { LUNA_LOG_ERROR("%s", msg); Destroy(); return false; };
    if (!CreateLUTImages())             return fail("VK Atmo: LUT image creation failed");
    if (!CreateSampler())               return fail("VK Atmo: sampler creation failed");
    if (!CreateUBO())                   return fail("VK Atmo: UBO creation failed");
    if (!CreateDescriptors())           return fail("VK Atmo: descriptor creation failed");
    if (!CreateComputePipelines())      return fail("VK Atmo: compute pipeline creation failed");
    if (!CreateAtmosphereRenderPass())  return fail("VK Atmo: atmosphere render pass creation failed");
    if (!CreateCompositePipeline())     return fail("VK Atmo: composite pipeline creation failed");

    VkCommandBuffer cmd = _core->BeginSingleTimeCommands();
    Precompute(cmd);
    _core->EndSingleTimeCommands(cmd);
    _precomputed = true;

    _ready = true;
    LUNA_LOG_INFO("VK Atmosphere: initialized (transmittance %dx%d, skyView %dx%d)",
                  TRANSMITTANCE_W, TRANSMITTANCE_H, SKYVIEW_W, SKYVIEW_H);
    return true;
}

void VulkanAtmosphere::Destroy()
{
    if (!_core) return;
    VkDevice dev = _core->GetDevice();
    if (dev) vkDeviceWaitIdle(dev);

    auto destroyImg = [&](VkImage& i, VkDeviceMemory& m, VkImageView& v) {
        if (v) { vkDestroyImageView(dev, v, nullptr); v = VK_NULL_HANDLE; }
        if (i) { vkDestroyImage(dev, i, nullptr); i = VK_NULL_HANDLE; }
        if (m) { vkFreeMemory(dev, m, nullptr); m = VK_NULL_HANDLE; }
    };
    destroyImg(_transmittanceImage, _transmittanceMem, _transmittanceView);
    destroyImg(_multiScatterImage, _multiScatterMem, _multiScatterView);
    destroyImg(_skyViewImage, _skyViewMem, _skyViewView);

    if (_lutSampler) { vkDestroySampler(dev, _lutSampler, nullptr); _lutSampler = VK_NULL_HANDLE; }

    for (uint32_t i = 0; i < _framesInFlight; ++i) {
        if (_ubo[i]) { vkDestroyBuffer(dev, _ubo[i], nullptr); _ubo[i] = VK_NULL_HANDLE; }
        if (_uboMem[i]) { vkFreeMemory(dev, _uboMem[i], nullptr); _uboMem[i] = VK_NULL_HANDLE; }
    }

    auto destroyPipe = [&](VkPipeline& p, VkPipelineLayout& l) {
        if (p) { vkDestroyPipeline(dev, p, nullptr); p = VK_NULL_HANDLE; }
        if (l) { vkDestroyPipelineLayout(dev, l, nullptr); l = VK_NULL_HANDLE; }
    };
    destroyPipe(_transmittancePipeline, _transmittancePipeLayout);
    destroyPipe(_multiScatterPipeline, _multiScatterPipeLayout);
    destroyPipe(_skyViewPipeline, _skyViewPipeLayout);
    destroyPipe(_compositePipeline, _compositePipeLayout);

    auto destroyDescLayout = [&](VkDescriptorSetLayout& l) {
        if (l) { vkDestroyDescriptorSetLayout(dev, l, nullptr); l = VK_NULL_HANDLE; }
    };
    destroyDescLayout(_transmittanceDescLayout);
    destroyDescLayout(_multiScatterDescLayout);
    destroyDescLayout(_skyViewDescLayout);
    destroyDescLayout(_compositeDescLayout);

    if (_computeDescPool)      { vkDestroyDescriptorPool(dev, _computeDescPool, nullptr);      _computeDescPool = VK_NULL_HANDLE; }
    if (_compositeDescPool)    { vkDestroyDescriptorPool(dev, _compositeDescPool, nullptr);    _compositeDescPool = VK_NULL_HANDLE; }
    if (_compositeFramebuffer) { vkDestroyFramebuffer(dev, _compositeFramebuffer, nullptr);    _compositeFramebuffer = VK_NULL_HANDLE; }
    if (_atmosphereRenderPass) { vkDestroyRenderPass(dev, _atmosphereRenderPass, nullptr);     _atmosphereRenderPass = VK_NULL_HANDLE; }

    _ready = false;
    _precomputed = false;
    _skyViewReady = false;
    _core = nullptr;
}

void VulkanAtmosphere::SetSunElevationAzimuth(float elevDeg, float azimDeg)
{
    _sunElevation = elevDeg;
    _sunAzimuth   = azimDeg;
    float elRad = elevDeg * 3.14159265f / 180.0f;
    float azRad = azimDeg * 3.14159265f / 180.0f;
    _sunDir = { cosf(elRad) * cosf(azRad), sinf(elRad), cosf(elRad) * sinf(azRad) };
}

bool VulkanAtmosphere::CreateLUTImages()
{
    VkFormat fmt = VK_FORMAT_R16G16B16A16_SFLOAT;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    if (!_core->CreateImage(TRANSMITTANCE_W, TRANSMITTANCE_H, fmt, VK_IMAGE_TILING_OPTIMAL, usage,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, _transmittanceImage, _transmittanceMem)) return false;
    _transmittanceView = _core->CreateImageView(_transmittanceImage, fmt, VK_IMAGE_ASPECT_COLOR_BIT);

    if (!_core->CreateImage(MULTISCATTER_W, MULTISCATTER_H, fmt, VK_IMAGE_TILING_OPTIMAL, usage,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, _multiScatterImage, _multiScatterMem)) return false;
    _multiScatterView = _core->CreateImageView(_multiScatterImage, fmt, VK_IMAGE_ASPECT_COLOR_BIT);

    if (!_core->CreateImage(SKYVIEW_W, SKYVIEW_H, fmt, VK_IMAGE_TILING_OPTIMAL, usage,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, _skyViewImage, _skyViewMem)) return false;
    _skyViewView = _core->CreateImageView(_skyViewImage, fmt, VK_IMAGE_ASPECT_COLOR_BIT);

    _core->TransitionImageLayout(_transmittanceImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    _core->TransitionImageLayout(_multiScatterImage,  VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    _core->TransitionImageLayout(_skyViewImage,       VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

    return true;
}

bool VulkanAtmosphere::CreateSampler()
{
    VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = VK_FILTER_LINEAR;  si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    return vkCreateSampler(_core->GetDevice(), &si, nullptr, &_lutSampler) == VK_SUCCESS;
}

bool VulkanAtmosphere::CreateUBO()
{
    for (uint32_t i = 0; i < _framesInFlight; ++i) {
        if (!_core->CreateBuffer(256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                _ubo[i], _uboMem[i])) return false;
        vkMapMemory(_core->GetDevice(), _uboMem[i], 0, 256, 0, &_uboMapped[i]);
    }
    return true;
}

bool VulkanAtmosphere::CreateDescriptors()
{
    VkDevice dev = _core->GetDevice();

    // Transmittance: b0=storage(out), b1=UBO
    {
        VkDescriptorSetLayoutBinding bindings[2]{};
        bindings[0] = { 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,   1, VK_SHADER_STAGE_COMPUTE_BIT };
        bindings[1] = { 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,  1, VK_SHADER_STAGE_COMPUTE_BIT };
        VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        ci.bindingCount = 2; ci.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(dev, &ci, nullptr, &_transmittanceDescLayout) != VK_SUCCESS) return false;
    }

    // MultiScatter: b0=storage(out), b1=UBO, b2=transmittance sampler
    {
        VkDescriptorSetLayoutBinding bindings[3]{};
        bindings[0] = { 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          1, VK_SHADER_STAGE_COMPUTE_BIT };
        bindings[1] = { 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         1, VK_SHADER_STAGE_COMPUTE_BIT };
        bindings[2] = { 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT };
        VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        ci.bindingCount = 3; ci.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(dev, &ci, nullptr, &_multiScatterDescLayout) != VK_SUCCESS) return false;
    }

    // SkyView: b0=storage(out), b1=UBO, b2=transmittance, b3=multiScatter
    {
        VkDescriptorSetLayoutBinding bindings[4]{};
        bindings[0] = { 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          1, VK_SHADER_STAGE_COMPUTE_BIT };
        bindings[1] = { 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         1, VK_SHADER_STAGE_COMPUTE_BIT };
        bindings[2] = { 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT };
        bindings[3] = { 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT };
        VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        ci.bindingCount = 4; ci.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(dev, &ci, nullptr, &_skyViewDescLayout) != VK_SUCCESS) return false;
    }

    // Composite: b0=UBO, b1=skyView, b2=depth  (no sceneTex — LOAD_OP_LOAD preserves scene)
    {
        VkDescriptorSetLayoutBinding bindings[3]{};
        bindings[0] = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         1, VK_SHADER_STAGE_FRAGMENT_BIT };
        bindings[1] = { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT };
        bindings[2] = { 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT };
        VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        ci.bindingCount = 3; ci.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(dev, &ci, nullptr, &_compositeDescLayout) != VK_SUCCESS) return false;
    }

    VkDescriptorPoolSize sizes[] = {
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          3 + _framesInFlight },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,          3 + _framesInFlight * 2 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,  5 + _framesInFlight * 4 },
    };
    VkDescriptorPoolCreateInfo poolCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    poolCI.maxSets = 3 + _framesInFlight * 2;  // transmittance + multiScatter + skyView*N + composite*N
    poolCI.poolSizeCount = 3; poolCI.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(dev, &poolCI, nullptr, &_computeDescPool) != VK_SUCCESS) return false;

    {
        VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = _computeDescPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &_transmittanceDescLayout;
        if (vkAllocateDescriptorSets(dev, &ai, &_transmittanceDescSet) != VK_SUCCESS) return false;
    }
    {
        VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = _computeDescPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &_multiScatterDescLayout;
        if (vkAllocateDescriptorSets(dev, &ai, &_multiScatterDescSet) != VK_SUCCESS) return false;
    }
    for (uint32_t f = 0; f < _framesInFlight; ++f) {
        VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = _computeDescPool; ai.descriptorSetCount = 1;
        ai.pSetLayouts = &_skyViewDescLayout;
        if (vkAllocateDescriptorSets(dev, &ai, &_skyViewDescSet[f]) != VK_SUCCESS) return false;
        ai.pSetLayouts = &_compositeDescLayout;
        if (vkAllocateDescriptorSets(dev, &ai, &_compositeDescSet[f]) != VK_SUCCESS) return false;
    }

    {
        VkDescriptorImageInfo  imgInfo{ VK_NULL_HANDLE, _transmittanceView, VK_IMAGE_LAYOUT_GENERAL };
        VkDescriptorBufferInfo bufInfo{ _ubo[0], 0, sizeof(AtmosphereUBO) };
        VkWriteDescriptorSet ws[2]{};
        ws[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _transmittanceDescSet, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  &imgInfo };
        ws[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _transmittanceDescSet, 1, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &bufInfo };
        vkUpdateDescriptorSets(dev, 2, ws, 0, nullptr);
    }
    {
        VkDescriptorImageInfo  outInfo  { VK_NULL_HANDLE, _multiScatterView,   VK_IMAGE_LAYOUT_GENERAL };
        VkDescriptorBufferInfo bufInfo  { _ubo[0], 0, sizeof(AtmosphereUBO) };
        VkDescriptorImageInfo  transInfo{ _lutSampler, _transmittanceView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet ws[3]{};
        ws[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _multiScatterDescSet, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          &outInfo };
        ws[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _multiScatterDescSet, 1, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         nullptr, &bufInfo };
        ws[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _multiScatterDescSet, 2, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &transInfo };
        vkUpdateDescriptorSets(dev, 3, ws, 0, nullptr);
    }

    for (uint32_t f = 0; f < _framesInFlight; ++f) {
        VkDescriptorImageInfo  skyOutInfo{ VK_NULL_HANDLE, _skyViewView, VK_IMAGE_LAYOUT_GENERAL };
        VkDescriptorBufferInfo bufInfo   { _ubo[f], 0, sizeof(AtmosphereUBO) };
        VkDescriptorImageInfo  transInfo { _lutSampler, _transmittanceView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo  msInfo    { _lutSampler, _multiScatterView,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };

        VkWriteDescriptorSet ws[4]{};
        ws[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _skyViewDescSet[f], 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          &skyOutInfo };
        ws[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _skyViewDescSet[f], 1, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         nullptr, &bufInfo };
        ws[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _skyViewDescSet[f], 2, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &transInfo };
        ws[3] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _skyViewDescSet[f], 3, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &msInfo };
        vkUpdateDescriptorSets(dev, 4, ws, 0, nullptr);

        VkDescriptorImageInfo skyViewInfo{ _lutSampler, _skyViewView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo depthInfo  { _lutSampler, _depthView,   VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };

        VkWriteDescriptorSet cws[3]{};
        cws[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _compositeDescSet[f], 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         nullptr, &bufInfo };
        cws[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _compositeDescSet[f], 1, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &skyViewInfo };
        cws[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _compositeDescSet[f], 2, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depthInfo };
        vkUpdateDescriptorSets(dev, 3, cws, 0, nullptr);
    }

    return true;
}

static VkPipeline CreateComputePipeline(VkDevice dev, const std::vector<uint32_t>& spv,
                                         VkPipelineLayout layout)
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

bool VulkanAtmosphere::CreateComputePipelines()
{
    VkDevice dev = _core->GetDevice();

    auto makePipeLayout = [&](VkDescriptorSetLayout dsl) -> VkPipelineLayout {
        VkPipelineLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        ci.setLayoutCount = 1; ci.pSetLayouts = &dsl;
        VkPipelineLayout pl = VK_NULL_HANDLE;
        vkCreatePipelineLayout(dev, &ci, nullptr, &pl);
        return pl;
    };

    _transmittancePipeLayout = makePipeLayout(_transmittanceDescLayout);
    _multiScatterPipeLayout  = makePipeLayout(_multiScatterDescLayout);
    _skyViewPipeLayout       = makePipeLayout(_skyViewDescLayout);

    std::vector<uint32_t> transSpv, msSpv, skySpv;
    if (!CompileGLSLtoSPIRV(GetShaderFullPath(L"atmosphere_transmittance_vk.comp.glsl").wstring(), transSpv)) return false;
    if (!CompileGLSLtoSPIRV(GetShaderFullPath(L"atmosphere_multiscatter_vk.comp.glsl").wstring(),  msSpv))    return false;
    if (!CompileGLSLtoSPIRV(GetShaderFullPath(L"atmosphere_skyview_vk.comp.glsl").wstring(),       skySpv))   return false;

    _transmittancePipeline = CreateComputePipeline(dev, transSpv, _transmittancePipeLayout);
    _multiScatterPipeline  = CreateComputePipeline(dev, msSpv,    _multiScatterPipeLayout);
    _skyViewPipeline       = CreateComputePipeline(dev, skySpv,   _skyViewPipeLayout);

    return _transmittancePipeline && _multiScatterPipeline && _skyViewPipeline;
}

// LOAD_OP_LOAD preserves deferred-lighting HDR content; sky pixels overwrite via discard.
// initialLayout=SHADER_READ_ONLY_OPTIMAL matches where ppRenderPass leaves the HDR image.
bool VulkanAtmosphere::CreateAtmosphereRenderPass()
{
    VkAttachmentDescription att{};
    att.format         = VK_FORMAT_R16G16B16A16_SFLOAT;
    att.samples        = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout  = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    att.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference cr{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };

    VkSubpassDescription sp{};
    sp.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1;
    sp.pColorAttachments    = &cr;

    // Wait for deferred lighting COLOR_ATTACHMENT_OUTPUT write before LOAD_OP_LOAD reads it.
    VkSubpassDependency dep{};
    dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass    = 0;
    dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpi{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpi.attachmentCount = 1; rpi.pAttachments  = &att;
    rpi.subpassCount    = 1; rpi.pSubpasses    = &sp;
    rpi.dependencyCount = 1; rpi.pDependencies = &dep;

    return vkCreateRenderPass(_core->GetDevice(), &rpi, nullptr, &_atmosphereRenderPass) == VK_SUCCESS;
}

bool VulkanAtmosphere::CreateCompositePipeline()
{
    VkDevice dev = _core->GetDevice();

    {
        VkPipelineLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        ci.setLayoutCount = 1; ci.pSetLayouts = &_compositeDescLayout;
        if (vkCreatePipelineLayout(dev, &ci, nullptr, &_compositePipeLayout) != VK_SUCCESS) return false;
    }

    std::vector<uint32_t> vsSpv, fsSpv;
    if (!CompileGLSLtoSPIRV(GetShaderFullPath(L"fullscreen_vk.vert.glsl").wstring(), vsSpv)) return false;
    if (!CompileGLSLtoSPIRV(GetShaderFullPath(L"atmosphere_composite_vk.frag.glsl").wstring(), fsSpv)) return false;

    auto mkMod = [&](const std::vector<uint32_t>& spv) -> VkShaderModule {
        VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        ci.codeSize = spv.size() * 4; ci.pCode = spv.data();
        VkShaderModule m = VK_NULL_HANDLE;
        vkCreateShaderModule(dev, &ci, nullptr, &m); return m;
    };
    VkShaderModule vsM = mkMod(vsSpv), fsM = mkMod(fsSpv);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,   vsM, "main" };
    stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fsM, "main" };

    VkPipelineVertexInputStateCreateInfo    vis{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo  ias{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ias.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo       vps{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vps.viewportCount = 1; vps.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo  rs { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo    ms { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo   dss{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    dss.depthTestEnable = VK_FALSE;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cbs{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cbs.attachmentCount = 1; cbs.pAttachments = &cba;
    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynState{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynState.dynamicStateCount = 2; dynState.pDynamicStates = dynStates;

    {
        VkFramebufferCreateInfo fbci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fbci.renderPass      = _atmosphereRenderPass;
        fbci.attachmentCount = 1; fbci.pAttachments = &_hdrView;
        fbci.width  = _extent.width; fbci.height = _extent.height; fbci.layers = 1;
        if (vkCreateFramebuffer(dev, &fbci, nullptr, &_compositeFramebuffer) != VK_SUCCESS) {
            vkDestroyShaderModule(dev, vsM, nullptr); vkDestroyShaderModule(dev, fsM, nullptr);
            return false;
        }
    }

    VkGraphicsPipelineCreateInfo gpci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gpci.stageCount = 2; gpci.pStages = stages;
    gpci.pVertexInputState   = &vis; gpci.pInputAssemblyState = &ias;
    gpci.pViewportState      = &vps; gpci.pRasterizationState = &rs;
    gpci.pMultisampleState   = &ms;  gpci.pDepthStencilState  = &dss;
    gpci.pColorBlendState    = &cbs; gpci.pDynamicState       = &dynState;
    gpci.layout     = _compositePipeLayout;
    gpci.renderPass = _atmosphereRenderPass; gpci.subpass = 0;

    VkResult vr = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, nullptr, &_compositePipeline);
    vkDestroyShaderModule(dev, vsM, nullptr);
    vkDestroyShaderModule(dev, fsM, nullptr);
    return vr == VK_SUCCESS;
}

void VulkanAtmosphere::Precompute(VkCommandBuffer cmd)
{
    AtmosphereUBO ubo{};
    ubo.sunDirection[0] = _sunDir.x; ubo.sunDirection[1] = _sunDir.y; ubo.sunDirection[2] = _sunDir.z;
    ubo.sunIntensity     = _sunIntensity;
    ubo.rayleighScat[0]  = 5.802e-6f; ubo.rayleighScat[1] = 13.558e-6f; ubo.rayleighScat[2] = 33.1e-6f;
    ubo.rayleighDensityH = 8000.0f;
    ubo.mieScattering    = 3.996e-6f;
    ubo.mieAbsorption    = 4.4e-6f;
    ubo.mieDensityH      = 1200.0f;
    ubo.miePhaseG        = 0.8f;
    ubo.ozoneAbsorption[0] = 0.650e-6f; ubo.ozoneAbsorption[1] = 1.881e-6f; ubo.ozoneAbsorption[2] = 0.085e-6f;
    ubo.ozoneCenterH     = 25000.0f;
    ubo.ozoneWidth       = 15000.0f;
    ubo.groundRadius     = 6360000.0f;
    ubo.atmosphereRadius = 6460000.0f;
    ubo.groundAlbedo[0]  = 0.3f; ubo.groundAlbedo[1] = 0.3f; ubo.groundAlbedo[2] = 0.3f;
    ubo.sunAngularRadius = 0.00935f;
    ubo.cameraHeight     = 1.0f;
    memcpy(_uboMapped[0], &ubo, sizeof(ubo));

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _transmittancePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _transmittancePipeLayout, 0, 1, &_transmittanceDescSet, 0, nullptr);
    vkCmdDispatch(cmd, (TRANSMITTANCE_W + 15) / 16, (TRANSMITTANCE_H + 15) / 16, 1);

    VkImageMemoryBarrier bar{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    bar.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
    bar.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
    bar.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
    bar.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.image               = _transmittanceImage;
    bar.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &bar);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _multiScatterPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _multiScatterPipeLayout, 0, 1, &_multiScatterDescSet, 0, nullptr);
    vkCmdDispatch(cmd, (MULTISCATTER_W + 7) / 8, (MULTISCATTER_H + 7) / 8, 1);

    bar.image = _multiScatterImage;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &bar);
}

void VulkanAtmosphere::Update(VkCommandBuffer cmd, uint32_t frameIndex,
                               const XMFLOAT4X4& view, const XMFLOAT4X4& proj)
{
    if (!_ready) return;

    AtmosphereUBO ubo{};
    ubo.sunDirection[0] = _sunDir.x; ubo.sunDirection[1] = _sunDir.y; ubo.sunDirection[2] = _sunDir.z;
    ubo.sunIntensity     = _sunIntensity;
    ubo.rayleighScat[0]  = 5.802e-6f; ubo.rayleighScat[1] = 13.558e-6f; ubo.rayleighScat[2] = 33.1e-6f;
    ubo.rayleighDensityH = 8000.0f;
    ubo.mieScattering    = 3.996e-6f;
    ubo.mieAbsorption    = 4.4e-6f;
    ubo.mieDensityH      = 1200.0f;
    ubo.miePhaseG        = 0.8f;
    ubo.ozoneAbsorption[0] = 0.650e-6f; ubo.ozoneAbsorption[1] = 1.881e-6f; ubo.ozoneAbsorption[2] = 0.085e-6f;
    ubo.ozoneCenterH     = 25000.0f;
    ubo.ozoneWidth       = 15000.0f;
    ubo.groundRadius     = 6360000.0f;
    ubo.atmosphereRadius = 6460000.0f;
    ubo.groundAlbedo[0]  = 0.3f; ubo.groundAlbedo[1] = 0.3f; ubo.groundAlbedo[2] = 0.3f;
    ubo.sunAngularRadius = 0.00935f;
    ubo.cameraHeight     = 1.0f;
    ubo.screenRes[0] = (float)_extent.width; ubo.screenRes[1] = (float)_extent.height;
    ubo.nearPlane = 0.1f; ubo.farPlane = 100.0f;

    XMMATRIX V = XMLoadFloat4x4(&view);
    XMMATRIX P = XMLoadFloat4x4(&proj);
    XMMATRIX iVP = XMMatrixInverse(nullptr, XMMatrixMultiply(V, P));
    XMFLOAT4X4 iVPF; XMStoreFloat4x4(&iVPF, iVP);
    memcpy(ubo.invViewProj, &iVPF, 64);
    memcpy(_uboMapped[frameIndex], &ubo, sizeof(ubo));

    // On the first Update() skyView is still GENERAL from CreateLUTImages — skip the transition.
    // Subsequent frames must transition back from SHADER_READ_ONLY (left by the previous Update).
    VkImageMemoryBarrier bar{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    bar.srcAccessMask       = VK_ACCESS_SHADER_READ_BIT;
    bar.dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
    bar.oldLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bar.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
    bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.image               = _skyViewImage;
    bar.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    if (_skyViewReady) {
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &bar);
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _skyViewPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _skyViewPipeLayout, 0, 1, &_skyViewDescSet[frameIndex], 0, nullptr);
    vkCmdDispatch(cmd, (SKYVIEW_W + 15) / 16, (SKYVIEW_H + 3) / 4, 1);

    bar.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    bar.oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
    bar.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &bar);

    _skyViewReady = true;
}

void VulkanAtmosphere::DrawComposite(VkCommandBuffer cmd, uint32_t frameIndex)
{
    if (!_ready || !_compositeFramebuffer) return;

    VkRenderPassBeginInfo rpi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rpi.renderPass        = _atmosphereRenderPass;
    rpi.framebuffer       = _compositeFramebuffer;
    rpi.renderArea.extent = _extent;
    rpi.clearValueCount   = 0;
    vkCmdBeginRenderPass(cmd, &rpi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{ 0, 0, (float)_extent.width, (float)_extent.height, 0, 1 };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc{ {0,0}, _extent };
    vkCmdSetScissor(cmd, 0, 1, &sc);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _compositePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _compositePipeLayout, 0, 1, &_compositeDescSet[frameIndex], 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
}

} // namespace Luna
