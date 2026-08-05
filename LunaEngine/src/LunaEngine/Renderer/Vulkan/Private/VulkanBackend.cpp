#include "LunaPCH.h"
#include "LunaEngine/Renderer/Vulkan/Public/VulkanBackend.h"
// S2b: Camera sensor
#include "LunaEngine/Sensor/CameraSensor.h"
#include "LunaEngine/Sensor/SensorComponent.h"
#include "LunaEngine/Manager/SceneManager.h"
#include "Scene/Scene.h"
#include "Components/GameObject.h"
#include "LunaEngine/Renderer/Vulkan/Public/VulkanRenderGraph.h"
#include "Logger/Logger.h"
#include "Renderer/Vulkan/Public/VulkanDevice.h"
#include "Renderer/Mesh.h"
#include "Renderer/Meshlet.h"        // Phase 27: meshlet generation
#include "LunaEngine/Utils/FileSystemUtil.h"
#include "stb_image.h"
#include "cgltf.h"
#include <algorithm>
#include <random>
#include <set>
#include <functional>

// DXC COM GUIDs - needed for DxcCreateInstance
#include <dxcapi.h>
#include <initguid.h>
DEFINE_GUID(CLSID_DxcUtils_Local, 0x6245d6af, 0x66e0, 0x48fd, 0x80, 0xb4, 0x4d, 0x27, 0x17, 0x96, 0x74, 0x8c);
DEFINE_GUID(CLSID_DxcCompiler_Local, 0x73e22d93, 0xe6ce, 0x47f3, 0xb5, 0xbf, 0xf0, 0x66, 0x4f, 0x39, 0xc1, 0xb0);

namespace Luna
{

// ---------------------------------------------------------------------------
// DXC helper ??compile HLSL ??SPIR-V
// ---------------------------------------------------------------------------
static bool CompileHLSLtoSPIRV(const std::wstring& path, const std::wstring& target,
                                 std::vector<uint32_t>& outSpirv)
{
    ComPtr<IDxcUtils>     utils;
    ComPtr<IDxcCompiler3> compiler;
    HRESULT hr1 = DxcCreateInstance(CLSID_DxcUtils_Local,     IID_PPV_ARGS(&utils));
    HRESULT hr2 = DxcCreateInstance(CLSID_DxcCompiler_Local, IID_PPV_ARGS(&compiler));
    if (FAILED(hr1) || !utils) { LUNA_LOG_ERROR("VK: DxcUtils creation failed (0x%08X)", hr1); return false; }
    if (FAILED(hr2) || !compiler) { LUNA_LOG_ERROR("VK: DxcCompiler creation failed (0x%08X)", hr2); return false; }

    ComPtr<IDxcBlobEncoding> src;
    if (FAILED(utils->LoadFile(path.c_str(), nullptr, &src)))
    { LUNA_LOG_ERROR("VK Shader: cannot load %ls", path.c_str()); return false; }

    DxcBuffer buf{ src->GetBufferPointer(), src->GetBufferSize(), DXC_CP_ACP };
    
    // Build args - only use -fvk-invert-y for vertex shaders (vs_*)
    std::vector<LPCWSTR> args = {
        path.c_str(), L"-E", L"main", L"-T", target.c_str(),
        L"-spirv", L"-fvk-use-dx-layout", L"-HV", L"2021",
    };
    bool isVertexShader = (target.find(L"vs_") == 0);
    bool isLibShader    = (target.find(L"lib_") == 0);
    if (isVertexShader) {
        args.push_back(L"-fvk-invert-y");
    }
    // Ray tracing lib shaders require SPIR-V 1.4 (Vulkan 1.1) for KHR_ray_tracing
    if (isLibShader) {
        args.push_back(L"-fspv-target-env=vulkan1.1spirv1.4");
    }

    ComPtr<IDxcResult> result;
    compiler->Compile(&buf, args.data(), (UINT32)args.size(), nullptr, IID_PPV_ARGS(&result));

    HRESULT hr = S_OK;
    result->GetStatus(&hr);
    ComPtr<IDxcBlobUtf8> errors;
    result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
    if (errors && errors->GetStringLength() > 0)
        LUNA_LOG_ERROR("VK Shader %ls: %s", path.c_str(), errors->GetStringPointer());
    if (FAILED(hr)) return false;

    ComPtr<IDxcBlob> blob;
    result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&blob), nullptr);
    if (!blob) return false;

    outSpirv.resize(blob->GetBufferSize() / 4);
    memcpy(outSpirv.data(), blob->GetBufferPointer(), blob->GetBufferSize());
    return true;
}

// ---------------------------------------------------------------------------
// glslc helper ??compile GLSL ??SPIR-V via glslc subprocess
// ---------------------------------------------------------------------------
static bool CompileGLSLtoSPIRV(const std::wstring& glslPath, std::vector<uint32_t>& outSpirv)
{
    // Locate glslc.exe via VULKAN_SDK env var
    wchar_t sdkBuf[MAX_PATH] = {};
    DWORD sdkLen = GetEnvironmentVariableW(L"VULKAN_SDK", sdkBuf, MAX_PATH);
    std::wstring glslcPath = (sdkLen > 0)
        ? (std::wstring(sdkBuf) + L"\\Bin\\glslc.exe")
        : L"glslc.exe";  // fallback: rely on PATH

    // Temp .spv output path
    wchar_t tempDir[MAX_PATH] = {}, tempBase[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempDir);
    GetTempFileNameW(tempDir, L"spv", 0, tempBase);
    DeleteFileW(tempBase);  // remove zero-byte placeholder
    std::wstring spvPath = std::wstring(tempBase) + L".spv";

    // Derive shader stage from compound extension (e.g. ".frag.glsl", ".rgen.glsl")
    // glslc cannot auto-detect from multi-part extensions ??must pass -fshader-stage=
    std::wstring stage;
    if      (glslPath.find(L".vert.")  != std::wstring::npos) stage = L"vertex";
    else if (glslPath.find(L".frag.")  != std::wstring::npos) stage = L"fragment";
    else if (glslPath.find(L".comp.")  != std::wstring::npos) stage = L"compute";
    else if (glslPath.find(L".task.")  != std::wstring::npos) stage = L"task";     // Phase 27
    else if (glslPath.find(L".mesh.")  != std::wstring::npos) stage = L"mesh";     // Phase 27
    else if (glslPath.find(L".rgen.")  != std::wstring::npos) stage = L"rgen";
    else if (glslPath.find(L".rmiss.") != std::wstring::npos) stage = L"rmiss";
    else if (glslPath.find(L".rchit.") != std::wstring::npos) stage = L"rchit";
    else { LUNA_LOG_ERROR("VK GLSL: cannot determine stage for %ls", glslPath.c_str()); return false; }

    // Build command line
    std::wstring cmd = L"\"" + glslcPath + L"\" --target-env=vulkan1.1 --target-spv=spv1.4"
                     + L" -fshader-stage=" + stage
                     + L" -o \"" + spvPath + L"\" \"" + glslPath + L"\"";

    // Pipe stderr+stdout
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

    // Drain stderr/stdout
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

    // Read compiled SPIR-V
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

    LUNA_LOG_INFO("VK GLSL: compiled %ls (%u bytes)", glslPath.c_str(), fileSize);
    return true;
}

static void DestroyDebugMessengerEXT(VkInstance instance, VkDebugUtilsMessengerEXT messenger,
                                     const VkAllocationCallbacks* alloc)
{
    auto fn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
    if (fn) fn(instance, messenger, alloc);
}

// ===========================================================================
VulkanBackend::VulkanBackend() = default;
VulkanBackend::~VulkanBackend() { VulkanBackend::Shutdown(); }

// ===========================================================================
// Init
// ===========================================================================
bool VulkanBackend::Init(void* windowHandler, uint32_t width, uint32_t height)
{
    _width = width; _height = height;
    if (!CreateInstance())              return false;
    SetupDebugMessenger();
    if (!CreateSurface(windowHandler))  return false;
    _device = std::make_unique<VulkanDevice>();
    if (!_device->Initialize(_instance, _surface)) return false;
    _rtSupported = _device->IsRTSupported();  // Phase 18D

    // Initialize VulkanCore wrapper ??provides VulkanCore* interface to subsystems
    if (!_core.InitFromDevice(_device.get()))
    {
        LUNA_LOG_ERROR("VK: Failed to initialize VulkanCore from device");
        return false;
    }

    if (_rtSupported)
    {
        VkDevice dev = _device->GetDevice();
        pfn_vkCreateAccelerationStructureKHR           = (PFN_vkCreateAccelerationStructureKHR)           vkGetDeviceProcAddr(dev, "vkCreateAccelerationStructureKHR");
        pfn_vkDestroyAccelerationStructureKHR          = (PFN_vkDestroyAccelerationStructureKHR)          vkGetDeviceProcAddr(dev, "vkDestroyAccelerationStructureKHR");
        pfn_vkGetAccelerationStructureBuildSizesKHR    = (PFN_vkGetAccelerationStructureBuildSizesKHR)    vkGetDeviceProcAddr(dev, "vkGetAccelerationStructureBuildSizesKHR");
        pfn_vkCmdBuildAccelerationStructuresKHR        = (PFN_vkCmdBuildAccelerationStructuresKHR)        vkGetDeviceProcAddr(dev, "vkCmdBuildAccelerationStructuresKHR");
        pfn_vkGetAccelerationStructureDeviceAddressKHR = (PFN_vkGetAccelerationStructureDeviceAddressKHR) vkGetDeviceProcAddr(dev, "vkGetAccelerationStructureDeviceAddressKHR");
        pfn_vkCreateRayTracingPipelinesKHR             = (PFN_vkCreateRayTracingPipelinesKHR)             vkGetDeviceProcAddr(dev, "vkCreateRayTracingPipelinesKHR");
        pfn_vkGetRayTracingShaderGroupHandlesKHR       = (PFN_vkGetRayTracingShaderGroupHandlesKHR)       vkGetDeviceProcAddr(dev, "vkGetRayTracingShaderGroupHandlesKHR");
        pfn_vkCmdTraceRaysKHR                          = (PFN_vkCmdTraceRaysKHR)                          vkGetDeviceProcAddr(dev, "vkCmdTraceRaysKHR");
        if (!pfn_vkCreateAccelerationStructureKHR || !pfn_vkCreateRayTracingPipelinesKHR || !pfn_vkCmdTraceRaysKHR)
        {
            LUNA_LOG_WARN("VK RT: failed to load RT function pointers ??disabling ray tracing");
            _rtSupported = false;
        }
        else { LUNA_LOG_INFO("VK RT: function pointers loaded"); }
    }
    if (!CreateImGuiDescriptorPool())   return false;
    {
        VulkanSwapchain::CreateInfo sci;
        sci.core    = &_core;
        sci.surface = _surface;
        sci.width   = width;
        sci.height  = height;
        sci.vsync   = _vsync;
        if (!_vkSwapchain.Create(sci)) return false;
    }
    if (!CreateFrameResources())        return false;

    // Transition all swapchain images from UNDEFINED → PRESENT_SRC_KHR so the
    // ImGui overlay render pass (LOAD_OP_LOAD, initialLayout=PRESENT_SRC_KHR) is valid
    // on their first use before the tonemap pass has written to them.
    {
        VkCommandBuffer initCmd = BeginSingleTimeCommands();
        if (initCmd)
        {
            for (uint32_t i = 0; i < _vkSwapchain.GetImageCount(); ++i)
            {
                VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
                b.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
                b.newLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.image               = _vkSwapchain.GetImage(i);
                b.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                vkCmdPipelineBarrier(initCmd,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    0, 0, nullptr, 0, nullptr, 1, &b);
            }
            EndSingleTimeCommands(initCmd);
        }
    }

    {
        VulkanGBuffer::CreateInfo gbInfo;
        gbInfo.core = &_core;
        gbInfo.extent = _vkSwapchain.GetExtent();
        gbInfo.depthView = _vkSwapchain.GetDepthView();
        gbInfo.depthFormat = _vkSwapchain.GetDepthFormat();
        if (!_gBuffer.Create(gbInfo)) return false;
    }
    if (!CreateRenderPass())            return false;
    if (!CreatePipeline())              return false;
    { VulkanShadows::CreateInfo ci{}; ci.core = &_core;
      if (!_shadows.Create(ci)) { LUNA_LOG_WARN("VK: CSM init failed ??shadows disabled"); } }

    // Create point-clamp sampler (shared by SSAO and deferred pipeline)
    if (_pointClampSampler == VK_NULL_HANDLE) {
        VkSamplerCreateInfo si{};
        si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter    = si.minFilter = VK_FILTER_NEAREST;
        si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxAnisotropy = 1.0f;
        vkCreateSampler(_device->GetDevice(), &si, nullptr, &_pointClampSampler);
    }

    // Create linear sampler (shared by materials, SSR, TAA, Bloom, Tonemap)
    // Must use REPEAT + anisotropic 8x to match DX12 and prevent UV seam artifacts (bug-fix/005)
    if (_linearSampler == VK_NULL_HANDLE) {
        VkSamplerCreateInfo si{};
        si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter    = VK_FILTER_LINEAR;
        si.minFilter    = VK_FILTER_LINEAR;
        si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        si.anisotropyEnable = VK_TRUE;
        si.maxAnisotropy    = 8.0f;
        si.maxLod           = 0.0f;
        vkCreateSampler(_device->GetDevice(), &si, nullptr, &_linearSampler);
    }

    // Initialize VulkanSSAO subsystem
    {
        VulkanSSAO::CreateInfo ssaoInfo;
        ssaoInfo.core = &_core;
        ssaoInfo.extent = _vkSwapchain.GetExtent();
        ssaoInfo.depthView = _vkSwapchain.GetDepthView();
        ssaoInfo.normalView = _gBuffer.GetNormalView();
        ssaoInfo.pointClampSampler = _pointClampSampler;
        ssaoInfo.framesInFlight = FRAMES_IN_FLIGHT;
        
        if (!_ssao.Create(ssaoInfo)) {
            LUNA_LOG_WARN("VK: SSAO init failed ??AO disabled");
        }
    }

    // Phase 24: Clustered lighting resources (must be before CreateDeferredPipeline for set=2 layout)
    CreateClusteredLightingResources();  // non-fatal: _clusteredLightingReady gates usage

    if (!CreateDeferredPipeline())      return false;

    // Initialize VulkanPostProcess subsystem
    {
        VulkanPostProcess::CreateInfo ppInfo;
        ppInfo.core = &_core;
        ppInfo.extent = _vkSwapchain.GetExtent();
        ppInfo.swapchainFormat = _vkSwapchain.GetFormat();
        ppInfo.depthView = _vkSwapchain.GetDepthView();
        ppInfo.normalView = _gBuffer.GetNormalView();
        ppInfo.metalRoughView = _gBuffer.GetMetalRoughView();
        ppInfo.linearSampler = _linearSampler;
        ppInfo.pointClampSampler = _pointClampSampler;

        if (!_postProcess.Create(ppInfo)) {
            LUNA_LOG_WARN("VK: PostProcess init failed — PP disabled");
        }
        // SetTonemapFramebuffers is called AFTER CreateFramebuffers() below
    }

    // Create tonemap framebuffers (must be after CreateRenderPass which creates _tonemapRenderPass)
    if (!CreateFramebuffers()) {
        LUNA_LOG_ERROR("VK: CreateFramebuffers failed");
        return false;
    }
    // Now that _tonemapFramebuffers is populated, pass it to PostProcess
    _postProcess.SetTonemapFramebuffers(_tonemapFramebuffers);

    // S2b: sensor pipelines (IBL set created lazily in LoadHDREnvironment)
    if (!CreateSensorLightingPipeline())
        LUNA_LOG_WARN("S2b: sensor lighting pipeline failed — camera sensor rendering disabled");
    if (!CreateSensorDistortPipeline())
        LUNA_LOG_WARN("S2b: sensor distort pipeline failed — camera sensor rendering disabled");

    // GPU Profiler
    _gpuProfiler.Init(_device->GetDevice(), _device->GetPhysicalDevice(), _device->GetGraphicsQueue());

    // Initial query pool reset via one-shot command buffer (required before first use)
    {
        VkCommandBuffer initCmd = BeginSingleTimeCommands();
        if (initCmd)
        {
            for (uint32_t i = 0; i < VulkanGPUProfiler::FRAME_LATENCY; ++i)
            {
                VkQueryPool pool = _gpuProfiler.GetQueryPoolAtSlot(i);
                if (pool) vkCmdResetQueryPool(initCmd, pool, 0, VulkanGPUProfiler::QUERY_COUNT);
            }
            EndSingleTimeCommands(initCmd);
        }
    }

    return true;
}

// ===========================================================================
// Shutdown
// ===========================================================================
void VulkanBackend::Shutdown()
{
    if (!_device || _device->GetDevice() == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(_device->GetDevice());

    VkDevice dev = _device->GetDevice();

    // Reset ALL command pools to release recorded resource references, then destroy them.
    // vkResetCommandPool returns command buffers to initial state, clearing any implicit
    // references to buffers/pipelines/descriptor sets. This prevents validation errors
    // about resources being "in use" by command buffers during subsequent cleanup.
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
        if (_frames[i].cmdPool) {
            vkResetCommandPool(dev, _frames[i].cmdPool, 0);
            vkDestroyCommandPool(dev, _frames[i].cmdPool, nullptr);
            _frames[i].cmdPool = VK_NULL_HANDLE;
            _frames[i].cmdBuffer = VK_NULL_HANDLE;
        }
        if (_computeFrames[i].cmdPool) {
            vkResetCommandPool(dev, _computeFrames[i].cmdPool, 0);
            vkDestroyCommandPool(dev, _computeFrames[i].cmdPool, nullptr);
            _computeFrames[i].cmdPool = VK_NULL_HANDLE;
            _computeFrames[i].cmdBuffer = VK_NULL_HANDLE;
        }
    }
    if (_transferCmdPool) {
        vkResetCommandPool(dev, _transferCmdPool, 0);
        vkDestroyCommandPool(dev, _transferCmdPool, nullptr);
        _transferCmdPool = VK_NULL_HANDLE;
    }
    _core.Shutdown();  // Destroys VulkanCore's _transferCmdPool; device pointer stays valid

    // Now safe to destroy subsystems and resources
    // S2b: camera sensor resources (must be before IBL/pipelines)
    {
        std::vector<CameraSensor*> camKeys;
        camKeys.reserve(_vkCameraRTs.size());
        for (auto& kv : _vkCameraRTs) camKeys.push_back(kv.first);
        for (auto* k : camKeys) DestroyVKCameraResources(k);

        if (_sensorIBLSet)           { vkFreeDescriptorSets(dev, _sensorIBLPool, 1, &_sensorIBLSet); _sensorIBLSet = VK_NULL_HANDLE; }
        if (_sensorIBLPool)          { vkDestroyDescriptorPool(dev, _sensorIBLPool, nullptr); _sensorIBLPool = VK_NULL_HANDLE; }
        if (_sensorLitPipeline)      { vkDestroyPipeline(dev, _sensorLitPipeline, nullptr); _sensorLitPipeline = VK_NULL_HANDLE; }
        if (_sensorDistPipeline)     { vkDestroyPipeline(dev, _sensorDistPipeline, nullptr); _sensorDistPipeline = VK_NULL_HANDLE; }
        if (_sensorLitPipeLayout)    { vkDestroyPipelineLayout(dev, _sensorLitPipeLayout, nullptr); _sensorLitPipeLayout = VK_NULL_HANDLE; }
        if (_sensorDistPipeLayout)   { vkDestroyPipelineLayout(dev, _sensorDistPipeLayout, nullptr); _sensorDistPipeLayout = VK_NULL_HANDLE; }
        if (_sensorSceneLayout)      { vkDestroyDescriptorSetLayout(dev, _sensorSceneLayout, nullptr); _sensorSceneLayout = VK_NULL_HANDLE; }
        if (_sensorGBufLayout)       { vkDestroyDescriptorSetLayout(dev, _sensorGBufLayout, nullptr); _sensorGBufLayout = VK_NULL_HANDLE; }
        if (_sensorIBLLayout)        { vkDestroyDescriptorSetLayout(dev, _sensorIBLLayout, nullptr); _sensorIBLLayout = VK_NULL_HANDLE; }
        if (_sensorDistUBOLayout)    { vkDestroyDescriptorSetLayout(dev, _sensorDistUBOLayout, nullptr); _sensorDistUBOLayout = VK_NULL_HANDLE; }
        if (_sensorDistInputLayout)  { vkDestroyDescriptorSetLayout(dev, _sensorDistInputLayout, nullptr); _sensorDistInputLayout = VK_NULL_HANDLE; }
        if (_sensorDistOutLayout)    { vkDestroyDescriptorSetLayout(dev, _sensorDistOutLayout, nullptr); _sensorDistOutLayout = VK_NULL_HANDLE; }
    }
    DestroyVKVisibilityResources(); // Phase 32: must be before device destruction
    DestroyVKOITResources();        // Phase 31: must be before device destruction
    _gi.Destroy();             // Phase 30: must be before device destruction
    _volumetricFog.Destroy();  // Phase 29: must be before device destruction
    _atmosphere.Destroy();     // Phase 28: must be before device destruction
    _shadows.Destroy();
    _hiZ.Destroy();
    _ibl.Destroy();
    _ssao.Destroy();
    _postProcess.Destroy();
    DestroyClusteredLightingResources();  // Phase 24

    _gpuProfiler.Shutdown();
    ShutdownImGui();

    // Destroy materials (deduplicated to avoid double-free on shared materials)
    {
        std::set<VkMaterial*> destroyedMats;
        for (auto& m : _vkSceneMeshes) {
            if (!m || !m->material) continue;
            if (destroyedMats.count(m->material.get())) continue;
            destroyedMats.insert(m->material.get());
            auto& mat = *m->material;
            auto dt = [&](VkTexture& t) {
                if (t.view)   { vkDestroyImageView(dev, t.view, nullptr); t.view   = VK_NULL_HANDLE; }
                if (t.image)  { vkDestroyImage(dev, t.image, nullptr);    t.image  = VK_NULL_HANDLE; }
                if (t.memory) { vkFreeMemory(dev, t.memory, nullptr);     t.memory = VK_NULL_HANDLE; }
            };
            dt(mat.albedo); dt(mat.normalMap); dt(mat.metalRough); dt(mat.emissive);
            if (mat.ubo)    { vkDestroyBuffer(dev, mat.ubo, nullptr);  mat.ubo    = VK_NULL_HANDLE; }
            if (mat.uboMem) { vkFreeMemory(dev, mat.uboMem, nullptr);  mat.uboMem = VK_NULL_HANDLE; }
        }
    }
    for (auto& m : _vkSceneMeshes) {
        if (!m) continue;
        if (m->vertexBuffer) vkDestroyBuffer(dev, m->vertexBuffer, nullptr);
        if (m->vertexMemory) vkFreeMemory(dev, m->vertexMemory, nullptr);
        if (m->indexBuffer)  vkDestroyBuffer(dev, m->indexBuffer, nullptr);
        if (m->indexMemory)  vkFreeMemory(dev, m->indexMemory, nullptr);
    }
    _vkSceneMeshes.clear();
    DestroyRTPipeline();            // Phase 18D: RT pipeline + shadow mask + SBT
    DestroyAccelerationStructures();// Phase 18D: BLAS + TLAS
    DestroyPPResources();  // Phase 16C: SSR + HDR RT resources
    DestroyPipeline();   // handles G-buffer, deferred, forward pipelines + render passes
    if (_linearSampler) { vkDestroySampler(dev, _linearSampler, nullptr); _linearSampler = VK_NULL_HANDLE; }
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
        if (_frames[i].mvpBuffer)   vkDestroyBuffer(dev, _frames[i].mvpBuffer, nullptr);
        if (_frames[i].mvpMemory)   vkFreeMemory(dev, _frames[i].mvpMemory, nullptr);
        if (_frames[i].sceneBuffer) vkDestroyBuffer(dev, _frames[i].sceneBuffer, nullptr);
        if (_frames[i].sceneMemory) vkFreeMemory(dev, _frames[i].sceneMemory, nullptr);
        // cmdPool already destroyed above
        if (_frames[i].fence)       vkDestroyFence(dev, _frames[i].fence, nullptr);
        if (_frames[i].imageReady)  vkDestroySemaphore(dev, _frames[i].imageReady, nullptr);
        if (_frames[i].renderDone)  vkDestroySemaphore(dev, _frames[i].renderDone, nullptr);
    }
    if (_imguiDescriptorPool) { vkDestroyDescriptorPool(dev, _imguiDescriptorPool, nullptr); _imguiDescriptorPool = VK_NULL_HANDLE; }
    _vkSwapchain.Destroy();
    if (_hdrFramebuffer)     { vkDestroyFramebuffer(dev, _hdrFramebuffer,    nullptr); _hdrFramebuffer     = VK_NULL_HANDLE; }
    if (_hdrRenderPass)      { vkDestroyRenderPass(dev, _hdrRenderPass,      nullptr); _hdrRenderPass      = VK_NULL_HANDLE; }
    if (_ppRenderPass)       { vkDestroyRenderPass(dev, _ppRenderPass,       nullptr); _ppRenderPass       = VK_NULL_HANDLE; }
    if (_tonemapRenderPass)  { vkDestroyRenderPass(dev, _tonemapRenderPass,  nullptr); _tonemapRenderPass  = VK_NULL_HANDLE; }
    for (auto fb : _tonemapFramebuffers)
        if (fb) vkDestroyFramebuffer(dev, fb, nullptr);
    _tonemapFramebuffers.clear();
    _device->ShutDown(); _device.reset();
    if (_surface)       { vkDestroySurfaceKHR(_instance, _surface, nullptr);   _surface       = VK_NULL_HANDLE; }
    if (_debugMessenger){ DestroyDebugMessengerEXT(_instance, _debugMessenger, nullptr); _debugMessenger = VK_NULL_HANDLE; }
    if (_instance)      { vkDestroyInstance(_instance, nullptr);               _instance      = VK_NULL_HANDLE; }
}

// ===========================================================================
// Frame loop
// ===========================================================================
void VulkanBackend::BeginFrame()
{
    _frameActive   = false;
    _drawCallIndex = 0;
    _computeSubmittedThisFrame = false;  // Phase 20

    // Bail out if device was lost - cannot recover
    if (_deviceLost) return;

    VkDevice dev = _device->GetDevice();

    // Handle deferred resize (safe ??no command buffer is recording yet)
    if (_pendingResize)
    {
        _pendingResize = false;
        _width  = _pendingResizeW;
        _height = _pendingResizeH;

        // Wait for ALL in-flight frames to finish before destroying resources
        for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++)
        {
            if (_frames[i].fence != VK_NULL_HANDLE)
            {
                VkResult wr = vkWaitForFences(dev, 1, &_frames[i].fence, VK_TRUE, UINT64_MAX);
                if (wr == VK_ERROR_DEVICE_LOST)
                {
                    LUNA_LOG_ERROR("VK: Device lost during resize fence wait");
                    _deviceLost = true;
                    return;
                }
            }
        }
        vkDeviceWaitIdle(dev);

        RecreateSwapchain();
        ImGui_ImplVulkan_SetMinImageCount(2);

        // Clear imagesInFlight to avoid stale fence references after swapchain recreation
        for (uint32_t ii = 0; ii < _vkSwapchain.GetImageCount(); ++ii) _vkSwapchain.SetImageFence(ii, VK_NULL_HANDLE);

        // Reset resize cooldown - RT will be skipped for a few frames
        _framesSinceResize = 0;

        // Return to let next BeginFrame start with clean state
        return;
    }

    auto& frame = _frames[_frameIndex];

    // Wait for this frame's fence (from FRAMES_IN_FLIGHT ago)
    VkResult waitRes = vkWaitForFences(dev, 1, &frame.fence, VK_TRUE, UINT64_MAX);
    if (waitRes == VK_ERROR_DEVICE_LOST)
    {
        LUNA_LOG_ERROR("VK: Device lost - cannot continue rendering");
        _deviceLost = true;
        return;
    }
    if (waitRes != VK_SUCCESS)
    {
        LUNA_LOG_ERROR("VK: vkWaitForFences failed: %d", (int)waitRes);
        return;
    }

    VkResult r = _vkSwapchain.AcquireNextImage(frame.imageReady, &_imageIndex);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
    {
        // Wait for ALL in-flight frames before recreation
        for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++)
        {
            if (_frames[i].fence != VK_NULL_HANDLE)
            {
                VkResult wr = vkWaitForFences(dev, 1, &_frames[i].fence, VK_TRUE, UINT64_MAX);
                if (wr == VK_ERROR_DEVICE_LOST)
                {
                    LUNA_LOG_ERROR("VK: Device lost during acquire retry fence wait");
                    _deviceLost = true;
                    return;
                }
            }
        }
        vkDeviceWaitIdle(dev);
        RecreateSwapchain();
        // Clear imagesInFlight to avoid stale fence references
        for (uint32_t ii = 0; ii < _vkSwapchain.GetImageCount(); ++ii) _vkSwapchain.SetImageFence(ii, VK_NULL_HANDLE);
        _framesSinceResize = 0;  // Reset resize cooldown
        return;
    }
    if (r == VK_ERROR_DEVICE_LOST)
    {
        LUNA_LOG_ERROR("VK: Device lost during image acquire");
        _deviceLost = true;
        return;
    }
    if (r != VK_SUCCESS) { LUNA_LOG_ERROR("vkAcquireNextImageKHR failed: %d", (int)r); return; }

    // Wait for any previous frame that was using this swapchain image
    VkFence imgFence = _vkSwapchain.GetImageFence(_imageIndex);
    if (imgFence != VK_NULL_HANDLE)
    {
        VkResult wr = vkWaitForFences(dev, 1, &imgFence, VK_TRUE, UINT64_MAX);
        if (wr == VK_ERROR_DEVICE_LOST)
        {
            LUNA_LOG_ERROR("VK: Device lost during image-in-flight fence wait");
            _deviceLost = true;
            return;
        }
    }
    // Mark this image as being used by this frame's fence
    _vkSwapchain.SetImageFence(_imageIndex, frame.fence);

    vkResetFences(dev, 1, &frame.fence);

    // Update ping-pong index and per-frame TAA descriptor (after fence ??safe to update GPU resources)
    _postProcess.UpdateDescriptors(_frameIndex);

    vkResetCommandBuffer(frame.cmdBuffer, 0);

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(frame.cmdBuffer, &bi);

    // GPU Profiler: begin frame + command-buffer query pool reset
    _gpuProfiler.BeginFrame();
    _gpuProfiler.ResetQueryPool(frame.cmdBuffer);

    // CSM shadow depth pre-pass (uses previous frame's cached model matrices)
    if (_shadows.GetArrayView() && !_lastMeshModels.empty())
    {
        _gpuProfiler.WriteBeginTimestamp(frame.cmdBuffer, "CSM Shadows");
        _shadows.UpdateMatrices(_lastView, _lastProj);

        // Build ShadowDraw list from cached mesh data
        std::vector<VulkanShadows::ShadowDraw> draws;
        draws.reserve(_vkSceneMeshes.size());
        for (size_t mi = 0; mi < _vkSceneMeshes.size() && mi < _lastMeshModels.size(); ++mi) {
            auto& m = _vkSceneMeshes[mi];
            if (!m) continue;
            draws.push_back({ m->vertexBuffer, m->indexBuffer, m->indexCount, _lastMeshModels[mi] });
        }
        _shadows.DrawPass(frame.cmdBuffer, draws);
        _gpuProfiler.WriteEndTimestamp(frame.cmdBuffer);
    }

    _gpuProfiler.WriteBeginTimestamp(frame.cmdBuffer, "GBuffer Fill");

    // G-buffer pass: 3 colour clears + depth clear
    VkClearValue gbClears[4]{};
    gbClears[3].depthStencil = { 1.0f, 0 };   // depth clear; colour targets default to 0

    VkRenderPassBeginInfo rpi{};
    rpi.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpi.renderPass        = _gBuffer.GetRenderPass();
    rpi.framebuffer       = _gBuffer.GetFramebuffer();
    rpi.renderArea.extent = _vkSwapchain.GetExtent();
    rpi.clearValueCount   = 4;
    rpi.pClearValues      = gbClears;
    vkCmdBeginRenderPass(frame.cmdBuffer, &rpi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{ 0, 0,
        (float)_vkSwapchain.GetExtent().width, (float)_vkSwapchain.GetExtent().height, 0, 1 };
    vkCmdSetViewport(frame.cmdBuffer, 0, 1, &vp);

    VkRect2D sc{ {0,0}, _vkSwapchain.GetExtent() };
    vkCmdSetScissor(frame.cmdBuffer, 0, 1, &sc);

    _frameActive = true;

    // Track frames since last resize for RT stabilization
    if (_framesSinceResize < 100) _framesSinceResize++;
}

void VulkanBackend::DrawFrame() {} // SceneManager calls DrawMesh directly

// CompositeFrame: end G-buffer pass ??render graph ??open ImGui pass
void VulkanBackend::CompositeFrame()
{
    if (!_frameActive || _deviceLost) return;
    auto& frame = _frames[_frameIndex];
    VkCommandBuffer cmd = frame.cmdBuffer;
    VkDevice dev = _device->GetDevice();

    // End G-buffer render pass
    vkCmdEndRenderPass(cmd);


    // ─── Phase 23: Build Hi-Z pyramid from current frame's depth ───
    // After the G-buffer pass ends, depth is in DEPTH_STENCIL_READ_ONLY_OPTIMAL.
    // Transition to DEPTH_STENCIL_ATTACHMENT_OPTIMAL so BuildPyramid can use it,
    // then build the Hi-Z pyramid so it's ready for next frame's occlusion cull.
    // Finally, restore depth to DEPTH_STENCIL_READ_ONLY_OPTIMAL for the render graph.
    if (_hiZ.GetMipCount() >= 2)
    {
        // READ_ONLY ??ATTACHMENT (BuildPyramid expects ATTACHMENT_OPTIMAL)
        VkImageMemoryBarrier depthBar{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        depthBar.srcAccessMask    = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        depthBar.dstAccessMask    = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                                  | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        depthBar.oldLayout        = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        depthBar.newLayout        = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthBar.image            = _vkSwapchain.GetDepthImage();
        depthBar.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
        depthBar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        depthBar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            0, 0, nullptr, 0, nullptr, 1, &depthBar);

        _gpuProfiler.WriteBeginTimestamp(cmd, "Hi-Z Build");
        _hiZ.BuildPyramid(cmd, _vkSwapchain.GetDepthImage(), _vkSwapchain.GetExtent());
        _gpuProfiler.WriteEndTimestamp(cmd);

        // ATTACHMENT ??READ_ONLY (restore for render graph + downstream SSAO/lighting/SSR)
        VkImageMemoryBarrier restoreBar{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        restoreBar.srcAccessMask    = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                                    | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        restoreBar.dstAccessMask    = VK_ACCESS_SHADER_READ_BIT;
        restoreBar.oldLayout        = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        restoreBar.newLayout        = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        restoreBar.image            = _vkSwapchain.GetDepthImage();
        restoreBar.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
        restoreBar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        restoreBar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &restoreBar);
    }

    // ?�━ Update deferred scene UBO ?�━?�━?�━?�━?�━?�━?�━?�━?�━?�━?�━?�━?�━?�━?�━?�━?�━?�━?�━?�━?�━?�━
    // Compute invViewProj = inverse(view * proj) using DirectXMath.
    {
        XMMATRIX V   = XMLoadFloat4x4(&_deferredView);
        XMMATRIX P   = XMLoadFloat4x4(&_deferredProj);
        XMMATRIX VP  = XMMatrixMultiply(V, P);
        XMVECTOR det = XMMatrixDeterminant(VP);
        XMMATRIX iVP = XMMatrixInverse(&det, VP);

        XMFLOAT4X4 iVPF;
        XMStoreFloat4x4(&iVPF, iVP);

        // Recover eye position from view matrix (row-major convention)
        float negTx = -_deferredView._41, negTy = -_deferredView._42, negTz = -_deferredView._43;
        float eyeX  = _deferredView._11*negTx + _deferredView._12*negTy + _deferredView._13*negTz;
        float eyeY  = _deferredView._21*negTx + _deferredView._22*negTy + _deferredView._23*negTz;
        float eyeZ  = _deferredView._31*negTx + _deferredView._32*negTy + _deferredView._33*negTz;

        static const float kSqrt6Inv = 1.0f / 2.449490f;
        DeferredSceneUBO ubo{};
        memcpy(ubo.invViewProj, &iVPF, 64);
        ubo.eyePosition[0] = eyeX; ubo.eyePosition[1] = eyeY; ubo.eyePosition[2] = eyeZ;
        ubo.lightDir[0] = kSqrt6Inv; ubo.lightDir[1] = 2.0f * kSqrt6Inv; ubo.lightDir[2] = kSqrt6Inv;
        ubo.lightIntensity = 3.0f;
        ubo.lightColor[0] = ubo.lightColor[1] = ubo.lightColor[2] = 1.0f;
        // S2b: cache for sensor lighting passes
        _cachedLightDir   = XMFLOAT3(kSqrt6Inv, 2.0f * kSqrt6Inv, kSqrt6Inv);
        _cachedLightColor = XMFLOAT4(1.0f, 1.0f, 1.0f, 3.0f);

        // CSM data: viewMatrix, lightVP[4], cascadeSplits
        memcpy(ubo.viewMatrix, &_deferredView, 64);
        const XMFLOAT4X4* lightVPs = _shadows.GetLightVPs();
        for (uint32_t c = 0; c < VulkanShadows::CASCADE_COUNT; c++)
            memcpy(ubo.lightVP[c], &lightVPs[c], 64);
        memcpy(ubo.cascadeSplits, _shadows.GetCascadeSplits(), 16);

        // Phase 18D: enable RT shadow in shader when pipeline and shadow mask are ready
        ubo.rtEnabled = (_rtSupported && _vkRTPipeline != VK_NULL_HANDLE
                         && _vkShadowMaskView != VK_NULL_HANDLE) ? 1u : 0u;
        // Phase 24: point light count
        ubo.numPointLights = _clusteredLightingReady ? (uint32_t)std::min((size_t)MAX_POINT_LIGHTS, _pointLights.size()) : 0u;

        memcpy(_deferredSceneCBMapped[_frameIndex], &ubo, sizeof(ubo));
    }

    // Phase 32: Visibility buffer — run vis pass + shade compute BEFORE render graph.
    // Shade compute overwrites G-buffer images (as UAVs) with correctly reconstructed attributes.
    // The render graph then reads those G-buffer images as SHADER_READ_ONLY_OPTIMAL as normal.
    if (_vkVisBufferReady && _vkVisBufferMode)
    {
        _gpuProfiler.WriteBeginTimestamp(cmd, "Visibility Pass");
        DrawVKVisibilityPass(cmd);
        _gpuProfiler.WriteEndTimestamp(cmd);
        _gpuProfiler.WriteBeginTimestamp(cmd, "Visibility Shade");
        DispatchVKVisibilityShade(cmd);
        _gpuProfiler.WriteEndTimestamp(cmd);
    }

    // PP resources are required; if missing, open ImGui pass and return early (black frame).
    bool ppReady = _postProcess.IsReady();
    if (!ppReady)
    {
        // PP resources not ready ??open ImGui pass and bail (black frame as error state)
        // Draw SSAO if the subsystem is ready
        if (_ssao.GetBlurredView() != VK_NULL_HANDLE) {
            _ssao.Draw(cmd, _frameIndex, _lastView, _lastProj);
            _ssao.DrawBlur(cmd);
        }
        // Swapchain image is UNDEFINED after acquire; _vkSwapchain.GetRenderPass() expects PRESENT_SRC_KHR.
        // Transition the swapchain image to PRESENT_SRC_KHR so the ImGui overlay pass layout is valid.
        {
            VkImageMemoryBarrier swapBarrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            swapBarrier.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
            swapBarrier.newLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            swapBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            swapBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            swapBarrier.image               = _vkSwapchain.GetImage(_imageIndex);
            swapBarrier.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            swapBarrier.srcAccessMask       = 0;
            swapBarrier.dstAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            vkCmdPipelineBarrier(cmd,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                0, 0, nullptr, 0, nullptr, 1, &swapBarrier);
        }
        VkClearValue clears[2]{};
        clears[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        clears[1].depthStencil = {1.0f, 0};
        VkRenderPassBeginInfo irpi{};
        irpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        irpi.renderPass = _vkSwapchain.GetRenderPass(); irpi.framebuffer = _vkSwapchain.GetFramebuffer(_imageIndex);
        irpi.renderArea.extent = _vkSwapchain.GetExtent(); irpi.clearValueCount = 2; irpi.pClearValues = clears;
        vkCmdBeginRenderPass(cmd, &irpi, VK_SUBPASS_CONTENTS_INLINE);
        return;
    }

    // ===================================================================
    // Phase 18C + Phase 26: VulkanRenderGraph — data-driven barrier
    // scheduling with transient resource aliasing support.
    // Phase 26: device + physicalDevice passed to enable CreateTransientImage().
    // Currently all images are imported (persistent); aliasing infrastructure
    // is ready for future migration of short-lived intermediates.
    // ===================================================================
    VulkanRenderGraph rg(_device->GetDevice(), _device->GetPhysicalDevice());

    // -- Import images with their post-G-buffer-pass states --
    // G-buffer: colour ??SHADER_READ_ONLY (render pass finalLayout),
    //           depth  ??DEPTH_STENCIL_READ_ONLY (render pass finalLayout).
    auto hGBAlbedo = rg.ImportImage(_gBuffer.GetAlbedoImage(),
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    auto hGBNormal = rg.ImportImage(_gBuffer.GetNormalImage(),
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    auto hGBMR = rg.ImportImage(_gBuffer.GetMetalRoughImage(),
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    auto hDepth = rg.ImportImage(_vkSwapchain.GetDepthImage(),
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_ASPECT_DEPTH_BIT);

    // SSAO intermediates (only if SSAO pipeline exists)
    VKRGHandle hSSAORT   = VKRG_NULL_HANDLE;
    VKRGHandle hSSAOBlur = VKRG_NULL_HANDLE;
    if (_ssao.IsReady())
    {
        hSSAORT = rg.ImportImage(_ssao.GetRawImage(),
            VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0);
        hSSAOBlur = rg.ImportImage(_ssao.GetBlurredImage(),
            VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0);
    }

    // RT shadow mask (only if RT supported and stable after resize)
    // Skip RT for 2 frames after resize to ensure all resources are stable
    VKRGHandle hShadowMask = VKRG_NULL_HANDLE;
    bool rtActive = _rtSupported && _vkRTPipeline != VK_NULL_HANDLE 
                    && _vkShadowMaskImage != VK_NULL_HANDLE
                    && _framesSinceResize >= 2;
    if (rtActive)
    {
        hShadowMask = rg.ImportImage(_vkShadowMaskImage,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0);
    }

    // HDR intermediate
    auto hHDR = rg.ImportImage(_postProcess.GetHDRImage(),
        VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0);

    // SSR image (kept in GENERAL)
    VKRGHandle hSSR = VKRG_NULL_HANDLE;
    if (_postProcess.IsSSRReady())
    {
        hSSR = rg.ImportImage(_postProcess.GetSSRImage(),
            VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    }

    // Motion blur
    VKRGHandle hMB = VKRG_NULL_HANDLE;
    bool mbActive = _postProcess.IsMotionBlurReady() && _postProcess.IsTAAReady();
    if (mbActive)
    {
        hMB = rg.ImportImage(_postProcess.GetMotionBlurImage(),
            VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0);
    }

    // TAA history (ping-pong)
    VKRGHandle hTAARead  = VKRG_NULL_HANDLE;
    VKRGHandle hTAAWrite = VKRG_NULL_HANDLE;
    bool taaActive = _postProcess.IsTAAReady();
    if (taaActive)
    {
        int writeIdx = _postProcess.GetTAAHistoryWriteIndex();
        int readIdx  = 1 - writeIdx;
        hTAARead = rg.ImportImage(_postProcess.GetTAAHistoryImage(readIdx),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT);
        hTAAWrite = rg.ImportImage(_postProcess.GetTAAHistoryImage(writeIdx),
            VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0);
    }

    // Bloom
    VKRGHandle hBloomBright = VKRG_NULL_HANDLE;
    VKRGHandle hBloomBlur   = VKRG_NULL_HANDLE;
    if (taaActive && _postProcess.GetBloomBrightImage())
    {
        hBloomBright = rg.ImportImage(_postProcess.GetBloomBrightImage(),
            VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0);
        hBloomBlur = rg.ImportImage(_postProcess.GetBloomBlurImage(),
            VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0);
    }

    // ?�?� Pass 1: SSAO ?�?�
    if (_ssao.IsReady())
    {
        rg.AddPass("SSAO")
            .Read(hDepth,    VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT)
            .Read(hGBNormal, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT)
            .Write(hSSAORT,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT)
            .SideEffect()
            .Execute([this](VkCommandBuffer c) { _ssao.Draw(c, _frameIndex, _lastView, _lastProj); });
    }

    // ?�?� Pass 2: SSAO Blur ?�?�
    if (_ssao.IsReady())
    {
        rg.AddPass("SSAO Blur")
            .Read(hSSAORT,     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT)
            .Write(hSSAOBlur,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT)
            .SideEffect()
            .Execute([this](VkCommandBuffer c) { _ssao.DrawBlur(c); });
    }

    // ?�?� Pass 3: RT Shadows ?�?�
    if (rtActive)
    {
        // Update RT scene UBO for this frame
        struct RTSceneUBO { float invViewProj[16]; float lightDir[3]; float maxDist; float _pad[44]; };
        {
            XMMATRIX V   = XMLoadFloat4x4(&_deferredView);
            XMMATRIX P   = XMLoadFloat4x4(&_deferredProj);
            XMMATRIX VP  = XMMatrixMultiply(V, P);
            XMVECTOR det = XMMatrixDeterminant(VP);
            XMMATRIX iVP = XMMatrixInverse(&det, VP);
            XMFLOAT4X4 iVPF; XMStoreFloat4x4(&iVPF, iVP);
            static const float kSqrt6Inv = 1.0f / 2.449490f;
            RTSceneUBO rtUBO{};
            memcpy(rtUBO.invViewProj, &iVPF, 64);
            rtUBO.lightDir[0] = kSqrt6Inv; rtUBO.lightDir[1] = 2.0f * kSqrt6Inv; rtUBO.lightDir[2] = kSqrt6Inv;
            rtUBO.maxDist = 100.0f;
            memcpy(_vkRTSceneCBMapped[_frameIndex], &rtUBO, sizeof(rtUBO));
        }

        // RT pass reads depth and normal via descriptor set - must declare for proper barriers
        rg.AddPass("RT Shadows")
            .Read(hDepth,      VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                               VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_SHADER_READ_BIT)
            .Read(hGBNormal,   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                               VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_SHADER_READ_BIT)
            .Write(hShadowMask, VK_IMAGE_LAYOUT_GENERAL,
                                VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_SHADER_WRITE_BIT)
            .SideEffect()
            .Execute([this](VkCommandBuffer c) {
                vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, _vkRTPipeline);
                vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, _vkRTPipeLayout,
                                         0, 1, &_vkRTDescSet[_frameIndex], 0, nullptr);
                pfn_vkCmdTraceRaysKHR(c, &_vkRgenRegion, &_vkMissRegion, &_vkHitRegion, &_vkCallRegion,
                                       _vkSwapchain.GetExtent().width, _vkSwapchain.GetExtent().height, 1);
            });
    }

    // ── Pass 3.5: Cluster Assign (compute — Phase 24) ──
    if (_clusteredLightingReady && !_pointLights.empty())
    {
        rg.AddPass("Cluster Assign")
            .SideEffect()
            .Execute([this](VkCommandBuffer c) {
                DispatchClusterAssign(c);
            });
    }

    // ── Pass 4: Deferred Lighting → HDR ──
    {
        auto& deferredPass = rg.AddPass("Deferred Lighting")
            .Read(hGBAlbedo, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT)
            .Read(hGBNormal, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT)
            .Read(hGBMR,     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT)
            .Read(hDepth,    VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT)
            .Write(hHDR,     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT)
            .SideEffect();

        if (_ssao.IsReady())
            deferredPass.Read(hSSAOBlur, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        if (rtActive)
            deferredPass.Read(hShadowMask, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);

        deferredPass.Execute([this](VkCommandBuffer c) {
            VkClearValue ltClear{};
            ltClear.color.float32[3] = 1.0f;

            VkRenderPassBeginInfo lrpi{};
            lrpi.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            lrpi.renderPass        = _postProcess.GetPPRenderPass();
            lrpi.framebuffer       = _postProcess.GetDeferredHDRFramebuffer();
            lrpi.renderArea.extent = _vkSwapchain.GetExtent();
            lrpi.clearValueCount   = 1;
            lrpi.pClearValues      = &ltClear;
            vkCmdBeginRenderPass(c, &lrpi, VK_SUBPASS_CONTENTS_INLINE);

            VkViewport vp{ 0, 0, (float)_vkSwapchain.GetExtent().width, (float)_vkSwapchain.GetExtent().height, 0, 1 };
            vkCmdSetViewport(c, 0, 1, &vp);
            VkRect2D sc{ {0,0}, _vkSwapchain.GetExtent() };
            vkCmdSetScissor(c, 0, 1, &sc);

            // Phase 30: GI pipeline requires cluster set (set=2) for set=3 to be reachable
            bool useGI  = _gi.IsReady() && _deferredGIPipeline && _clusterLightDescSet;
            bool useIBL = !useGI && _ibl.IsReady() && _deferredIBLPipeline;

            VkPipeline       activePipeline = useGI  ? _deferredGIPipeline
                                            : useIBL ? _deferredIBLPipeline
                                                     : _deferredPipeline;
            VkPipelineLayout activeLayout   = useGI  ? _deferredGIPipeLayout : _deferredPipeLayout;

            vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, activePipeline);

            if (useGI) {
                // Update per-frame GI descriptor set (ssgi ping-pong view + probe grid UBO)
                UpdateGIDescriptorSet(_frameIndex);
                VkDescriptorSet dSets4[] = {
                    _deferredSceneDescSet[_frameIndex],
                    _deferredGbufDescSet,
                    _clusterLightDescSet,
                    _giDescSet[_frameIndex]
                };
                vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                         activeLayout, 0, 4, dSets4, 0, nullptr);
            } else if (_clusterLightDescSet) {
                VkDescriptorSet dSets3[] = { _deferredSceneDescSet[_frameIndex], _deferredGbufDescSet, _clusterLightDescSet };
                vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                         activeLayout, 0, 3, dSets3, 0, nullptr);
            } else {
                VkDescriptorSet dSets[] = { _deferredSceneDescSet[_frameIndex], _deferredGbufDescSet };
                vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                         activeLayout, 0, 2, dSets, 0, nullptr);
            }
            vkCmdDraw(c, 3, 1, 0, 0);
            vkCmdEndRenderPass(c);
        });
    }

    // ── Phase 28: Atmosphere sky-view LUT update + sky composite ──
    if (_atmosphere.IsReady())
    {
        // Pass 4.5: Sky-view LUT compute (reads transmittance+multiscatter, writes skyView)
        rg.AddPass("Atmo SkyView LUT")
            .SideEffect()
            .Execute([this](VkCommandBuffer c) {
                _atmosphere.Update(c, _frameIndex, _deferredView, _deferredProj);
            });

        // Pass 4.6: Sky composite (reads skyView+depth, composites sky onto HDR via LOAD_OP_LOAD)
        // HDR is NOT read as sceneTex — scene content is preserved by the atmosphere render pass
        // LOAD op. The render pass transitions HDR: SHADER_READ_ONLY → COLOR_ATTACHMENT → SHADER_READ_ONLY.
        rg.AddPass("Sky Composite")
            .Read(hDepth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT)
            .Write(hHDR,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT)
            .SideEffect()
            .Execute([this](VkCommandBuffer c) {
                _atmosphere.DrawComposite(c, _frameIndex);
            });
    }

    // ── Pass 4.6: SSGI + Probe Update (compute, after SSAO) ──
    if (_gi.IsReady())
    {
        rg.AddPass("SSGI")
            .Read(hDepth,   VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT)
            .SideEffect()
            .Execute([this](VkCommandBuffer c) {
                XMFLOAT4X4 invVPf, prevVPf, viewf;
                XMMATRIX view = XMLoadFloat4x4(&_deferredView);
                XMMATRIX proj = XMLoadFloat4x4(&_deferredProj);
                XMStoreFloat4x4(&invVPf, XMMatrixInverse(nullptr, XMMatrixMultiply(view, proj)));
                // prevVP: stored in _vkPrevUnjitteredVP as float[16], copy it
                memcpy(&prevVPf, _vkPrevUnjitteredVP, 64);
                XMStoreFloat4x4(&viewf, view);
                _gi.Dispatch(c, _frameIndex, invVPf, prevVPf, viewf);
            });
    }

    // ── Pass 4.7: Volumetric Fog Compute (inject + scatter, no HDR dependency) ──
    if (_volumetricFog.IsReady() && _volFogEnabled)
    {
        rg.AddPass("Vol Fog Compute")
            .Read(hDepth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT)
            .SideEffect()
            .Execute([this](VkCommandBuffer c) {
                const XMFLOAT4X4* csmVP = _shadows.GetLightVPs();
                const float*      splits = _shadows.GetCascadeSplits();
                XMFLOAT4 cascadeSplits{ splits[0], splits[1], splits[2], splits[3] };
                _volumetricFog.Dispatch(c, _frameIndex, _deferredView, _deferredProj,
                                        csmVP, cascadeSplits);
            });
    }

    // ── Pass 4.8: Volumetric Fog Apply (additive blend onto HDR) ──
    if (_volumetricFog.IsReady() && _volFogEnabled)
    {
        rg.AddPass("Vol Fog Apply")
            .Write(hHDR,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT)
            .SideEffect()
            .Execute([this](VkCommandBuffer c) {
                _volumetricFog.DrawApply(c, _frameIndex);
            });
    }

    // ── Pass 4.9: OIT Forward + Composite (after fog/sky, before SSR) ──
    if (_vkOitReady && !_vkOitMeshes.empty())
    {
        rg.AddPass("OIT Forward")
            .Read(hDepth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT)
            .SideEffect()
            .Execute([this](VkCommandBuffer c) { DrawVKOITForward(c); });

        rg.AddPass("OIT Composite")
            .Write(hHDR, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT)
            .SideEffect()
            .Execute([this](VkCommandBuffer c) {
                DrawVKOITComposite(c);
                _vkOitMeshes.clear();
            });
    }

    // ── Pass 5: SSR Compute ──
    if (_postProcess.IsSSRReady())
    {
        rg.AddPass("SSR Compute")
            .Read(hHDR,      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT)
            .Read(hDepth,    VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT)
            .Read(hGBNormal, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT)
            .Read(hGBMR,     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT)
            .Write(hSSR,     VK_IMAGE_LAYOUT_GENERAL,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT)
            .SideEffect()
            .Execute([this](VkCommandBuffer c) {
                _postProcess.DrawSSR(c, _frameIndex, _deferredView, _deferredProj);
            });
    }

    // ?�?� Pass 6: Motion Blur ?�?�
    if (mbActive)
    {
        rg.AddPass("Motion Blur")
            .Read(hHDR,   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT)
            .Read(hDepth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT)
            .Write(hMB,   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT)
            .SideEffect()
            .Execute([this](VkCommandBuffer c) {
                XMFLOAT4X4 prevVP;
                memcpy(&prevVP, _postProcess.GetPrevUnjitteredVP(), 64);
                _postProcess.DrawMotionBlur(c, _frameIndex, _deferredView, _deferredProj, prevVP);
            });
    }

    if (taaActive)
    {
        // Determine TAA source: motion blur output if available, else HDR
        VKRGHandle hTAASrc = mbActive ? hMB : hHDR;

        // ?�?� Pass 7: TAA ?�?�
        rg.AddPass("TAA")
            .Read(hTAASrc,   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT)
            .Read(hTAARead,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT)
            .Read(hDepth,    VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT)
            .Write(hTAAWrite, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT)
            .SideEffect()
            .Execute([this](VkCommandBuffer c) {
                // Build TAA constants
                XMMATRIX V   = XMLoadFloat4x4(&_deferredView);
                XMMATRIX P   = XMLoadFloat4x4(&_deferredProj);
                XMMATRIX VP  = XMMatrixMultiply(V, P);
                XMMATRIX iVP = XMMatrixInverse(nullptr, VP);
                XMFLOAT4X4 iVPF; XMStoreFloat4x4(&iVPF, iVP);

                VulkanPostProcess::TAAConstants taaConst{};
                memcpy(taaConst.invViewProj, &iVPF, 64);
                memcpy(taaConst.prevViewProj, _postProcess.GetPrevUnjitteredVP(), 64);
                taaConst.jitter[0]     = _vkCurJitter[0];  taaConst.jitter[1]     = _vkCurJitter[1];
                taaConst.prevJitter[0] = _vkPrevJitter[0]; taaConst.prevJitter[1] = _vkPrevJitter[1];
                taaConst.alpha = (_postProcess.GetFrameCount() < 8) ? 1.0f : 0.1f;

                _postProcess.DrawTAA(c, _frameIndex, taaConst);
            });

        // ?�?� Pass 8: Bloom Bright ?�?�
        if (_postProcess.GetBloomBrightImage())
        {
            rg.AddPass("Bloom Bright")
                .Read(hTAAWrite,    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT)
                .Write(hBloomBright, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT)
                .SideEffect()
                .Execute([this](VkCommandBuffer c) { _postProcess.DrawBloomBright(c); });

            // ?�?� Pass 9: Bloom Blur H ?�?�
            rg.AddPass("Bloom Blur H")
                .Read(hBloomBright, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT)
                .Write(hBloomBlur,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT)
                .SideEffect()
                .Execute([this](VkCommandBuffer c) { _postProcess.DrawBloomBlur(c, true); });

            // ?�?� Pass 10: Bloom Blur V ?�?�
            rg.AddPass("Bloom Blur V")
                .Read(hBloomBlur,    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT)
                .Write(hBloomBright, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT)
                .SideEffect()
                .Execute([this](VkCommandBuffer c) { _postProcess.DrawBloomBlur(c, false); });
        }

        // ?�?� Pass 11: Tonemap ??swapchain ?�?�
        {
            auto& tonemapPass = rg.AddPass("Tonemap")
                .Read(hTAAWrite, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT)
                .SideEffect();
            if (_postProcess.GetBloomBrightImage())
                tonemapPass.Read(hBloomBright, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
            if (_postProcess.IsSSRReady())
                tonemapPass.Read(hSSR, VK_IMAGE_LAYOUT_GENERAL,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
            tonemapPass.Execute([this](VkCommandBuffer c) { _postProcess.DrawTonemap(c, _imageIndex); });
        }
    }
    else
    {
        // Phase 16C fallback: simple HDR + SSR ??swapchain (no TAA)
        auto& fallbackPass = rg.AddPass("SSR Tonemap Fallback")
            .Read(hHDR, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT)
            .SideEffect();
        if (_postProcess.IsSSRReady())
            fallbackPass.Read(hSSR, VK_IMAGE_LAYOUT_GENERAL,
                              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        fallbackPass.Execute([this](VkCommandBuffer c) {
            VkClearValue ltClear{};
            ltClear.color.float32[3] = 1.0f;

            VkRenderPassBeginInfo trpi{};
            trpi.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            trpi.renderPass        = _tonemapRenderPass;
            trpi.framebuffer       = _tonemapFramebuffers[_imageIndex];
            trpi.renderArea.extent = _vkSwapchain.GetExtent();
            trpi.clearValueCount   = 1;
            trpi.pClearValues      = &ltClear;
            vkCmdBeginRenderPass(c, &trpi, VK_SUBPASS_CONTENTS_INLINE);

            VkViewport vp{ 0, 0, (float)_vkSwapchain.GetExtent().width, (float)_vkSwapchain.GetExtent().height, 0, 1 };
            vkCmdSetViewport(c, 0, 1, &vp);
            VkRect2D sc{ {0,0}, _vkSwapchain.GetExtent() };
            vkCmdSetScissor(c, 0, 1, &sc);

            vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, _vkSSRTonemapPipeline);
            vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                     _vkSSRTonemapPipeLayout, 0, 1, &_vkSSRTonemapDescSet, 0, nullptr);
            vkCmdDraw(c, 3, 1, 0, 0);
            vkCmdEndRenderPass(c);
        });
    }

    // Compile (DAG cull) + Execute (emit barriers + run lambdas)
    rg.Compile();
    rg.Execute(cmd, &_gpuProfiler);

    // ─── Open ImGui overlay render pass (_vkSwapchain.GetRenderPass(), LOAD from swapchain) ───
    VkClearValue imguiClears[2]{};
    imguiClears[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    imguiClears[1].depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo irpi{};
    irpi.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    irpi.renderPass        = _vkSwapchain.GetRenderPass();
    irpi.framebuffer       = _vkSwapchain.GetFramebuffer(_imageIndex);
    irpi.renderArea.extent = _vkSwapchain.GetExtent();
    irpi.clearValueCount   = 2;
    irpi.pClearValues      = imguiClears;
    vkCmdBeginRenderPass(cmd, &irpi, VK_SUBPASS_CONTENTS_INLINE);
}

void VulkanBackend::EndFrame()
{
    if (!_frameActive || _deviceLost) return;
    auto& frame = _frames[_frameIndex];
    vkCmdEndRenderPass(frame.cmdBuffer);   // ends the ImGui overlay pass opened in CompositeFrame


    vkEndCommandBuffer(frame.cmdBuffer);

    // Phase 20: Build wait semaphore array (imageReady + optionally computeDone)
    VkSemaphore waitSemaphores[2]          = { frame.imageReady, VK_NULL_HANDLE };
    VkPipelineStageFlags waitStages[2]     = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                               VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT };
    uint32_t waitCount = 1;
    if (_computeSubmittedThisFrame)
    {
        waitSemaphores[1] = _computeFrames[_frameIndex].doneSemaphore;
        waitCount = 2;
        _computeSubmittedThisFrame = false;
    }

    VkSubmitInfo si{};
    si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount   = waitCount;
    si.pWaitSemaphores      = waitSemaphores;
    si.pWaitDstStageMask    = waitStages;
    si.commandBufferCount   = 1;
    si.pCommandBuffers      = &frame.cmdBuffer;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores    = &frame.renderDone;
    VkResult submitRes = vkQueueSubmit(_device->GetGraphicsQueue(), 1, &si, frame.fence);
    if (submitRes == VK_ERROR_DEVICE_LOST)
    {
        LUNA_LOG_ERROR("VK: Device lost during queue submit");
        _deviceLost = true;
        return;
    }

    VkResult r = _vkSwapchain.Present(_device->GetPresentQueue(), frame.renderDone, _imageIndex);
    if (r == VK_ERROR_DEVICE_LOST)
    {
        LUNA_LOG_ERROR("VK: Device lost during present");
        _deviceLost = true;
        return;
    }
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        // Wait for ALL in-flight frames before recreation
        for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++)
        {
            if (_frames[i].fence != VK_NULL_HANDLE)
            {
                VkResult wr = vkWaitForFences(_device->GetDevice(), 1, &_frames[i].fence, VK_TRUE, UINT64_MAX);
                if (wr == VK_ERROR_DEVICE_LOST)
                {
                    LUNA_LOG_ERROR("VK: Device lost during present resize fence wait");
                    _deviceLost = true;
                    return;
                }
            }
        }
        vkDeviceWaitIdle(_device->GetDevice());
        RecreateSwapchain();
        for (uint32_t ii = 0; ii < _vkSwapchain.GetImageCount(); ++ii) _vkSwapchain.SetImageFence(ii, VK_NULL_HANDLE);
        _framesSinceResize = 0;  // Reset resize cooldown
    }
    _frameIndex = (_frameIndex + 1) % FRAMES_IN_FLIGHT;
    _gpuProfiler.EndFrame();
}

// ===========================================================================
// UpdateMVP / DrawMesh
// ===========================================================================
void VulkanBackend::UpdateMVP(const XMFLOAT4X4&, const XMFLOAT4X4& view, const XMFLOAT4X4& proj)
{
    _lastView     = view;
    _lastProj     = proj;
    _deferredView = view;
    _deferredProj = proj;


    // Store unjittered VP for TAA reprojection (before applying jitter)
    {
        XMMATRIX V  = XMLoadFloat4x4(&view);
        XMMATRIX P  = XMLoadFloat4x4(&proj);
        XMMATRIX VP = XMMatrixMultiply(V, P);
        XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(_vkUnjitteredVP), VP);
        _postProcess.SetUnjitteredVP(_vkUnjitteredVP);
    }

    // Phase 10: Halton(2,3) jitter ??sub-pixel TAA offset on projection matrix.
    _vkPrevJitter[0] = _vkCurJitter[0];
    _vkPrevJitter[1] = _vkCurJitter[1];
    if (_postProcess.IsTAAReady()) {
        auto Halton = [](int i, int b) -> float {
            float r = 0.0f, f = 1.0f / float(b);
            for (; i > 0; i /= b, f /= float(b)) r += f * float(i % b);
            return r;
        };
        int idx = int(_postProcess.GetFrameCount() % 16);
        _vkCurJitter[0] = (Halton(idx + 1, 2) - 0.5f) * 1.0f / float(_vkSwapchain.GetExtent().width);
        _vkCurJitter[1] = (Halton(idx + 1, 3) - 0.5f) * 1.0f / float(_vkSwapchain.GetExtent().height);

        // Apply jitter to both projections (G-buffer + deferred lighting must match)
        _lastProj._31     += _vkCurJitter[0];
        _lastProj._32     += _vkCurJitter[1];
        _deferredProj._31 += _vkCurJitter[0];
        _deferredProj._32 += _vkCurJitter[1];

        _postProcess.SetJitter(_vkCurJitter[0], _vkCurJitter[1], _vkPrevJitter[0], _vkPrevJitter[1]);
    } else {
        _vkCurJitter[0] = 0.0f;
        _vkCurJitter[1] = 0.0f;
        memcpy(_vkUnjitteredVP, _vkUnjitteredVP, 64); // no-op, already set
    }

    // Phase 5C: Recover eye position from view matrix (DirectXMath row-major convention).
    // View matrix row 3 holds: (-dot(xaxis,E), -dot(yaxis,E), -dot(zaxis,E), 1).
    // Rotation submatrix is orthogonal, so E = R * negT where negT = -row3.xyz
    float negTx = -view._41, negTy = -view._42, negTz = -view._43;
    float eyeX  = view._11 * negTx + view._12 * negTy + view._13 * negTz;
    float eyeY  = view._21 * negTx + view._22 * negTy + view._23 * negTz;
    float eyeZ  = view._31 * negTx + view._32 * negTy + view._33 * negTz;

    // Write per-frame SceneUBO ??matches SceneBuffer layout in pbr_forward.frag.hlsl
    // lightDir = normalize(1,2,1), lightIntensity=3.0, lightColor=(1,1,1)
    // (same values as the DX12 deferred lighting path for visual consistency)
    if (_frames[_frameIndex].sceneMapped)
    {
        static const float kSqrt6Inv = 1.0f / 2.449490f;  // 1/sqrt(6)
        float data[12] = {
            eyeX,        eyeY,        eyeZ,        0.0f,  // eyePosition + pad
            kSqrt6Inv,   2.0f * kSqrt6Inv, kSqrt6Inv, 3.0f,  // lightDir (norm(1,2,1)) + lightIntensity
            1.0f,        1.0f,        1.0f,        0.0f,  // lightColor + pad
        };
        memcpy(_frames[_frameIndex].sceneMapped, data, sizeof(data));
    }
}

void VulkanBackend::DrawMesh(const Mesh*, const XMFLOAT4X4& model)
{
    if (!_frameActive) return;
    if (_drawCallIndex >= _vkSceneMeshes.size()) return;

    auto& m = _vkSceneMeshes[_drawCallIndex];
    if (!m) { _drawCallIndex++; return; }

    // Cache model matrix for CSM depth pre-pass next frame
    if (_drawCallIndex < _lastMeshModels.size())
        _lastMeshModels[_drawCallIndex] = model;
    else
        _lastMeshModels.push_back(model);

    // Phase 31: transparent meshes bypass GPU-driven path → OIT deferred draw list
    if (_vkOitReady && m->material && m->material->alpha < 1.0f)
    {
        if (_vkOitMeshes.size() < MAX_OIT_MESHES)
            _vkOitMeshes.push_back({ m.get(), model, m->material->alpha });
        _drawCallIndex++;
        return;
    }

    // Phase 15B: GPU-driven path ??accumulate instance instead of recording draw
    if (_gpuDrivenReady && m->material && _cpuInstances.size() < MAX_GPU_OBJECTS) {
        GPUObjectDataVK obj{};
        obj.model          = model;
        obj.boundingSphere = m->boundingSphere;
        obj.meshIndex      = _drawCallIndex;
        obj.materialIndex  = m->material->bindlessIndex;
        obj._unused        = 0;
        _cpuInstances.push_back(obj);
        _drawCallIndex++;
        return;
    }

    // Legacy immediate draw path
    if (!_graphicsPipeline || !m->material) { _drawCallIndex++; return; }
    if (_drawCallIndex >= VkFrameResource::MAX_DRAWS) {
        LUNA_LOG_WARN("VK DrawMesh: exceeded MAX_DRAWS (%u)", VkFrameResource::MAX_DRAWS);
        _drawCallIndex++;
        return;
    }

    auto& frame = _frames[_frameIndex];
    VkCommandBuffer cmd = frame.cmdBuffer;

    static constexpr uint32_t MVP_SLOT_SIZE = 256;
    struct MVPData { XMFLOAT4X4 model, view, proj; };
    MVPData mvp{ model, _lastView, _lastProj };
    uint8_t* dst = static_cast<uint8_t*>(frame.mvpMapped) + _drawCallIndex * MVP_SLOT_SIZE;
    memcpy(dst, &mvp, sizeof(MVPData));

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _gbPipeline);
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &m->vertexBuffer, &off);
    vkCmdBindIndexBuffer(cmd, m->indexBuffer, 0, VK_INDEX_TYPE_UINT32);
    uint32_t dynOffset = static_cast<uint32_t>(_drawCallIndex) * MVP_SLOT_SIZE;
    VkDescriptorSet sets[] = { frame.mvpDescSet, m->material->descSet };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                             _pipelineLayout, 0, 2, sets, 1, &dynOffset);
    vkCmdDrawIndexed(cmd, m->indexCount, 1, 0, 0, 0);

    _drawCallIndex++;
}

// ===========================================================================
// LoadMeshes
// ===========================================================================
std::vector<std::shared_ptr<Mesh>> VulkanBackend::LoadMeshes(const std::string& path)
{
    cgltf_options opts = {}; cgltf_data* data = nullptr;
    if (cgltf_parse_file(&opts, path.c_str(), &data) != cgltf_result_success)
    { LUNA_LOG_ERROR("VK LoadMeshes: failed %s", path.c_str()); return {}; }
    cgltf_load_buffers(&opts, data, path.c_str());

    // Bug #5 fix: clear previous meshes before reloading to prevent duplicates
    if (_device) vkDeviceWaitIdle(_device->GetDevice());
    _vkSceneMeshes.clear();

    std::string dir;
    size_t slash = path.find_last_of("/\\");
    if (slash != std::string::npos) dir = path.substr(0, slash + 1);

    VkDevice dev = _device->GetDevice();

    // Shared sampler ??anisotropic to match DX12 (reduces UV seam artifacts at triangle edges)
    if (_linearSampler == VK_NULL_HANDLE) {
        VkSamplerCreateInfo si{};
        si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter    = VK_FILTER_LINEAR;
        si.minFilter    = VK_FILTER_LINEAR;
        si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        si.anisotropyEnable = VK_TRUE;
        si.maxAnisotropy    = 8.0f;
        si.maxLod           = 0.0f;   // only 1 mip level ??clamp LOD to 0
        vkCreateSampler(dev, &si, nullptr, &_linearSampler);
    }

    // Upload RGBA8 pixels ??DEVICE_LOCAL VkImage
    auto uploadTex = [&](const std::vector<uint8_t>& px, uint32_t w, uint32_t h) -> VkTexture {
        VkTexture t{};
        VkDeviceSize sz = (VkDeviceSize)w * h * 4;

        VkBuffer stg; VkDeviceMemory stgMem;
        CreateBuffer(sz,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            stg, stgMem);

        void* mp;
        vkMapMemory(dev, stgMem, 0, sz, 0, &mp);
        memcpy(mp, px.data(), (size_t)sz);
        vkUnmapMemory(dev, stgMem);

        CreateImage(w, h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, t.image, t.memory);

        TransitionImageLayout(t.image, VK_IMAGE_LAYOUT_UNDEFINED,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        CopyBufferToImage(stg, t.image, w, h);
        TransitionImageLayout(t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        vkDestroyBuffer(dev, stg, nullptr);
        vkFreeMemory(dev, stgMem, nullptr);

        t.view = CreateImageView(t.image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);
        return t;
    };

    auto fallback = [&](uint8_t r, uint8_t g, uint8_t b, uint8_t a) -> VkTexture {
        return uploadTex({r,g,b,a}, 1, 1);
    };

    auto decodeImg = [&](const cgltf_image* img)
        -> std::pair<std::vector<uint8_t>, std::pair<uint32_t,uint32_t>>
    {
        int w, h, c; stbi_uc* px = nullptr;
        if (img->buffer_view && img->buffer_view->buffer && img->buffer_view->buffer->data) {
            const uint8_t* src = (const uint8_t*)img->buffer_view->buffer->data + img->buffer_view->offset;
            px = stbi_load_from_memory(src, (int)img->buffer_view->size, &w, &h, &c, 4);
        } else if (img->uri) {
            std::string fp = dir + img->uri;
            px = stbi_load(fp.c_str(), &w, &h, &c, 4);
        }
        if (!px) return {{}, {0,0}};
        std::vector<uint8_t> buf(px, px + w*h*4);
        stbi_image_free(px);
        return {buf, {(uint32_t)w, (uint32_t)h}};
    };

    // Phase 5C: MatUBO now contains only per-material PBR factors.
    // Per-frame scene data (eye pos, light dir/color) moved to per-frame SceneUBO.
    struct MatUBO {
        float albedo[4];         // albedoFactor (RGBA)
        float metallic;          // metallicFactor
        float roughness;         // roughnessFactor
        float _pad[2];
    };  // 32 B

    std::vector<std::shared_ptr<VkMaterial>> mats(data->materials_count);
    for (cgltf_size mi = 0; mi < data->materials_count; mi++) {
        auto mat = std::make_shared<VkMaterial>();
        const cgltf_material& gm = data->materials[mi];

        MatUBO ubo{};
        ubo.albedo[0] = ubo.albedo[1] = ubo.albedo[2] = ubo.albedo[3] = 1.0f;
        ubo.metallic  = 0.0f;
        ubo.roughness = 0.5f;

        if (gm.has_pbr_metallic_roughness) {
            const auto& pbr = gm.pbr_metallic_roughness;
            for (int k = 0; k < 4; k++) ubo.albedo[k] = pbr.base_color_factor[k];
            ubo.metallic  = pbr.metallic_factor;
            ubo.roughness = pbr.roughness_factor;
            // GPU-driven SSBO factors (CreateIndirectResources reads these)
            for (int k = 0; k < 4; k++) mat->albedoFactor[k] = pbr.base_color_factor[k];
            mat->metallicFactor  = pbr.metallic_factor;
            mat->roughnessFactor = pbr.roughness_factor;

            if (pbr.base_color_texture.texture && pbr.base_color_texture.texture->image) {
                auto [px,dim] = decodeImg(pbr.base_color_texture.texture->image);
                LUNA_LOG_INFO("VK Mat[%zu] albedo: %ux%u, first4px=[%u,%u,%u,%u]",
                    mi, dim.first, dim.second,
                    px.size()>=4 ? px[0]:0, px.size()>=4 ? px[1]:0,
                    px.size()>=4 ? px[2]:0, px.size()>=4 ? px[3]:0);
                mat->albedo = px.empty() ? fallback(255,255,255,255) : uploadTex(px,dim.first,dim.second);
            } else mat->albedo = fallback(255,255,255,255);

            if (pbr.metallic_roughness_texture.texture && pbr.metallic_roughness_texture.texture->image) {
                auto [px,dim] = decodeImg(pbr.metallic_roughness_texture.texture->image);
                LUNA_LOG_INFO("VK Mat[%zu] metalRough: %ux%u, first4px=[%u,%u,%u,%u]",
                    mi, dim.first, dim.second,
                    px.size()>=4 ? px[0]:0, px.size()>=4 ? px[1]:0,
                    px.size()>=4 ? px[2]:0, px.size()>=4 ? px[3]:0);
                mat->metalRough = px.empty() ? fallback(0,128,0,255) : uploadTex(px,dim.first,dim.second);
            } else mat->metalRough = fallback(0,128,0,255);
        } else {
            mat->albedo     = fallback(255,255,255,255);
            mat->metalRough = fallback(0,128,0,255);
        }

        if (gm.normal_texture.texture && gm.normal_texture.texture->image) {
            auto [px,dim] = decodeImg(gm.normal_texture.texture->image);
            LUNA_LOG_INFO("VK Mat[%zu] normal: %ux%u, first4px=[%u,%u,%u,%u]",
                mi, dim.first, dim.second,
                px.size()>=4 ? px[0]:0, px.size()>=4 ? px[1]:0,
                px.size()>=4 ? px[2]:0, px.size()>=4 ? px[3]:0);
            mat->normalMap = px.empty() ? fallback(128,128,255,255) : uploadTex(px,dim.first,dim.second);
        } else mat->normalMap = fallback(128,128,255,255);

        // Emissive texture (fallback: black)
        if (gm.emissive_texture.texture && gm.emissive_texture.texture->image) {
            auto [px,dim] = decodeImg(gm.emissive_texture.texture->image);
            LUNA_LOG_INFO("VK Mat[%zu] emissive: %ux%u", mi, dim.first, dim.second);
            mat->emissive = px.empty() ? fallback(0,0,0,255) : uploadTex(px,dim.first,dim.second);
        } else mat->emissive = fallback(0,0,0,255);

        LUNA_LOG_INFO("VK Mat[%zu] factors: albedo=(%.2f,%.2f,%.2f,%.2f) met=%.2f rough=%.2f",
            mi, ubo.albedo[0], ubo.albedo[1], ubo.albedo[2], ubo.albedo[3],
            ubo.metallic, ubo.roughness);

        CreateBuffer(sizeof(MatUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            mat->ubo, mat->uboMem);
        vkMapMemory(dev, mat->uboMem, 0, sizeof(MatUBO), 0, &mat->uboMapped);
        memcpy(mat->uboMapped, &ubo, sizeof(MatUBO));

        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool     = _sceneDescPool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts        = &_materialDescLayout;
        vkAllocateDescriptorSets(dev, &dsai, &mat->descSet);

        VkDescriptorBufferInfo uboBI{ mat->ubo, 0, sizeof(MatUBO) };
        VkDescriptorImageInfo aII{ _linearSampler, mat->albedo.view,    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo nII{ _linearSampler, mat->normalMap.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo mII{ _linearSampler, mat->metalRough.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo sII{ _linearSampler, VK_NULL_HANDLE,      VK_IMAGE_LAYOUT_UNDEFINED };

        VkWriteDescriptorSet ws[5]{};
        ws[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, mat->descSet, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &uboBI, nullptr };
        ws[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, mat->descSet, 1, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  &aII,    nullptr, nullptr };
        ws[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, mat->descSet, 2, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  &nII,    nullptr, nullptr };
        ws[3] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, mat->descSet, 3, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  &mII,    nullptr, nullptr };
        ws[4] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, mat->descSet, 4, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER,        &sII,    nullptr, nullptr };
        vkUpdateDescriptorSets(dev, 5, ws, 0, nullptr);

        mats[mi] = mat;
    }

    std::vector<std::shared_ptr<Mesh>> dummy;

    // Phase 15B: collect all vertex/index data for merged geometry
    std::vector<std::vector<PBRVertex>> allVerts;
    std::vector<std::vector<uint32_t>> allIdxs;

    // Phase 21: clear stored transforms
    _lastLoadTransforms.clear();

    // Phase 21: helper ??process a single primitive with a world transform
    auto ProcessPrimitive = [&](cgltf_mesh* gltfMesh, cgltf_size pi,
                                 const XMFLOAT4X4& worldTransform)
    {
        const cgltf_primitive& prim = gltfMesh->primitives[pi];
        if (prim.type != cgltf_primitive_type_triangles) return;

        cgltf_accessor *posA=nullptr, *nrmA=nullptr, *uvA=nullptr, *tanA=nullptr;
        for (cgltf_size ai = 0; ai < prim.attributes_count; ai++) {
            auto& a = prim.attributes[ai];
            if      (a.type==cgltf_attribute_type_position)                posA=a.data;
            else if (a.type==cgltf_attribute_type_normal)                  nrmA=a.data;
            else if (a.type==cgltf_attribute_type_texcoord && a.index==0)  uvA =a.data;
            else if (a.type==cgltf_attribute_type_tangent)                 tanA=a.data;
        }
        if (!posA) return;

        cgltf_size vc = posA->count;
        std::vector<PBRVertex> verts(vc);
        for (cgltf_size v = 0; v < vc; v++) {
            float p[3]={},n[3]={0,1,0},uv[2]={},t[4]={1,0,0,1};
            cgltf_accessor_read_float(posA, v, p, 3);
            if (nrmA) cgltf_accessor_read_float(nrmA, v, n, 3);
            if (uvA)  cgltf_accessor_read_float(uvA,  v, uv, 2);
            if (tanA) cgltf_accessor_read_float(tanA, v, t, 4);
            verts[v] = {{p[0],p[1],p[2]},{n[0],n[1],n[2]},{uv[0],uv[1]},{t[0],t[1],t[2],t[3]}};
        }

        std::vector<uint32_t> idxs;
        if (prim.indices) {
            idxs.resize(prim.indices->count);
            for (cgltf_size i = 0; i < prim.indices->count; i++)
                idxs[i] = (uint32_t)cgltf_accessor_read_index(prim.indices, i);
        } else {
            idxs.resize(vc);
            for (cgltf_size i = 0; i < vc; i++) idxs[i] = (uint32_t)i;
        }

        auto sm = std::make_shared<VkSceneMesh>();
        sm->indexCount = (uint32_t)idxs.size();

        // Compute object-space bounding sphere (centroid + max radius)
        {
            XMFLOAT3 centroid{0,0,0};
            for (const auto& vtx : verts) {
                centroid.x += vtx.position.x;
                centroid.y += vtx.position.y;
                centroid.z += vtx.position.z;
            }
            float invN = 1.0f / float(std::max(vc, cgltf_size(1)));
            centroid.x *= invN; centroid.y *= invN; centroid.z *= invN;
            float maxR = 0.0f;
            for (const auto& vtx : verts) {
                float dx = vtx.position.x - centroid.x;
                float dy = vtx.position.y - centroid.y;
                float dz = vtx.position.z - centroid.z;
                maxR = std::max(maxR, dx*dx + dy*dy + dz*dz);
            }
            sm->boundingSphere = {centroid.x, centroid.y, centroid.z, std::sqrt(maxR)};
        }

        // Vertex buffer
        VkDeviceSize vbs = verts.size() * sizeof(PBRVertex);
        VkBuffer vs2; VkDeviceMemory vm;
        CreateBuffer(vbs, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            vs2, vm);
        void* vp; vkMapMemory(dev,vm,0,vbs,0,&vp); memcpy(vp,verts.data(),(size_t)vbs); vkUnmapMemory(dev,vm);
        CreateBuffer(vbs,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            sm->vertexBuffer, sm->vertexMemory);
        CopyBuffer(vs2, sm->vertexBuffer, vbs);
        vkDestroyBuffer(dev,vs2,nullptr); vkFreeMemory(dev,vm,nullptr);

        // Index buffer
        VkDeviceSize ibs = idxs.size() * sizeof(uint32_t);
        VkBuffer is2; VkDeviceMemory im;
        CreateBuffer(ibs, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            is2, im);
        void* ip; vkMapMemory(dev,im,0,ibs,0,&ip); memcpy(ip,idxs.data(),(size_t)ibs); vkUnmapMemory(dev,im);
        CreateBuffer(ibs,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            sm->indexBuffer, sm->indexMemory);
        CopyBuffer(is2, sm->indexBuffer, ibs);
        vkDestroyBuffer(dev,is2,nullptr); vkFreeMemory(dev,im,nullptr);

        UINT matIdx = prim.material ? (UINT)(prim.material - data->materials) : 0;
        sm->material = (matIdx < mats.size()) ? mats[matIdx] : nullptr;
        _vkSceneMeshes.push_back(sm);
        _lastLoadTransforms.push_back(worldTransform);

        // Build display name
        std::string displayName;
        cgltf_size mi = (cgltf_size)(gltfMesh - data->meshes);
        if (gltfMesh->name && gltfMesh->name[0])
            displayName = gltfMesh->name;
        else
            displayName = "Mesh_" + std::to_string(mi);
        if (gltfMesh->primitives_count > 1)
            displayName += "/Prim_" + std::to_string(pi);

        allVerts.push_back(std::move(verts));
        allIdxs.push_back(std::move(idxs));

        auto d = std::make_shared<Mesh>(); d->indexCount = sm->indexCount;
        d->name = displayName;
        dummy.push_back(d);
    };

    // Phase 21: recursive node tree traversal for world transforms
    std::function<void(const cgltf_node*, XMMATRIX)> TraverseNode;
    TraverseNode = [&](const cgltf_node* node, XMMATRIX parentWorld)
    {
        float localMat[16];
        cgltf_node_transform_local(node, localMat);
        XMMATRIX local = XMMatrixTranspose(XMMATRIX(localMat));
        XMMATRIX world = XMMatrixMultiply(local, parentWorld);

        if (node->mesh)
        {
            XMFLOAT4X4 worldF;
            XMStoreFloat4x4(&worldF, world);
            for (cgltf_size pi = 0; pi < node->mesh->primitives_count; ++pi)
                ProcessPrimitive(node->mesh, pi, worldF);
        }

        for (cgltf_size ci = 0; ci < node->children_count; ++ci)
            TraverseNode(node->children[ci], world);
    };

    XMMATRIX identity = XMMatrixIdentity();
    if (data->scenes_count > 0) {
        const cgltf_scene& scene = data->scene ? *data->scene : data->scenes[0];
        for (cgltf_size ni = 0; ni < scene.nodes_count; ++ni)
            TraverseNode(scene.nodes[ni], identity);
    } else {
        for (cgltf_size ni = 0; ni < data->nodes_count; ++ni)
            if (data->nodes[ni].parent == nullptr)
                TraverseNode(&data->nodes[ni], identity);
    }

    cgltf_free(data);

    // Phase 15B: assign bindless indices to materials and build merged geometry
    {
        uint32_t bindIdx = 0;
        std::set<VkMaterial*> seen;
        for (auto& sm : _vkSceneMeshes) {
            if (sm->material && seen.find(sm->material.get()) == seen.end()) {
                sm->material->bindlessIndex = bindIdx++;
                seen.insert(sm->material.get());
            }
        }
    }
    BuildMergedGeometry(allVerts, allIdxs);
    if (CreateIndirectResources())
    {
        LUNA_LOG_INFO("VK: Phase 15B GPU-driven ready (%zu meshes)", _vkSceneMeshes.size());
        { VulkanHiZ::CreateInfo hiZInfo{}; hiZInfo.core = &_core; hiZInfo.extent = _vkSwapchain.GetExtent(); hiZInfo.depthView = _vkSwapchain.GetDepthView();
          if (_hiZ.Create(hiZInfo)) {
              // Update cull descriptor sets with Hi-Z bindings (4=UBO, 5=texture)
              for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
                  if (_vkCullDescSet[i] == VK_NULL_HANDLE) continue;
                  VkDescriptorBufferInfo hizBI { _hiZ.GetParamsBuffer(), 0, 128 };
                  VkDescriptorImageInfo  hizII { _hiZ.GetSampler(), _hiZ.GetFullView(), VK_IMAGE_LAYOUT_GENERAL };
                  VkWriteDescriptorSet ws[2]{};
                  ws[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _vkCullDescSet[i], 4, 0, 1,
                            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &hizBI, nullptr };
                  ws[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _vkCullDescSet[i], 5, 0, 1,
                            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &hizII, nullptr, nullptr };
                  vkUpdateDescriptorSets(_device->GetDevice(), 2, ws, 0, nullptr);
              }
          } else {
              LUNA_LOG_WARN("VK Phase 23: Hi-Z occlusion culling init failed ??frustum-only culling");
          }
        }
        if (!CreateAsyncComputeResources())
            LUNA_LOG_WARN("Phase 20: Async compute init failed — cull on graphics queue");
        // Phase 27: mesh shader pipeline (requires objectData + meshlet buffers)
        if (!CreateMeshShaderResources())
            LUNA_LOG_INFO("VK Mesh Shader: init skipped — using indirect draw path");
    }
    else
        LUNA_LOG_WARN("VK: CreateIndirectResources FAILED");

    // Phase 18D: build acceleration structures + RT pipeline (requires merged geometry)
    if (_rtSupported) {
        if (BuildAccelerationStructures() && CreateRTPipeline())
            LUNA_LOG_INFO("VK RT: ray tracing initialized (%zu BLAS(es))", _vkBLASes.size());
        else
            LUNA_LOG_WARN("VK RT: initialization failed — falling back to CSM-only shadows");
    }

    LUNA_LOG_INFO("VK: Loaded %zu mesh(es)", _vkSceneMeshes.size());
    return dummy;
}

// ===========================================================================
// LoadDebugQuad ??procedural 2×2m flat plane for rendering pipeline debug
// ===========================================================================
std::vector<std::shared_ptr<Mesh>> VulkanBackend::LoadDebugQuad()
{
    VkDevice dev = _device->GetDevice();
    vkDeviceWaitIdle(dev);
    _vkSceneMeshes.clear();

    if (_linearSampler == VK_NULL_HANDLE) {
        VkSamplerCreateInfo si{};
        si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter    = VK_FILTER_LINEAR;
        si.minFilter    = VK_FILTER_LINEAR;
        si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        si.anisotropyEnable = VK_TRUE;
        si.maxAnisotropy    = 8.0f;
        si.maxLod           = 0.0f;
        vkCreateSampler(dev, &si, nullptr, &_linearSampler);
    }

    auto upload1x1 = [&](uint8_t r, uint8_t g, uint8_t b, uint8_t a) -> VkTexture {
        VkTexture t{};
        uint8_t px[4] = {r, g, b, a};
        VkBuffer stg; VkDeviceMemory stgMem;
        CreateBuffer(4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stg, stgMem);
        void* mp; vkMapMemory(dev, stgMem, 0, 4, 0, &mp); memcpy(mp, px, 4); vkUnmapMemory(dev, stgMem);
        CreateImage(1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, t.image, t.memory);
        TransitionImageLayout(t.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        CopyBufferToImage(stg, t.image, 1, 1);
        TransitionImageLayout(t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        vkDestroyBuffer(dev, stg, nullptr); vkFreeMemory(dev, stgMem, nullptr);
        t.view = CreateImageView(t.image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);
        return t;
    };

    auto mat = std::make_shared<VkMaterial>();
    mat->albedo     = upload1x1(255, 255, 255, 255);  // white
    mat->normalMap  = upload1x1(128, 128, 255, 255);  // flat tangent-space normal (0,0,1)
    mat->metalRough = upload1x1(  0, 128,   0, 255);  // G=128??.5 roughness, B=0 metallic (glTF)
    mat->albedoFactor[0] = mat->albedoFactor[1] = mat->albedoFactor[2] = mat->albedoFactor[3] = 1.0f;
    mat->metallicFactor  = 0.0f;
    mat->roughnessFactor = 1.0f;
    mat->bindlessIndex   = 0;

    // 2×2m quad in XZ plane (Y=0), normal=+Y, tangent=+X
    // Clockwise winding from above (+Y view) matches VK_FRONT_FACE_CLOCKWISE pipeline
    std::vector<PBRVertex> quadV = {
        {{-1, 0, -1}, {0,1,0}, {0,0}, {1,0,0, 1}},
        {{ 1, 0, -1}, {0,1,0}, {1,0}, {1,0,0, 1}},
        {{ 1, 0,  1}, {0,1,0}, {1,1}, {1,0,0, 1}},
        {{-1, 0,  1}, {0,1,0}, {0,1}, {1,0,0, 1}},
    };
    std::vector<uint32_t> quadI = {0, 1, 2,  0, 2, 3};

    auto sm = std::make_shared<VkSceneMesh>();
    sm->indexCount     = 6;
    sm->material       = mat;
    sm->boundingSphere = {0, 0, 0, 1.415f};

    {
        VkDeviceSize vbs = quadV.size() * sizeof(PBRVertex);
        VkBuffer vs; VkDeviceMemory vm;
        CreateBuffer(vbs, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, vs, vm);
        void* vp; vkMapMemory(dev, vm, 0, vbs, 0, &vp); memcpy(vp, quadV.data(), (size_t)vbs); vkUnmapMemory(dev, vm);
        CreateBuffer(vbs, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, sm->vertexBuffer, sm->vertexMemory);
        CopyBuffer(vs, sm->vertexBuffer, vbs);
        vkDestroyBuffer(dev, vs, nullptr); vkFreeMemory(dev, vm, nullptr);
    }
    {
        VkDeviceSize ibs = quadI.size() * sizeof(uint32_t);
        VkBuffer is2; VkDeviceMemory im;
        CreateBuffer(ibs, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, is2, im);
        void* ip; vkMapMemory(dev, im, 0, ibs, 0, &ip); memcpy(ip, quadI.data(), (size_t)ibs); vkUnmapMemory(dev, im);
        CreateBuffer(ibs, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, sm->indexBuffer, sm->indexMemory);
        CopyBuffer(is2, sm->indexBuffer, ibs);
        vkDestroyBuffer(dev, is2, nullptr); vkFreeMemory(dev, im, nullptr);
    }
    _vkSceneMeshes.push_back(sm);

    std::vector<std::vector<PBRVertex>> allVerts = {quadV};
    std::vector<std::vector<uint32_t>>  allIdxs  = {quadI};
    BuildMergedGeometry(allVerts, allIdxs);
    if (CreateIndirectResources())
    {
        LUNA_LOG_INFO("VK: Debug quad GPU-driven ready");
        CreateMeshShaderResources();  // Phase 27: try mesh shader for debug quad too
    }

    if (_rtSupported) {
        if (BuildAccelerationStructures() && CreateRTPipeline())
            LUNA_LOG_INFO("VK RT: debug quad RT initialized");
        else
            LUNA_LOG_WARN("VK RT: debug quad RT setup failed");
    }

    auto d = std::make_shared<Mesh>(); d->indexCount = sm->indexCount;
    return {d};
}

// ===========================================================================
// Calibration Scene (Vulkan) — delegates to LoadDebugQuad
// ===========================================================================
std::vector<std::shared_ptr<Mesh>> VulkanBackend::LoadCalibrationScene()
{
    return LoadDebugQuad();
}

// ===========================================================================
// ImGui
// ===========================================================================
void VulkanBackend::InitImGui(void* windowHandler)
{
    // Ensure no stale ImGui context/backend exists before initializing
    if (ImGui::GetCurrentContext() != nullptr)
    {
        // If a context already exists, check if platform backend was initialized
        ImGuiIO& io = ImGui::GetIO();
        if (io.BackendPlatformUserData != nullptr)
        {
            ImGui_ImplGlfw_Shutdown();
        }
        if (io.BackendRendererUserData != nullptr)
        {
            ImGui_ImplVulkan_Shutdown();
        }
        ImGui::DestroyContext();
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForVulkan(static_cast<GLFWwindow*>(windowHandler), true);

    ImGui_ImplVulkan_InitInfo ii{};
    ii.ApiVersion      = VK_API_VERSION_1_2;
    ii.Instance        = _instance;
    ii.PhysicalDevice  = _device->GetPhysicalDevice();
    ii.Device          = _device->GetDevice();
    ii.Queue           = _device->GetGraphicsQueue();
    ii.QueueFamily     = _device->GetGraphicsQueueFamily();
    ii.DescriptorPool  = _imguiDescriptorPool;
    ii.MinImageCount   = 2;
    ii.ImageCount      = (uint32_t)_vkSwapchain.GetImageCount();
    ii.CheckVkResultFn = [](VkResult e){ if(e!=VK_SUCCESS) LUNA_LOG_ERROR("ImGui Vk: %d",(int)e); };
    ii.PipelineInfoMain.RenderPass   = _vkSwapchain.GetRenderPass();
    ii.PipelineInfoMain.Subpass      = 0;
    ii.PipelineInfoMain.MSAASamples  = VK_SAMPLE_COUNT_1_BIT;
    ImGui_ImplVulkan_Init(&ii);
}

void VulkanBackend::StartImGui() {
    ImGui_ImplVulkan_NewFrame(); ImGui_ImplGlfw_NewFrame(); ImGui::NewFrame();
}
void VulkanBackend::RenderImGui() {
    ImGui::Render();
    if (!_frameActive) return;
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), _frames[_frameIndex].cmdBuffer);
}
void VulkanBackend::ShutdownImGui() {
    if (ImGui::GetCurrentContext() == nullptr) return;

    // Reset all command pools BEFORE ImGui destroys its resources.
    // Application::Shutdown() calls ShutdownImGui() before Backend::Shutdown(),
    // so frame command buffers still hold references to ImGui's internal
    // vertex/index buffers and pipeline from ImGui_ImplVulkan_RenderDrawData().
    // Resetting the pools clears those references, preventing validation errors.
    if (_device && _device->GetDevice() != VK_NULL_HANDLE)
    {
        VkDevice dev = _device->GetDevice();
        vkDeviceWaitIdle(dev);
        for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
            if (_frames[i].cmdPool)
                vkResetCommandPool(dev, _frames[i].cmdPool, 0);
            if (_computeFrames[i].cmdPool)
                vkResetCommandPool(dev, _computeFrames[i].cmdPool, 0);
        }
        if (_transferCmdPool)
            vkResetCommandPool(dev, _transferCmdPool, 0);
    }

    ImGuiIO& io = ImGui::GetIO();
    if (io.BackendRendererUserData != nullptr)
        ImGui_ImplVulkan_Shutdown();
    if (io.BackendPlatformUserData != nullptr)
        ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

void VulkanBackend::Resize(uint32_t w, uint32_t h) {
    if (!w || !h) return;
    // Defer resize to next BeginFrame ??avoids destroying framebuffers
    // while a command buffer is in recording state.
    _pendingResizeW = w;
    _pendingResizeH = h;
    _pendingResize  = true;
}

void VulkanBackend::Draw(uint32_t) {}
void VulkanBackend::SetVertexBuffer(class IBuffer*) {}
void VulkanBackend::BindPipeline(class IPipeline*) {}

// ===========================================================================
// Buffer / Image helpers
// ===========================================================================
uint32_t VulkanBackend::FindMemoryType(uint32_t filter, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(_device->GetPhysicalDevice(), &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((filter & (1u<<i)) && (mp.memoryTypes[i].propertyFlags & props) == props)
            return i;
    return UINT32_MAX;
}

bool VulkanBackend::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                  VkMemoryPropertyFlags props,
                                  VkBuffer& ob, VkDeviceMemory& om) {
    VkDevice dev = _device->GetDevice();
    VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size        = size;
    bi.usage       = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(dev, &bi, nullptr, &ob) != VK_SUCCESS) return false;

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, ob, &req);

    // Phase 18D: when buffer needs a device address, the allocation must include the flag
    VkMemoryAllocateFlagsInfo mafi{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO };
    mafi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

    VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize  = req.size;
    ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, props);
    if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) ai.pNext = &mafi;
    if (vkAllocateMemory(dev, &ai, nullptr, &om) != VK_SUCCESS) return false;
    vkBindBufferMemory(dev, ob, om, 0);
    return true;
}

bool VulkanBackend::CopyBuffer(VkBuffer src, VkBuffer dst, VkDeviceSize size) {
    VkCommandBuffer cmd = BeginSingleTimeCommands();
    VkBufferCopy c{ 0, 0, size };
    vkCmdCopyBuffer(cmd, src, dst, 1, &c);
    EndSingleTimeCommands(cmd);
    return true;
}

bool VulkanBackend::CreateImage(uint32_t w, uint32_t h, VkFormat fmt,
                                 VkImageTiling tiling, VkImageUsageFlags usage,
                                 VkMemoryPropertyFlags props,
                                 VkImage& oi, VkDeviceMemory& om) {
    VkDevice dev = _device->GetDevice();
    VkImageCreateInfo ii{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType   = VK_IMAGE_TYPE_2D;
    ii.format      = fmt;
    ii.extent      = { w, h, 1 };
    ii.mipLevels   = 1;
    ii.arrayLayers = 1;
    ii.samples     = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling      = tiling;
    ii.usage       = usage;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(dev, &ii, nullptr, &oi) != VK_SUCCESS) return false;

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, oi, &req);
    VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize  = req.size;
    ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, props);
    if (vkAllocateMemory(dev, &ai, nullptr, &om) != VK_SUCCESS) return false;
    vkBindImageMemory(dev, oi, om, 0);
    return true;
}

VkImageView VulkanBackend::CreateImageView(VkImage img, VkFormat fmt, VkImageAspectFlags aspect) {
    VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image            = img;
    vi.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    vi.format           = fmt;
    vi.subresourceRange = { aspect, 0, 1, 0, 1 };
    VkImageView v = VK_NULL_HANDLE;
    vkCreateImageView(_device->GetDevice(), &vi, nullptr, &v);
    return v;
}

bool VulkanBackend::TransitionImageLayout(VkImage img, VkImageLayout oldL, VkImageLayout newL) {
    VkCommandBuffer cmd = BeginSingleTimeCommands();
    VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout           = oldL;
    b.newLayout           = newL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image               = img;
    b.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkPipelineStageFlags ss, ds;
    if (oldL == VK_IMAGE_LAYOUT_UNDEFINED) {
        b.srcAccessMask = 0;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        ss = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        ds = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else {
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        ss = VK_PIPELINE_STAGE_TRANSFER_BIT;
        ds = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }
    vkCmdPipelineBarrier(cmd, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
    EndSingleTimeCommands(cmd);
    return true;
}

bool VulkanBackend::CopyBufferToImage(VkBuffer buf, VkImage img, uint32_t w, uint32_t h) {
    VkCommandBuffer cmd = BeginSingleTimeCommands();
    VkBufferImageCopy r{};
    r.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    r.imageExtent      = { w, h, 1 };
    vkCmdCopyBufferToImage(cmd, buf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    EndSingleTimeCommands(cmd);
    return true;
}

VkCommandBuffer VulkanBackend::BeginSingleTimeCommands() {
    // Early out if device already lost - prevents cascading errors
    if (_deviceLost) return VK_NULL_HANDLE;

    VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool        = _transferCmdPool;  // Use dedicated pool (avoids frame pool race)
    ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkResult r = vkAllocateCommandBuffers(_device->GetDevice(), &ai, &cmd);
    if (r == VK_ERROR_DEVICE_LOST) {
        LUNA_LOG_ERROR("VK: Device lost during command buffer allocation");
        _deviceLost = true;
        return VK_NULL_HANDLE;
    }
    if (r != VK_SUCCESS) {
        LUNA_LOG_ERROR("VK: Failed to allocate command buffer: %d", (int)r);
        return VK_NULL_HANDLE;
    }
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    return cmd;
}

void VulkanBackend::EndSingleTimeCommands(VkCommandBuffer cmd) {
    // Handle null from failed BeginSingleTimeCommands
    if (cmd == VK_NULL_HANDLE || _deviceLost) return;

    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &cmd;
    VkResult subRes = vkQueueSubmit(_device->GetGraphicsQueue(), 1, &si, VK_NULL_HANDLE);
    if (subRes == VK_ERROR_DEVICE_LOST) {
        LUNA_LOG_ERROR("VK: Device lost during queue submit (single-time cmd)");
        _deviceLost = true;
    }
    VkResult waitRes = vkQueueWaitIdle(_device->GetGraphicsQueue());
    if (waitRes == VK_ERROR_DEVICE_LOST) {
        LUNA_LOG_ERROR("VK: Device lost during queue wait (single-time cmd)");
        _deviceLost = true;
    }
    vkFreeCommandBuffers(_device->GetDevice(), _transferCmdPool, 1, &cmd);
}

// ===========================================================================
// Instance / Debug / Surface / ImGui pool
// ===========================================================================
bool VulkanBackend::CreateInstance() {
    uint32_t c = 0;
    const char** e = glfwGetRequiredInstanceExtensions(&c);
    if (!e) { LUNA_LOG_ERROR("Vulkan not supported"); return false; }
    std::vector<const char*> exts(e, e+c);
#if defined(_DEBUG)
    exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    const char* vl = "VK_LAYER_KHRONOS_validation";
#endif
    VkApplicationInfo ai{};
    ai.sType      = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.pEngineName = "Luna";
    ai.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo ci{};
    ci.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo        = &ai;
    ci.enabledExtensionCount   = (uint32_t)exts.size();
    ci.ppEnabledExtensionNames = exts.data();
#if defined(_DEBUG)
    ci.enabledLayerCount   = 1;
    ci.ppEnabledLayerNames = &vl;
#endif
    if (vkCreateInstance(&ci, nullptr, &_instance) != VK_SUCCESS)
    { LUNA_LOG_ERROR("Failed Vulkan instance"); return false; }
    LUNA_LOG_INFO("Vulkan instance created (API 1.3)");
    return true;
}

bool VulkanBackend::SetupDebugMessenger() {
#if defined(_DEBUG)
    VkDebugUtilsMessengerCreateInfoEXT ci{};
    ci.sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    ci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                       | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    ci.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                       | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    ci.pfnUserCallback = [](VkDebugUtilsMessageSeverityFlagBitsEXT sev,
        VkDebugUtilsMessageTypeFlagsEXT,
        const VkDebugUtilsMessengerCallbackDataEXT* d, void*) -> VkBool32 {
        if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
            LUNA_LOG_ERROR("[VkVal] %s", d->pMessage);
        return VK_FALSE;
    };
    auto fn = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(_instance, "vkCreateDebugUtilsMessengerEXT"));
    if (fn) fn(_instance, &ci, nullptr, &_debugMessenger);
    LUNA_LOG_INFO("Vulkan debug messenger active");
#endif
    return true;
}

bool VulkanBackend::CreateSurface(void* wh) {
    return glfwCreateWindowSurface(_instance,
        static_cast<GLFWwindow*>(wh), nullptr, &_surface) == VK_SUCCESS;
}

bool VulkanBackend::CreateImGuiDescriptorPool() {
    VkDescriptorPoolSize sz[] = {
        { VK_DESCRIPTOR_TYPE_SAMPLER,                1000 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,          1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         1000 },
    };
    VkDescriptorPoolCreateInfo pi{};
    pi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pi.poolSizeCount = (uint32_t)std::size(sz);
    pi.pPoolSizes    = sz;
    pi.maxSets       = 1000;
    return vkCreateDescriptorPool(_device->GetDevice(), &pi, nullptr, &_imguiDescriptorPool) == VK_SUCCESS;
}

void VulkanBackend::RecreateSwapchain() {
    LUNA_LOG_INFO("VK: RecreateSwapchain %ux%u", _width, _height);

    // Extra safety: ensure GPU is fully idle before touching any resources
    VkResult idleRes = vkDeviceWaitIdle(_device->GetDevice());
    if (idleRes == VK_ERROR_DEVICE_LOST) {
        LUNA_LOG_ERROR("VK: Device lost during RecreateSwapchain waitIdle");
        _deviceLost = true;
        return;
    }

    LUNA_LOG_INFO("VK: RecreateSwapchain ??destroying old resources...");
    _postProcess.Destroy();  // Destroy PP resources first (owns HDR/SSR/TAA/bloom images)
    _gBuffer.Destroy();
    // Destroy tonemap framebuffers (reference swapchain image views)
    { VkDevice tdev = _device->GetDevice(); for (auto fb : _tonemapFramebuffers) if (fb) vkDestroyFramebuffer(tdev, fb, nullptr); _tonemapFramebuffers.clear(); }
    _vkSwapchain.RequestResize(_width, _height); if (!_vkSwapchain.RecreateIfNeeded()) { _deviceLost = true; return; }
    _gBuffer.Resize(_vkSwapchain.GetExtent(), _vkSwapchain.GetDepthView()); CreateFramebuffers();

    // Resize SSAO subsystem 
    if (_ssao.IsReady()) {
        _ssao.Resize(_vkSwapchain.GetExtent(), _vkSwapchain.GetDepthView(), _gBuffer.GetNormalView());
    }

    // Recreate size-dependent SSAO resources if SSAO is active (legacy path)
    if (_ssaoPipeline && _ssaoDescPool) {
        VkDevice dev = _device->GetDevice();
        uint32_t halfW = std::max(1u, _vkSwapchain.GetExtent().width  / 2);
        uint32_t halfH = std::max(1u, _vkSwapchain.GetExtent().height / 2);

        // Destroy old RT images and framebuffers
        if (_ssaoFramebuffer)     { vkDestroyFramebuffer(dev, _ssaoFramebuffer, nullptr);     _ssaoFramebuffer     = VK_NULL_HANDLE; }
        if (_ssaoBlurFramebuffer) { vkDestroyFramebuffer(dev, _ssaoBlurFramebuffer, nullptr); _ssaoBlurFramebuffer = VK_NULL_HANDLE; }
        auto dI = [&](VkImageView& v, VkImage& i, VkDeviceMemory& m) {
            if (v) { vkDestroyImageView(dev, v, nullptr); v = VK_NULL_HANDLE; }
            if (i) { vkDestroyImage(dev, i, nullptr);     i = VK_NULL_HANDLE; }
            if (m) { vkFreeMemory(dev, m, nullptr);       m = VK_NULL_HANDLE; }
        };
        dI(_ssaoRTView,   _ssaoRTImage,   _ssaoRTMemory);
        dI(_ssaoBlurView, _ssaoBlurImage, _ssaoBlurMemory);

        // Recreate RT images at new half-res
        auto mkRT = [&](VkImage& img, VkDeviceMemory& mem, VkImageView& view) {
            CreateImage(halfW, halfH, VK_FORMAT_R8_UNORM, VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, img, mem);
            view = CreateImageView(img, VK_FORMAT_R8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);
        };
        mkRT(_ssaoRTImage,   _ssaoRTMemory,   _ssaoRTView);
        mkRT(_ssaoBlurImage, _ssaoBlurMemory, _ssaoBlurView);
        TransitionImageLayout(_ssaoRTImage,   VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        TransitionImageLayout(_ssaoBlurImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        // Recreate framebuffers at new size
        auto mkFB = [&](VkImageView v) -> VkFramebuffer {
            VkFramebufferCreateInfo fi{}; fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fi.renderPass = _ssaoRenderPass; fi.attachmentCount = 1; fi.pAttachments = &v;
            fi.width = halfW; fi.height = halfH; fi.layers = 1;
            VkFramebuffer fb = VK_NULL_HANDLE;
            vkCreateFramebuffer(dev, &fi, nullptr, &fb); return fb;
        };
        _ssaoFramebuffer     = mkFB(_ssaoRTView);
        _ssaoBlurFramebuffer = mkFB(_ssaoBlurView);

        // Update _ssaoTexDescSet: depth + normal views changed after G-buffer recreation
        VkDescriptorImageInfo di { _pointClampSampler, _vkSwapchain.GetDepthView(),    VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo ni { _pointClampSampler, _gBuffer.GetNormalView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet tw[2]{};
        tw[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _ssaoTexDescSet, 0, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &di, nullptr, nullptr };
        tw[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _ssaoTexDescSet, 1, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &ni, nullptr, nullptr };
        vkUpdateDescriptorSets(dev, 2, tw, 0, nullptr);

        // Update _ssaoBlurDescSet: raw SSAO RT view changed
        VkDescriptorImageInfo ri { _pointClampSampler, _ssaoRTView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet bw = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _ssaoBlurDescSet, 0, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &ri, nullptr, nullptr };
        vkUpdateDescriptorSets(dev, 1, &bw, 0, nullptr);
    }

    // Update deferred G-buffer descriptors last (picks up new SSAO blur view)
    UpdateDeferredGbufDescriptors();

    // Phase 18D: update RT descriptor sets with new depth/normal views after G-buffer recreation
    // CRITICAL: Must update even if RT pipeline exists, to replace stale view references
    if (_rtSupported && _vkRTDescPool)
    {
        VkDevice dev = _device->GetDevice();

        // Validate views are non-null before proceeding
        if (!_vkSwapchain.GetDepthView() || !_gBuffer.GetNormalView())
        {
            LUNA_LOG_ERROR("VK RT resize: _vkSwapchain.GetDepthView()=%p _gBuffer.GetNormalView()=%p - RT will be disabled",
                           (void*)_vkSwapchain.GetDepthView(), (void*)_gBuffer.GetNormalView());
        }
        else
        {
            // Recreate shadow mask at new resolution
            if (_vkShadowMaskView) { vkDestroyImageView(dev, _vkShadowMaskView, nullptr); _vkShadowMaskView = VK_NULL_HANDLE; }
            if (_vkShadowMaskImage){ vkDestroyImage(dev, _vkShadowMaskImage, nullptr);    _vkShadowMaskImage = VK_NULL_HANDLE; }
            if (_vkShadowMaskMem)  { vkFreeMemory(dev, _vkShadowMaskMem, nullptr);        _vkShadowMaskMem = VK_NULL_HANDLE; }
            uint32_t W = _vkSwapchain.GetExtent().width, H = _vkSwapchain.GetExtent().height;
            CreateImage(W, H, VK_FORMAT_R8_UNORM, VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, _vkShadowMaskImage, _vkShadowMaskMem);
            _vkShadowMaskView = CreateImageView(_vkShadowMaskImage, VK_FORMAT_R8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);
            TransitionImageLayout(_vkShadowMaskImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

            for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++)
            {
                if (_vkRTDescSet[i] == VK_NULL_HANDLE) continue;
                VkDescriptorImageInfo shadowII{ VK_NULL_HANDLE,      _vkShadowMaskView, VK_IMAGE_LAYOUT_GENERAL };
                VkDescriptorImageInfo depthII { _pointClampSampler,  _vkSwapchain.GetDepthView(),        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
                VkDescriptorImageInfo normalII{ _pointClampSampler,  _gBuffer.GetNormalView(),     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                VkWriteDescriptorSet ws[3]{};
                ws[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _vkRTDescSet[i], 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          &shadowII, nullptr, nullptr };
                ws[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _vkRTDescSet[i], 2, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depthII,  nullptr, nullptr };
                ws[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _vkRTDescSet[i], 3, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &normalII, nullptr, nullptr };
                vkUpdateDescriptorSets(dev, 3, ws, 0, nullptr);
            }
            LUNA_LOG_INFO("VK RT: descriptors updated for new resolution %ux%u", W, H);
        }
    }

    // Recreate PostProcess subsystem at new resolution
    {
        VulkanPostProcess::CreateInfo ppInfo;
        ppInfo.core = &_core;
        ppInfo.extent = _vkSwapchain.GetExtent();
        ppInfo.swapchainFormat = _vkSwapchain.GetFormat();
        ppInfo.depthView = _vkSwapchain.GetDepthView();
        ppInfo.normalView = _gBuffer.GetNormalView();
        ppInfo.metalRoughView = _gBuffer.GetMetalRoughView();
        ppInfo.linearSampler = _linearSampler;
        ppInfo.pointClampSampler = _pointClampSampler;

        if (!_postProcess.Create(ppInfo)) {
            LUNA_LOG_WARN("VK: PostProcess recreation failed on resize");
        }
        _postProcess.SetTonemapFramebuffers(_tonemapFramebuffers);
    }

    // Phase 23: recreate Hi-Z pyramid at new resolution
    if (_gpuDrivenReady) {
        if (_hiZ.Resize(_vkSwapchain.GetExtent(), _vkSwapchain.GetDepthView())) {
            // Update cull descriptor sets with Hi-Z bindings (4=UBO, 5=texture)
            for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
                if (_vkCullDescSet[i] == VK_NULL_HANDLE) continue;
                VkDescriptorBufferInfo hizBI { _hiZ.GetParamsBuffer(), 0, 128 };
                VkDescriptorImageInfo  hizII { _hiZ.GetSampler(), _hiZ.GetFullView(), VK_IMAGE_LAYOUT_GENERAL };
                VkWriteDescriptorSet ws[2]{};
                ws[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _vkCullDescSet[i], 4, 0, 1,
                          VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &hizBI, nullptr };
                ws[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _vkCullDescSet[i], 5, 0, 1,
                          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &hizII, nullptr, nullptr };
                vkUpdateDescriptorSets(_device->GetDevice(), 2, ws, 0, nullptr);
            }
        } else {
            LUNA_LOG_WARN("VK Phase 23: Hi-Z recreation failed on resize ??frustum-only culling");
        }
    }

    LUNA_LOG_INFO("VK: RecreateSwapchain complete (%ux%u)", _vkSwapchain.GetExtent().width, _vkSwapchain.GetExtent().height);
}

// ===========================================================================
// Render pass (color + depth)
// ===========================================================================
bool VulkanBackend::CreateRenderPass() {
    VkDevice dev = _device->GetDevice();

    // ?�?�?Helper: create a single-subpass render pass ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?
    auto mkRP = [&](VkFormat colorFmt, VkImageLayout colorFinal,
                    bool hasDepth, VkAttachmentLoadOp colorLoad) -> VkRenderPass
    {
        VkAttachmentDescription atts[2]{};
        atts[0].format         = colorFmt;
        atts[0].samples        = VK_SAMPLE_COUNT_1_BIT;
        atts[0].loadOp         = colorLoad;
        atts[0].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        atts[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[0].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        atts[0].finalLayout    = colorFinal;

        if (hasDepth) {
            atts[1].format         = VK_FORMAT_D32_SFLOAT;
            atts[1].samples        = VK_SAMPLE_COUNT_1_BIT;
            atts[1].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
            atts[1].storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            atts[1].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            atts[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            atts[1].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
            atts[1].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        }

        VkAttachmentReference cr{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkAttachmentReference dr{ 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };

        VkSubpassDescription sp{};
        sp.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sp.colorAttachmentCount    = 1;
        sp.pColorAttachments       = &cr;
        sp.pDepthStencilAttachment = hasDepth ? &dr : nullptr;

        VkSubpassDependency dep{};
        dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass    = 0;
        dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                          | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                          | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                          | (hasDepth ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : 0);

        VkRenderPassCreateInfo rpi{};
        rpi.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpi.attachmentCount = hasDepth ? 2 : 1;
        rpi.pAttachments    = atts;
        rpi.subpassCount    = 1;
        rpi.pSubpasses      = &sp;
        rpi.dependencyCount = 1;
        rpi.pDependencies   = &dep;
        VkRenderPass rp = VK_NULL_HANDLE;
        vkCreateRenderPass(dev, &rpi, nullptr, &rp);
        return rp;
    };

    // HDR forward render pass ??PBR geometry ??R16G16B16A16_SFLOAT (SHADER_READ_ONLY final)
    _hdrRenderPass = mkRP(VK_FORMAT_R16G16B16A16_SFLOAT,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          true, VK_ATTACHMENT_LOAD_OP_CLEAR);
    if (!_hdrRenderPass) return false;

    // PP render pass ??bloom/TAA targets (R16G16B16A16_SFLOAT, SHADER_READ_ONLY final, no depth)
    _ppRenderPass = mkRP(VK_FORMAT_R16G16B16A16_SFLOAT,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         false, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
    if (!_ppRenderPass) return false;

    // Tone map render pass ??writes to swapchain (PRESENT final, no depth, DONT_CARE load)
    _tonemapRenderPass = mkRP(_vkSwapchain.GetFormat(), VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                              false, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
    if (!_tonemapRenderPass) return false;

    // G-buffer render passes are now created by VulkanGBuffer subsystem.
    // _gBuffer.GetRenderPass() = CLEAR variant, _gBuffer.GetRenderPassLoad() = LOAD variant.
    return true;
}

// ===========================================================================
// Framebuffers
// ===========================================================================
bool VulkanBackend::CreateFramebuffers() {
    VkDevice dev = _device->GetDevice();
    UINT W = _vkSwapchain.GetExtent().width, H = _vkSwapchain.GetExtent().height;

    // Helper
    auto mkFB = [&](VkRenderPass rp, VkImageView color, VkImageView depth,
                    uint32_t w, uint32_t h) -> VkFramebuffer
    {
        VkImageView atts[2] = { color, depth };
        VkFramebufferCreateInfo fi{};
        fi.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fi.renderPass      = rp;
        fi.attachmentCount = depth ? 2u : 1u;
        fi.pAttachments    = atts;
        fi.width = w; fi.height = h; fi.layers = 1;
        VkFramebuffer fb = VK_NULL_HANDLE;
        vkCreateFramebuffer(dev, &fi, nullptr, &fb);
        return fb;
    };

    // HDR framebuffer ??forward PBR writes here (depth needed)
    if (_hdrView && _hdrRenderPass)
        _hdrFramebuffer = mkFB(_hdrRenderPass, _hdrView, _vkSwapchain.GetDepthView(), W, H);

    // G-buffer framebuffers now owned by VulkanGBuffer subsystem

    // Tone-map framebuffers ??one per swapchain image (no depth, PRESENT final)
    _tonemapFramebuffers.resize(_vkSwapchain.GetImageCount());
    for (size_t i = 0; i < _vkSwapchain.GetImageCount(); i++)
        _tonemapFramebuffers[i] = mkFB(_tonemapRenderPass,
                                       _vkSwapchain.GetImageView(i), VK_NULL_HANDLE, W, H);

    // ImGui/swapchain framebuffers ??used for the ImGui render pass (_vkSwapchain.GetRenderPass(), no depth)
    // ImGui/swapchain framebuffers now owned by VulkanSwapchain



    return true;
}

// ===========================================================================
// Per-frame resources
// ===========================================================================
bool VulkanBackend::CreateFrameResources() {
    VkDevice dev = _device->GetDevice();
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
        VkCommandPoolCreateInfo pci{};
        pci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.queueFamilyIndex = _device->GetGraphicsQueueFamily();
        pci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        if (vkCreateCommandPool(dev, &pci, nullptr, &_frames[i].cmdPool) != VK_SUCCESS) return false;

        VkCommandBufferAllocateInfo cbai{};
        cbai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool        = _frames[i].cmdPool;
        cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(dev, &cbai, &_frames[i].cmdBuffer) != VK_SUCCESS) return false;

        VkFenceCreateInfo fi{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT };
        if (vkCreateFence(dev, &fi, nullptr, &_frames[i].fence) != VK_SUCCESS) return false;

        VkSemaphoreCreateInfo si{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        if (vkCreateSemaphore(dev, &si, nullptr, &_frames[i].imageReady) != VK_SUCCESS) return false;
        if (vkCreateSemaphore(dev, &si, nullptr, &_frames[i].renderDone) != VK_SUCCESS) return false;

        // MVP UBO: MAX_DRAWS slots × 3 float4x4 = MAX_DRAWS × 192 bytes.
        // Each DrawMesh writes to slot[drawCallIndex] via dynamic descriptor offset,
        // so multiple meshes per frame get independent model matrices.
        // minUniformBufferOffsetAlignment is typically 256 B ??round each slot to 256 B.
        static constexpr VkDeviceSize MVP_SLOT_SIZE = 256;  // >= sizeof(MVPData)=192
        const VkDeviceSize mvpBufSize = VkFrameResource::MAX_DRAWS * MVP_SLOT_SIZE;
        if (!CreateBuffer(mvpBufSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                _frames[i].mvpBuffer, _frames[i].mvpMemory))
            return false;
        vkMapMemory(dev, _frames[i].mvpMemory, 0, mvpBufSize, 0, &_frames[i].mvpMapped);

        // Phase 5C: Scene UBO ??eyePosition(12) + pad(4) + lightDir(12) + lightIntensity(4)
        //                       + lightColor(12) + pad(4) = 48 bytes, allocated as 256B
        if (!CreateBuffer(256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                _frames[i].sceneBuffer, _frames[i].sceneMemory))
            return false;
        vkMapMemory(dev, _frames[i].sceneMemory, 0, 256, 0, &_frames[i].sceneMapped);
    }

    // Create dedicated command pool for single-time/transfer commands
    // This avoids race conditions with frame command pools during async resource loading
    VkCommandPoolCreateInfo tpi{};
    tpi.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    tpi.queueFamilyIndex = _device->GetGraphicsQueueFamily();
    tpi.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    if (vkCreateCommandPool(dev, &tpi, nullptr, &_transferCmdPool) != VK_SUCCESS) {
        LUNA_LOG_ERROR("VK: Failed to create transfer command pool");
        return false;
    }

    return true;
}

// ===========================================================================
// Pipeline
// ===========================================================================
bool VulkanBackend::CreateSceneDescriptorPool() {
    VkDescriptorPoolSize sz[] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         128 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 8   },  // MVP dynamic UBO (FRAMES_IN_FLIGHT)
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,          256 },
        { VK_DESCRIPTOR_TYPE_SAMPLER,                128 },
    };
    VkDescriptorPoolCreateInfo pi{};
    pi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.poolSizeCount = (uint32_t)std::size(sz);
    pi.pPoolSizes    = sz;
    pi.maxSets       = 256;
    return vkCreateDescriptorPool(_device->GetDevice(), &pi, nullptr, &_sceneDescPool) == VK_SUCCESS;
}

bool VulkanBackend::CreatePipeline() {
    VkDevice dev = _device->GetDevice();
    if (!CreateSceneDescriptorPool()) return false;

    // set=0: MVP UBO (binding=0, vertex+frag) + Scene UBO (binding=1, frag-only)
    // Phase 5C: both UBOs in the same descriptor set for efficient binding
    {
        VkDescriptorSetLayoutBinding bs[2]{};
        bs[0].binding         = 0;
        bs[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;  // dynamic offset per draw
        bs[0].descriptorCount = 1;
        bs[0].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        bs[1].binding         = 1;
        bs[1].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bs[1].descriptorCount = 1;
        bs[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo li{};
        li.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 2;
        li.pBindings    = bs;
        vkCreateDescriptorSetLayout(dev, &li, nullptr, &_mvpDescLayout);
    }
    // set=1: material UBO + 3 textures + sampler
    {
        VkDescriptorSetLayoutBinding bs[5]{};
        bs[0] = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        bs[1] = { 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        bs[2] = { 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        bs[3] = { 3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        bs[4] = { 4, VK_DESCRIPTOR_TYPE_SAMPLER,        1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        VkDescriptorSetLayoutCreateInfo li{};
        li.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 5;
        li.pBindings    = bs;
        vkCreateDescriptorSetLayout(dev, &li, nullptr, &_materialDescLayout);
    }

    // Allocate + write set=0 descriptor sets (MVP + Scene) for each frame
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool     = _sceneDescPool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts        = &_mvpDescLayout;
        vkAllocateDescriptorSets(dev, &dsai, &_frames[i].mvpDescSet);

        VkDescriptorBufferInfo mvpBI  { _frames[i].mvpBuffer,   0, 192 };  // range = one slot
        VkDescriptorBufferInfo sceneBI{ _frames[i].sceneBuffer, 0, 48  };

        VkWriteDescriptorSet wr[2]{};
        wr[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[0].dstSet          = _frames[i].mvpDescSet;
        wr[0].dstBinding      = 0;
        wr[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        wr[0].descriptorCount = 1;
        wr[0].pBufferInfo     = &mvpBI;
        wr[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[1].dstSet          = _frames[i].mvpDescSet;
        wr[1].dstBinding      = 1;
        wr[1].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        wr[1].descriptorCount = 1;
        wr[1].pBufferInfo     = &sceneBI;
        vkUpdateDescriptorSets(dev, 2, wr, 0, nullptr);
    }

    VkDescriptorSetLayout layouts[] = { _mvpDescLayout, _materialDescLayout };
    VkPipelineLayoutCreateInfo pli{};
    pli.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 2;
    pli.pSetLayouts    = layouts;
    vkCreatePipelineLayout(dev, &pli, nullptr, &_pipelineLayout);

    // Compile shaders
    std::vector<uint32_t> vsS, fsS;
    if (!CompileHLSLtoSPIRV(GetShaderFullPath(L"pbr_forward.vert.hlsl").wstring(), L"vs_6_0", vsS) ||
        !CompileHLSLtoSPIRV(GetShaderFullPath(L"pbr_forward.frag.hlsl").wstring(), L"ps_6_0", fsS))
    { LUNA_LOG_ERROR("VK: shader compile failed"); return false; }

    auto mkMod = [&](const std::vector<uint32_t>& sp) -> VkShaderModule {
        VkShaderModuleCreateInfo si{};
        si.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        si.codeSize = sp.size() * 4;
        si.pCode    = sp.data();
        VkShaderModule m = VK_NULL_HANDLE;
        vkCreateShaderModule(dev, &si, nullptr, &m);
        return m;
    };
    VkShaderModule vsM = mkMod(vsS), fsM = mkMod(fsS);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vsM;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fsM;
    stages[1].pName  = "main";

    // PBRVertex: pos(12)+normal(12)+uv(8)+tangent(16) = stride 48
    VkVertexInputBindingDescription vbd{ 0, 48, VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription vad[4]{};
    vad[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT,    0  };
    vad[1] = { 1, 0, VK_FORMAT_R32G32B32_SFLOAT,    12 };
    vad[2] = { 2, 0, VK_FORMAT_R32G32_SFLOAT,       24 };
    vad[3] = { 3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 32 };

    VkPipelineVertexInputStateCreateInfo vis{};
    vis.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vis.vertexBindingDescriptionCount   = 1;
    vis.pVertexBindingDescriptions      = &vbd;
    vis.vertexAttributeDescriptionCount = 4;
    vis.pVertexAttributeDescriptions    = vad;

    VkPipelineInputAssemblyStateCreateInfo ias{};
    ias.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ias.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vps{};
    vps.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1;
    vps.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_BACK_BIT;
    rs.frontFace   = VK_FRONT_FACE_CLOCKWISE;  // -fvk-invert-y flips winding
    rs.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable  = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp   = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                       | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo cbs{};
    cbs.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cbs.attachmentCount = 1;
    cbs.pAttachments    = &cba;

    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dsi{};
    dsi.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dsi.dynamicStateCount = 2;
    dsi.pDynamicStates    = dyn;

    VkGraphicsPipelineCreateInfo gpi{};
    gpi.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpi.stageCount          = 2;
    gpi.pStages             = stages;
    gpi.pVertexInputState   = &vis;
    gpi.pInputAssemblyState = &ias;
    gpi.pViewportState      = &vps;
    gpi.pRasterizationState = &rs;
    gpi.pMultisampleState   = &ms;
    gpi.pDepthStencilState  = &ds;
    gpi.pColorBlendState    = &cbs;
    gpi.pDynamicState       = &dsi;
    gpi.layout              = _pipelineLayout;
    gpi.renderPass          = _vkSwapchain.GetRenderPass();
    gpi.subpass             = 0;

    VkResult res = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, nullptr, &_graphicsPipeline);
    vkDestroyShaderModule(dev, vsM, nullptr);
    vkDestroyShaderModule(dev, fsM, nullptr);

    if (res != VK_SUCCESS)
    { LUNA_LOG_ERROR("VK pipeline failed: %d", (int)res); return false; }

    LUNA_LOG_INFO("VK: Forward PBR pipeline created");

    // ?�?�?G-buffer fill pipeline ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?
    // Compiles gbuffer_vk.frag.hlsl; reuses same vertex shader, same _pipelineLayout.
    // Targets _gBuffer.GetRenderPass() which has 3 colour attachments.
    {
        std::vector<uint32_t> gbFsS;
        if (!CompileGLSLtoSPIRV(GetShaderFullPath(L"gbuffer_vk.frag.glsl").wstring(), gbFsS))
        { LUNA_LOG_ERROR("VK: gbuffer_vk shader compile failed"); return false; }

        std::vector<uint32_t> gbVsS;
        if (!CompileHLSLtoSPIRV(GetShaderFullPath(L"pbr_forward.vert.hlsl").wstring(), L"vs_6_0", gbVsS))
        { LUNA_LOG_ERROR("VK: pbr_forward.vert compile failed (2nd)"); return false; }

        auto mkM = [&](const std::vector<uint32_t>& sp) {
            VkShaderModuleCreateInfo si{};
            si.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            si.codeSize = sp.size() * 4; si.pCode = sp.data();
            VkShaderModule m = VK_NULL_HANDLE;
            vkCreateShaderModule(dev, &si, nullptr, &m);
            return m;
        };
        VkShaderModule gbVsM = mkM(gbVsS), gbFsM = mkM(gbFsS);

        VkPipelineShaderStageCreateInfo gbStages[2]{};
        gbStages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,   gbVsM, "main" };
        gbStages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, gbFsM, "main" };

        // 3 colour blend attachments for the G-buffer render pass
        VkPipelineColorBlendAttachmentState gbCba{};
        gbCba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                             | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendAttachmentState gbCbas[3] = { gbCba, gbCba, gbCba };

        VkPipelineColorBlendStateCreateInfo gbCbs{};
        gbCbs.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        gbCbs.attachmentCount = 3;
        gbCbs.pAttachments    = gbCbas;

        VkGraphicsPipelineCreateInfo gbGpi = gpi;  // copy common state
        gbGpi.stageCount      = 2;
        gbGpi.pStages         = gbStages;
        gbGpi.pColorBlendState= &gbCbs;
        gbGpi.layout          = _pipelineLayout;
        gbGpi.renderPass      = _gBuffer.GetRenderPass();

        res = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gbGpi, nullptr, &_gbPipeline);
        vkDestroyShaderModule(dev, gbVsM, nullptr);
        vkDestroyShaderModule(dev, gbFsM, nullptr);
        if (res != VK_SUCCESS)
        { LUNA_LOG_ERROR("VK: G-buffer pipeline failed: %d", (int)res); return false; }

        LUNA_LOG_INFO("VK: G-buffer fill pipeline created");
    }

    return true;
}

// ===========================================================================
// G-buffer resources — now delegated to VulkanGBuffer subsystem
// ===========================================================================

void VulkanBackend::UpdateDeferredGbufDescriptors()
{
    if (_deferredGbufDescSet == VK_NULL_HANDLE) return;
    VkDevice dev = _device->GetDevice();

    VkDescriptorImageInfo aII  { _pointClampSampler, _gBuffer.GetAlbedoView(),     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo nII  { _pointClampSampler, _gBuffer.GetNormalView(),     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo mrII { _pointClampSampler, _gBuffer.GetMetalRoughView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo dII  { _pointClampSampler, _vkSwapchain.GetDepthView(),        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo sII  { _pointClampSampler, VK_NULL_HANDLE,    VK_IMAGE_LAYOUT_UNDEFINED };
    VkDescriptorImageInfo csmII{ VK_NULL_HANDLE,     _shadows.GetArrayView() ? _shadows.GetArrayView() : _gBuffer.GetAlbedoView(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo csmSII{ _shadows.GetSampler() ? _shadows.GetSampler() : _pointClampSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
    VkImageView ssaoView = _ssao.GetBlurredView();
    VkDescriptorImageInfo ssaoII{ VK_NULL_HANDLE, ssaoView ? ssaoView : _gBuffer.GetAlbedoView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo bilinII{ _pointClampSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };  // VulkanSSAO owns its bilinear sampler

    VkWriteDescriptorSet ws[14]{};
    ws[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _deferredGbufDescSet, 0, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &aII,    nullptr, nullptr };
    ws[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _deferredGbufDescSet, 1, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &nII,    nullptr, nullptr };
    ws[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _deferredGbufDescSet, 2, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &mrII,   nullptr, nullptr };
    ws[3] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _deferredGbufDescSet, 3, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &dII,    nullptr, nullptr };
    ws[4] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _deferredGbufDescSet, 4, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER,       &sII,    nullptr, nullptr };
    ws[5] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _deferredGbufDescSet, 5, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &csmII,  nullptr, nullptr };
    ws[6] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _deferredGbufDescSet, 6, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER,       &csmSII, nullptr, nullptr };
    ws[7] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _deferredGbufDescSet, 7, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &ssaoII, nullptr, nullptr };
    ws[8] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _deferredGbufDescSet, 8, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER,       &bilinII,nullptr, nullptr };
    uint32_t writeCount = 9;

    // Phase 15C: write IBL bindings when available
    VkDescriptorImageInfo envII{}, irrII{}, lutII{}, envSmpII{};
    if (_ibl.IsReady()) {
        envII    = { VK_NULL_HANDLE,  _ibl.GetPrefilterView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        irrII    = { VK_NULL_HANDLE,  _ibl.GetIrradianceView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        lutII    = { VK_NULL_HANDLE,  _ibl.GetBRDFLUTView(),   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        envSmpII = { _ibl.GetIBLSampler(), VK_NULL_HANDLE,     VK_IMAGE_LAYOUT_UNDEFINED };
        ws[9]  = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _deferredGbufDescSet,  9, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &envII,    nullptr, nullptr };
        ws[10] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _deferredGbufDescSet, 10, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &irrII,    nullptr, nullptr };
        ws[11] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _deferredGbufDescSet, 11, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &lutII,    nullptr, nullptr };
        ws[12] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _deferredGbufDescSet, 12, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER,       &envSmpII, nullptr, nullptr };
        writeCount = 13;
    }

    // Phase 18D: write RT shadow mask when available
    VkDescriptorImageInfo rtShadowII{};
    if (_vkShadowMaskView != VK_NULL_HANDLE) {
        rtShadowII = { VK_NULL_HANDLE, _vkShadowMaskView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        ws[writeCount] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _deferredGbufDescSet, 13, 0, 1,
                            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &rtShadowII, nullptr, nullptr };
        writeCount++;
    }

    vkUpdateDescriptorSets(dev, writeCount, ws, 0, nullptr);
}

// ===========================================================================
// Deferred lighting pipeline
// ===========================================================================
bool VulkanBackend::CreateDeferredPipeline()
{
    VkDevice dev = _device->GetDevice();

    // Point-clamp sampler for G-buffer reads
    if (_pointClampSampler == VK_NULL_HANDLE) {
        VkSamplerCreateInfo si{};
        si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter    = VK_FILTER_NEAREST;
        si.minFilter    = VK_FILTER_NEAREST;
        si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxAnisotropy = 1.0f;
        vkCreateSampler(dev, &si, nullptr, &_pointClampSampler);
    }

    // set=0: deferred scene UBO
    {
        VkDescriptorSetLayoutBinding b{};
        b.binding        = 0;
        b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b.descriptorCount= 1;
        b.stageFlags     = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo li{};
        li.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 1;
        li.pBindings    = &b;
        vkCreateDescriptorSetLayout(dev, &li, nullptr, &_deferredSceneLayout);
    }
    // set=1: 9 G-buffer/CSM/SSAO bindings + 4 IBL bindings + 1 RT shadow (PARTIALLY_BOUND until available)
    {
        VkDescriptorSetLayoutBinding bs[14]{};
        bs[0]  = { 0,  VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        bs[1]  = { 1,  VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        bs[2]  = { 2,  VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        bs[3]  = { 3,  VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        bs[4]  = { 4,  VK_DESCRIPTOR_TYPE_SAMPLER,       1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        bs[5]  = { 5,  VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        bs[6]  = { 6,  VK_DESCRIPTOR_TYPE_SAMPLER,       1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        bs[7]  = { 7,  VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        bs[8]  = { 8,  VK_DESCRIPTOR_TYPE_SAMPLER,       1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        // Phase 15C: IBL bindings (9-12) ??PARTIALLY_BOUND until LoadHDREnvironment()
        bs[9]  = { 9,  VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr }; // envCube
        bs[10] = { 10, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr }; // irrCube
        bs[11] = { 11, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr }; // brdfLUT
        bs[12] = { 12, VK_DESCRIPTOR_TYPE_SAMPLER,       1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr }; // envSampler
        // Phase 18D: RT shadow mask ??PARTIALLY_BOUND until RT pipeline is created
        bs[13] = { 13, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr }; // rtShadowTex

        VkDescriptorBindingFlags bindFlags[14] = {};
        bindFlags[9]  = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
        bindFlags[10] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
        bindFlags[11] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
        bindFlags[12] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
        bindFlags[13] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;

        VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{};
        flagsInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        flagsInfo.bindingCount = 14;
        flagsInfo.pBindingFlags= bindFlags;

        VkDescriptorSetLayoutCreateInfo li{};
        li.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.pNext        = &flagsInfo;
        li.bindingCount = 14;
        li.pBindings    = bs;
        vkCreateDescriptorSetLayout(dev, &li, nullptr, &_deferredGbufLayout);
    }

    VkDescriptorSetLayout dLayouts[] = { _deferredSceneLayout, _deferredGbufLayout,
                                         _clusterLightLayout ? _clusterLightLayout : _deferredSceneLayout };
    VkPipelineLayoutCreateInfo pli{};
    pli.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = _clusterLightLayout ? 3u : 2u;
    pli.pSetLayouts    = dLayouts;
    vkCreatePipelineLayout(dev, &pli, nullptr, &_deferredPipeLayout);

    // Descriptor pool: FRAMES_IN_FLIGHT scene sets + 1 G-buffer set (includes IBL + RT shadow slots)
    VkDescriptorPoolSize dsz[] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, FRAMES_IN_FLIGHT },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  11               },  // 4 G-buffer + CSM + SSAO + 3 IBL + rtShadow
        { VK_DESCRIPTOR_TYPE_SAMPLER,        4                },  // point + CSM + bilinear + envSampler
    };
    VkDescriptorPoolCreateInfo dpi{};
    dpi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.poolSizeCount = (uint32_t)std::size(dsz);
    dpi.pPoolSizes    = dsz;
    dpi.maxSets       = FRAMES_IN_FLIGHT + 1;
    vkCreateDescriptorPool(dev, &dpi, nullptr, &_deferredDescPool);

    // Per-frame scene UBOs + descriptor sets
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
        if (!CreateBuffer(512, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                _deferredSceneCB[i], _deferredSceneCBMem[i]))
            return false;
        vkMapMemory(dev, _deferredSceneCBMem[i], 0, 512, 0, &_deferredSceneCBMapped[i]);

        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool     = _deferredDescPool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts        = &_deferredSceneLayout;
        vkAllocateDescriptorSets(dev, &dsai, &_deferredSceneDescSet[i]);

        VkDescriptorBufferInfo bi{ _deferredSceneCB[i], 0, sizeof(DeferredSceneUBO) };
        VkWriteDescriptorSet wr{};
        wr.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr.dstSet          = _deferredSceneDescSet[i];
        wr.dstBinding      = 0;
        wr.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        wr.descriptorCount = 1;
        wr.pBufferInfo     = &bi;
        vkUpdateDescriptorSets(dev, 1, &wr, 0, nullptr);
    }

    // G-buffer descriptor set (static, updated on image creation/resize)
    {
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool     = _deferredDescPool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts        = &_deferredGbufLayout;
        vkAllocateDescriptorSets(dev, &dsai, &_deferredGbufDescSet);
    }
    UpdateDeferredGbufDescriptors();

    // Compile shaders
    std::vector<uint32_t> vsS, fsS;
    if (!CompileHLSLtoSPIRV(GetShaderFullPath(L"fullscreen.vert.hlsl").wstring(),            L"vs_6_0", vsS) ||
        !CompileGLSLtoSPIRV(GetShaderFullPath(L"deferred_lighting_vk.frag.glsl").wstring(),  fsS))
    { LUNA_LOG_ERROR("VK: deferred_lighting shader compile failed"); return false; }

    auto mkMod = [&](const std::vector<uint32_t>& sp) -> VkShaderModule {
        VkShaderModuleCreateInfo si{};
        si.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        si.codeSize = sp.size() * 4;
        si.pCode    = sp.data();
        VkShaderModule m = VK_NULL_HANDLE;
        vkCreateShaderModule(dev, &si, nullptr, &m);
        return m;
    };
    VkShaderModule vsM = mkMod(vsS), fsM = mkMod(fsS);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,   vsM, "main" };
    stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fsM, "main" };

    VkPipelineVertexInputStateCreateInfo vis{};
    vis.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo ias{};
    ias.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ias.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vps{};
    vps.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1;
    vps.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_NONE;
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType           = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                       | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo cbs{};
    cbs.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cbs.attachmentCount = 1;
    cbs.pAttachments    = &cba;

    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dsi{};
    dsi.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dsi.dynamicStateCount = 2;
    dsi.pDynamicStates    = dyn;

    VkGraphicsPipelineCreateInfo gpi{};
    gpi.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpi.stageCount          = 2;
    gpi.pStages             = stages;
    gpi.pVertexInputState   = &vis;
    gpi.pInputAssemblyState = &ias;
    gpi.pViewportState      = &vps;
    gpi.pRasterizationState = &rs;
    gpi.pMultisampleState   = &ms;
    gpi.pDepthStencilState  = &ds;
    gpi.pColorBlendState    = &cbs;
    gpi.pDynamicState       = &dsi;
    gpi.layout              = _deferredPipeLayout;
    gpi.renderPass          = _ppRenderPass;  // Phase 16C: writes to HDR intermediate RT
    gpi.subpass             = 0;

    VkResult r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, nullptr, &_deferredPipeline);
    vkDestroyShaderModule(dev, vsM, nullptr);
    vkDestroyShaderModule(dev, fsM, nullptr);

    if (r != VK_SUCCESS) { LUNA_LOG_ERROR("VK: deferred pipeline failed: %d", (int)r); return false; }

    LUNA_LOG_INFO("VK: Deferred lighting pipeline created");
    return true;
}


// ===========================================================================
// SSAO Resources
// ===========================================================================
static void GenerateSSAOKernel(XMFLOAT4* samples, int sampleCount)
{
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);
    for (int i = 0; i < sampleCount; ++i) {
        XMFLOAT3 s{ dist(rng)*2.f-1.f, dist(rng)*2.f-1.f, dist(rng) };
        XMVECTOR v = XMVector3Normalize(XMLoadFloat3(&s));
        float scale = float(i) / float(sampleCount);
        scale = 0.1f + scale * scale * 0.9f;
        v = XMVectorScale(v, scale * dist(rng));
        XMFLOAT3 r; XMStoreFloat3(&r, v);
        samples[i] = XMFLOAT4(r.x, r.y, r.z, 0.f);
    }
}

bool VulkanBackend::CreateSSAOResources()
{
    VkDevice dev = _device->GetDevice();
    uint32_t halfW = std::max(1u, _vkSwapchain.GetExtent().width  / 2);
    uint32_t halfH = std::max(1u, _vkSwapchain.GetExtent().height / 2);

    // -- Half-res R8_UNORM render targets (raw + blur) --
    auto mkRT = [&](VkImage& img, VkDeviceMemory& mem, VkImageView& view) -> bool {
        if (!CreateImage(halfW, halfH, VK_FORMAT_R8_UNORM, VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, img, mem))
            return false;
        view = CreateImageView(img, VK_FORMAT_R8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);
        return view != VK_NULL_HANDLE;
    };
    if (!mkRT(_ssaoRTImage, _ssaoRTMemory, _ssaoRTView)) return false;
    if (!mkRT(_ssaoBlurImage, _ssaoBlurMemory, _ssaoBlurView)) return false;

    // Transition both to SHADER_READ_ONLY initially
    TransitionImageLayout(_ssaoRTImage,   VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    TransitionImageLayout(_ssaoBlurImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // -- 4×4 R8G8_UNORM noise texture --
    {
        std::mt19937 rng(123);
        std::uniform_real_distribution<float> dist(-1.f, 1.f);
        struct { uint8_t r, g; } pixels[NOISE_SIZE * NOISE_SIZE];
        for (auto& p : pixels) {
            p.r = uint8_t((dist(rng) * 0.5f + 0.5f) * 255.f);
            p.g = uint8_t((dist(rng) * 0.5f + 0.5f) * 255.f);
        }
        if (!CreateImage(NOISE_SIZE, NOISE_SIZE, VK_FORMAT_R8G8_UNORM, VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, _ssaoNoiseImage, _ssaoNoiseMemory))
            return false;
        _ssaoNoiseView = CreateImageView(_ssaoNoiseImage, VK_FORMAT_R8G8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);

        // Upload via staging buffer
        VkDeviceSize sz = NOISE_SIZE * NOISE_SIZE * 2;
        VkBuffer staging; VkDeviceMemory stagingMem;
        CreateBuffer(sz, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            staging, stagingMem);
        void* data; vkMapMemory(dev, stagingMem, 0, sz, 0, &data);
        memcpy(data, pixels, sz);
        vkUnmapMemory(dev, stagingMem);
        TransitionImageLayout(_ssaoNoiseImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        CopyBufferToImage(staging, _ssaoNoiseImage, NOISE_SIZE, NOISE_SIZE);
        TransitionImageLayout(_ssaoNoiseImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        vkDestroyBuffer(dev, staging, nullptr);
        vkFreeMemory(dev, stagingMem, nullptr);
    }

    // -- Samplers --
    if (_pointClampSampler == VK_NULL_HANDLE) {
        VkSamplerCreateInfo si{};
        si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter    = si.minFilter = VK_FILTER_NEAREST;
        si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxAnisotropy = 1.0f;
        vkCreateSampler(dev, &si, nullptr, &_pointClampSampler);
    }
    if (!_ssaoPointWrap) {
        VkSamplerCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = si.minFilter = VK_FILTER_NEAREST;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        vkCreateSampler(dev, &si, nullptr, &_ssaoPointWrap);
    }
    if (!_ssaoBilinearClamp) {
        VkSamplerCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vkCreateSampler(dev, &si, nullptr, &_ssaoBilinearClamp);
    }

    // -- Render pass (R8_UNORM colour, no depth) --
    if (!_ssaoRenderPass) {
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
        vkCreateRenderPass(dev, &rpi, nullptr, &_ssaoRenderPass);
    }

    // Framebuffers
    auto mkFB = [&](VkImageView v) -> VkFramebuffer {
        VkFramebufferCreateInfo fi{}; fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fi.renderPass = _ssaoRenderPass; fi.attachmentCount = 1; fi.pAttachments = &v;
        fi.width = halfW; fi.height = halfH; fi.layers = 1;
        VkFramebuffer fb = VK_NULL_HANDLE;
        vkCreateFramebuffer(dev, &fi, nullptr, &fb); return fb;
    };
    _ssaoFramebuffer     = mkFB(_ssaoRTView);
    _ssaoBlurFramebuffer = mkFB(_ssaoBlurView);

    // -- Descriptor layouts --
    // SSAO set=0: UBO
    {
        VkDescriptorSetLayoutBinding b{ 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        VkDescriptorSetLayoutCreateInfo li{}; li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 1; li.pBindings = &b;
        vkCreateDescriptorSetLayout(dev, &li, nullptr, &_ssaoSceneLayout);
    }
    // SSAO set=1: depth + normal + noise + 2 samplers
    {
        VkDescriptorSetLayoutBinding bs[5]{};
        bs[0] = { 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        bs[1] = { 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        bs[2] = { 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        bs[3] = { 3, VK_DESCRIPTOR_TYPE_SAMPLER,       1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        bs[4] = { 4, VK_DESCRIPTOR_TYPE_SAMPLER,       1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        VkDescriptorSetLayoutCreateInfo li{}; li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 5; li.pBindings = bs;
        vkCreateDescriptorSetLayout(dev, &li, nullptr, &_ssaoTexLayout);
    }
    // Blur set=0: raw ssao + sampler
    {
        VkDescriptorSetLayoutBinding bs[2]{};
        bs[0] = { 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        bs[1] = { 1, VK_DESCRIPTOR_TYPE_SAMPLER,       1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        VkDescriptorSetLayoutCreateInfo li{}; li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 2; li.pBindings = bs;
        vkCreateDescriptorSetLayout(dev, &li, nullptr, &_ssaoBlurLayout);
    }

    // Pipeline layouts
    {
        VkDescriptorSetLayout sl[] = { _ssaoSceneLayout, _ssaoTexLayout };
        VkPipelineLayoutCreateInfo pli{}; pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = 2; pli.pSetLayouts = sl;
        vkCreatePipelineLayout(dev, &pli, nullptr, &_ssaoPipeLayout);
    }
    {
        VkPipelineLayoutCreateInfo pli{}; pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = 1; pli.pSetLayouts = &_ssaoBlurLayout;
        vkCreatePipelineLayout(dev, &pli, nullptr, &_ssaoBlurPipeLayout);
    }

    // Descriptor pool
    VkDescriptorPoolSize ps[] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, FRAMES_IN_FLIGHT },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  4 },  // depth+normal+noise + blur raw
        { VK_DESCRIPTOR_TYPE_SAMPLER,        3 },
    };
    VkDescriptorPoolCreateInfo dpi{}; dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.poolSizeCount = (uint32_t)std::size(ps); dpi.pPoolSizes = ps;
    dpi.maxSets = FRAMES_IN_FLIGHT + 2; // scene×FIF + tex + blur
    vkCreateDescriptorPool(dev, &dpi, nullptr, &_ssaoDescPool);

    // Allocate + write descriptor sets
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
        CreateBuffer(512, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            _ssaoCB[i], _ssaoCBMem[i]);
        vkMapMemory(dev, _ssaoCBMem[i], 0, 512, 0, &_ssaoCBMapped[i]);

        VkDescriptorSetAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = _ssaoDescPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &_ssaoSceneLayout;
        vkAllocateDescriptorSets(dev, &ai, &_ssaoSceneDescSet[i]);

        VkDescriptorBufferInfo bi{ _ssaoCB[i], 0, sizeof(SSAOConstants) };
        VkWriteDescriptorSet wr{}; wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr.dstSet = _ssaoSceneDescSet[i]; wr.dstBinding = 0;
        wr.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; wr.descriptorCount = 1; wr.pBufferInfo = &bi;
        vkUpdateDescriptorSets(dev, 1, &wr, 0, nullptr);
    }
    {
        VkDescriptorSetAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = _ssaoDescPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &_ssaoTexLayout;
        vkAllocateDescriptorSets(dev, &ai, &_ssaoTexDescSet);

        VkDescriptorImageInfo di { _pointClampSampler, _vkSwapchain.GetDepthView(),      VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo ni { _pointClampSampler, _gBuffer.GetNormalView(),   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo noi{ _ssaoPointWrap,     _ssaoNoiseView,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo s0 { _pointClampSampler, VK_NULL_HANDLE,  VK_IMAGE_LAYOUT_UNDEFINED };
        VkDescriptorImageInfo s1 { _ssaoPointWrap,     VK_NULL_HANDLE,  VK_IMAGE_LAYOUT_UNDEFINED };
        VkWriteDescriptorSet ws[5]{};
        ws[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _ssaoTexDescSet, 0, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &di,  nullptr, nullptr };
        ws[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _ssaoTexDescSet, 1, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &ni,  nullptr, nullptr };
        ws[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _ssaoTexDescSet, 2, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &noi, nullptr, nullptr };
        ws[3] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _ssaoTexDescSet, 3, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER,       &s0,  nullptr, nullptr };
        ws[4] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _ssaoTexDescSet, 4, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER,       &s1,  nullptr, nullptr };
        vkUpdateDescriptorSets(dev, 5, ws, 0, nullptr);
    }
    {
        VkDescriptorSetAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = _ssaoDescPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &_ssaoBlurLayout;
        vkAllocateDescriptorSets(dev, &ai, &_ssaoBlurDescSet);

        VkDescriptorImageInfo ri { _pointClampSampler, _ssaoRTView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo si { _pointClampSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
        VkWriteDescriptorSet ws[2]{};
        ws[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _ssaoBlurDescSet, 0, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &ri, nullptr, nullptr };
        ws[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _ssaoBlurDescSet, 1, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER,       &si, nullptr, nullptr };
        vkUpdateDescriptorSets(dev, 2, ws, 0, nullptr);
    }

    // -- Compile shaders & create pipelines --
    auto mkFullscreenPipeline = [&](const wchar_t* fsPath, VkPipelineLayout layout, VkRenderPass rp) -> VkPipeline {
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
        vkDestroyShaderModule(dev, vsM, nullptr); vkDestroyShaderModule(dev, fsM, nullptr);
        return pipe;
    };
    _ssaoPipeline     = mkFullscreenPipeline(L"ssao_vk.frag.glsl",      _ssaoPipeLayout,     _ssaoRenderPass);
    _ssaoBlurPipeline = mkFullscreenPipeline(L"ssao_blur_vk.frag.glsl", _ssaoBlurPipeLayout, _ssaoRenderPass);
    if (!_ssaoPipeline || !_ssaoBlurPipeline) { LUNA_LOG_ERROR("VK: SSAO pipeline creation failed"); return false; }

    // Generate kernel
    GenerateSSAOKernel(_ssaoKernel.samples, SSAO_SAMPLE_COUNT);
    _ssaoKernel.radius = 0.5f;
    _ssaoKernel.bias   = 0.025f;
    _ssaoKernel.noiseScale = XMFLOAT2(float(halfW) / 4.f, float(halfH) / 4.f);

    LUNA_LOG_INFO("VK: SSAO resources created (%ux%u, %d samples)", halfW, halfH, SSAO_SAMPLE_COUNT);
    return true;
}

void VulkanBackend::DestroySSAOResources()
{
    VkDevice dev = _device->GetDevice();
    auto dI = [&](VkImageView& v, VkImage& i, VkDeviceMemory& m) {
        if (v) { vkDestroyImageView(dev, v, nullptr); v = VK_NULL_HANDLE; }
        if (i) { vkDestroyImage(dev, i, nullptr); i = VK_NULL_HANDLE; }
        if (m) { vkFreeMemory(dev, m, nullptr); m = VK_NULL_HANDLE; }
    };
    dI(_ssaoRTView, _ssaoRTImage, _ssaoRTMemory);
    dI(_ssaoBlurView, _ssaoBlurImage, _ssaoBlurMemory);
    dI(_ssaoNoiseView, _ssaoNoiseImage, _ssaoNoiseMemory);
    if (_ssaoFramebuffer)     { vkDestroyFramebuffer(dev, _ssaoFramebuffer, nullptr);     _ssaoFramebuffer     = VK_NULL_HANDLE; }
    if (_ssaoBlurFramebuffer) { vkDestroyFramebuffer(dev, _ssaoBlurFramebuffer, nullptr); _ssaoBlurFramebuffer = VK_NULL_HANDLE; }
    if (_ssaoPipeline)        { vkDestroyPipeline(dev, _ssaoPipeline, nullptr);           _ssaoPipeline        = VK_NULL_HANDLE; }
    if (_ssaoBlurPipeline)    { vkDestroyPipeline(dev, _ssaoBlurPipeline, nullptr);       _ssaoBlurPipeline    = VK_NULL_HANDLE; }
    if (_ssaoPipeLayout)      { vkDestroyPipelineLayout(dev, _ssaoPipeLayout, nullptr);   _ssaoPipeLayout      = VK_NULL_HANDLE; }
    if (_ssaoBlurPipeLayout)  { vkDestroyPipelineLayout(dev, _ssaoBlurPipeLayout, nullptr); _ssaoBlurPipeLayout= VK_NULL_HANDLE; }
    if (_ssaoSceneLayout)     { vkDestroyDescriptorSetLayout(dev, _ssaoSceneLayout, nullptr); _ssaoSceneLayout = VK_NULL_HANDLE; }
    if (_ssaoTexLayout)       { vkDestroyDescriptorSetLayout(dev, _ssaoTexLayout, nullptr);   _ssaoTexLayout   = VK_NULL_HANDLE; }
    if (_ssaoBlurLayout)      { vkDestroyDescriptorSetLayout(dev, _ssaoBlurLayout, nullptr);  _ssaoBlurLayout  = VK_NULL_HANDLE; }
    if (_ssaoDescPool)        { vkDestroyDescriptorPool(dev, _ssaoDescPool, nullptr);         _ssaoDescPool    = VK_NULL_HANDLE; }
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
        if (_ssaoCB[i])    { vkDestroyBuffer(dev, _ssaoCB[i], nullptr);  _ssaoCB[i]    = VK_NULL_HANDLE; }
        if (_ssaoCBMem[i]) { vkFreeMemory(dev, _ssaoCBMem[i], nullptr); _ssaoCBMem[i] = VK_NULL_HANDLE; }
    }
    if (_ssaoPointWrap)     { vkDestroySampler(dev, _ssaoPointWrap, nullptr);     _ssaoPointWrap     = VK_NULL_HANDLE; }
    if (_ssaoBilinearClamp) { vkDestroySampler(dev, _ssaoBilinearClamp, nullptr); _ssaoBilinearClamp = VK_NULL_HANDLE; }
    if (_ssaoRenderPass)    { vkDestroyRenderPass(dev, _ssaoRenderPass, nullptr); _ssaoRenderPass    = VK_NULL_HANDLE; }
}

// ===========================================================================
// Phase 16C: PP Resources (HDR RT + SSR compute + tonemap)
// ===========================================================================
bool VulkanBackend::CreatePPResources()
{
    VkDevice dev = _device->GetDevice();
    uint32_t W = _vkSwapchain.GetExtent().width, H = _vkSwapchain.GetExtent().height;

    // ?�?�?1. HDR image (COLOR_ATTACHMENT | SAMPLED, R16G16B16A16_SFLOAT) ?�?�?
    if (!CreateImage(W, H, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, _hdrImage, _hdrMemory))
        return false;
    _hdrView = CreateImageView(_hdrImage, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!_hdrView) return false;

    // Create _deferredHDRFramebuffer targeting _ppRenderPass + _hdrView (no depth)
    {
        VkFramebufferCreateInfo fi{}; fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fi.renderPass = _ppRenderPass; fi.attachmentCount = 1; fi.pAttachments = &_hdrView;
        fi.width = W; fi.height = H; fi.layers = 1;
        if (vkCreateFramebuffer(dev, &fi, nullptr, &_deferredHDRFramebuffer) != VK_SUCCESS)
            return false;
    }

    // ?�?�?2. SSR image (STORAGE | SAMPLED, R16G16B16A16_SFLOAT, kept GENERAL) ?�?�?
    if (!CreateImage(W, H, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, _ssrImage, _ssrMemory))
        return false;
    _ssrView = CreateImageView(_ssrImage, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!_ssrView) return false;
    TransitionImageLayout(_ssrImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

    // ?�?�?3. Tonemap DSL: binding0=SAMPLED_IMAGE(hdr), binding1=SAMPLED_IMAGE(ssr), binding2=SAMPLER ?�?�?
    {
        VkDescriptorSetLayoutBinding bs[3]{};
        bs[0] = { 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        bs[1] = { 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        bs[2] = { 2, VK_DESCRIPTOR_TYPE_SAMPLER,       1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        VkDescriptorSetLayoutCreateInfo li{}; li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 3; li.pBindings = bs;
        if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &_vkSSRTonemapLayout) != VK_SUCCESS)
            return false;
    }
    {
        VkPipelineLayoutCreateInfo pli{}; pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = 1; pli.pSetLayouts = &_vkSSRTonemapLayout;
        if (vkCreatePipelineLayout(dev, &pli, nullptr, &_vkSSRTonemapPipeLayout) != VK_SUCCESS)
            return false;
    }

    // Descriptor pool for tonemap (1 set: 2 SAMPLED_IMAGE + 1 SAMPLER)
    {
        VkDescriptorPoolSize ps[] = {
            { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2 },
            { VK_DESCRIPTOR_TYPE_SAMPLER,       1 },
        };
        VkDescriptorPoolCreateInfo dpi{}; dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpi.poolSizeCount = (uint32_t)std::size(ps); dpi.pPoolSizes = ps;
        dpi.maxSets = 1;
        if (vkCreateDescriptorPool(dev, &dpi, nullptr, &_vkSSRTonemapDescPool) != VK_SUCCESS)
            return false;

        VkDescriptorSetAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = _vkSSRTonemapDescPool; ai.descriptorSetCount = 1;
        ai.pSetLayouts = &_vkSSRTonemapLayout;
        vkAllocateDescriptorSets(dev, &ai, &_vkSSRTonemapDescSet);
    }

    // Ensure _linearSampler exists (created in CreateSSAOResources usually, but guard here)
    if (_linearSampler == VK_NULL_HANDLE) {
        VkSamplerCreateInfo si{}; si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = si.minFilter = VK_FILTER_LINEAR; si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        si.anisotropyEnable = VK_TRUE; si.maxAnisotropy = 8.0f; si.maxLod = 0.0f;
        vkCreateSampler(dev, &si, nullptr, &_linearSampler);
    }

    // Write tonemap descriptors
    {
        VkDescriptorImageInfo hi{ VK_NULL_HANDLE, _hdrView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo si{ VK_NULL_HANDLE, _ssrView, VK_IMAGE_LAYOUT_GENERAL };
        VkDescriptorImageInfo sp{ _linearSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
        VkWriteDescriptorSet ws[3]{};
        ws[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _vkSSRTonemapDescSet, 0, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &hi, nullptr, nullptr };
        ws[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _vkSSRTonemapDescSet, 1, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &si, nullptr, nullptr };
        ws[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _vkSSRTonemapDescSet, 2, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER,       &sp, nullptr, nullptr };
        vkUpdateDescriptorSets(dev, 3, ws, 0, nullptr);
    }

    // Compile + create tonemap graphics pipeline
    {
        std::vector<uint32_t> vsS, fsS;
        if (!CompileHLSLtoSPIRV(GetShaderFullPath(L"fullscreen.vert.hlsl").wstring(),    L"vs_6_0", vsS) ||
            !CompileGLSLtoSPIRV(GetShaderFullPath(L"tonemapping_vk.frag.glsl").wstring(), fsS))
        { LUNA_LOG_ERROR("VK PP: tonemap shader compile failed"); return false; }

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
        gpi.layout = _vkSSRTonemapPipeLayout; gpi.renderPass = _tonemapRenderPass;
        VkResult r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, nullptr, &_vkSSRTonemapPipeline);
        vkDestroyShaderModule(dev, vsM, nullptr); vkDestroyShaderModule(dev, fsM, nullptr);
        if (r != VK_SUCCESS) { LUNA_LOG_ERROR("VK PP: tonemap pipeline failed: %d", (int)r); return false; }
    }

    // ?�?�?4. SSR compute pipeline ?�?�?
    // DSL: binding0=UBO, binding1-4=SAMPLED_IMAGE, binding5=STORAGE_IMAGE, binding6-7=SAMPLER
    {
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
        if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &_vkSSRLayout) != VK_SUCCESS)
            return false;
    }
    {
        VkPipelineLayoutCreateInfo pli{}; pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = 1; pli.pSetLayouts = &_vkSSRLayout;
        if (vkCreatePipelineLayout(dev, &pli, nullptr, &_vkSSRPipeLayout) != VK_SUCCESS)
            return false;
    }

    // Descriptor pool for SSR (FRAMES_IN_FLIGHT sets)
    {
        VkDescriptorPoolSize ps[] = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, FRAMES_IN_FLIGHT },
            { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  4 * FRAMES_IN_FLIGHT },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  FRAMES_IN_FLIGHT },
            { VK_DESCRIPTOR_TYPE_SAMPLER,        2 * FRAMES_IN_FLIGHT },
        };
        VkDescriptorPoolCreateInfo dpi{}; dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpi.poolSizeCount = (uint32_t)std::size(ps); dpi.pPoolSizes = ps;
        dpi.maxSets = FRAMES_IN_FLIGHT;
        if (vkCreateDescriptorPool(dev, &dpi, nullptr, &_vkSSRDescPool) != VK_SUCCESS)
            return false;
    }

    // Ensure _pointClampSampler exists
    if (_pointClampSampler == VK_NULL_HANDLE) {
        VkSamplerCreateInfo si{}; si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = si.minFilter = VK_FILTER_NEAREST; si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxAnisotropy = 1.0f;
        vkCreateSampler(dev, &si, nullptr, &_pointClampSampler);
    }

    // Per-frame UBOs + descriptor sets
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
        CreateBuffer(256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            _vkSSRCB[i], _vkSSRCBMem[i]);
        vkMapMemory(dev, _vkSSRCBMem[i], 0, 256, 0, &_vkSSRCBMapped[i]);

        VkDescriptorSetAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = _vkSSRDescPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &_vkSSRLayout;
        vkAllocateDescriptorSets(dev, &ai, &_vkSSRDescSet[i]);

        VkDescriptorBufferInfo ubi{ _vkSSRCB[i], 0, 256 };
        VkDescriptorImageInfo  di { VK_NULL_HANDLE, _vkSwapchain.GetDepthView(),          VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo  ni { VK_NULL_HANDLE, _gBuffer.GetNormalView(),       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo  mri{ VK_NULL_HANDLE, _gBuffer.GetMetalRoughView(),   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo  hi { VK_NULL_HANDLE, _hdrView,            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo  oi { VK_NULL_HANDLE, _ssrView,       VK_IMAGE_LAYOUT_GENERAL };
        VkDescriptorImageInfo  s0 { _pointClampSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
        VkDescriptorImageInfo  s1 { _linearSampler,     VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };

        VkWriteDescriptorSet ws[8]{};
        ws[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _vkSSRDescSet[i], 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &ubi, nullptr };
        ws[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _vkSSRDescSet[i], 1, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  &di,  nullptr, nullptr };
        ws[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _vkSSRDescSet[i], 2, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  &ni,  nullptr, nullptr };
        ws[3] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _vkSSRDescSet[i], 3, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  &mri, nullptr, nullptr };
        ws[4] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _vkSSRDescSet[i], 4, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  &hi,  nullptr, nullptr };
        ws[5] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _vkSSRDescSet[i], 5, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  &oi,  nullptr, nullptr };
        ws[6] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _vkSSRDescSet[i], 6, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER,        &s0,  nullptr, nullptr };
        ws[7] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _vkSSRDescSet[i], 7, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER,        &s1,  nullptr, nullptr };
        vkUpdateDescriptorSets(dev, 8, ws, 0, nullptr);
    }

    // Compile + create SSR compute pipeline
    {
        std::vector<uint32_t> csS;
        if (!CompileGLSLtoSPIRV(GetShaderFullPath(L"ssr_vk.comp.glsl").wstring(), csS))
        { LUNA_LOG_ERROR("VK PP: SSR compute shader compile failed ??SSR will be skipped"); }
        else {
            VkShaderModuleCreateInfo si{}; si.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            si.codeSize = csS.size()*4; si.pCode = csS.data();
            VkShaderModule csM = VK_NULL_HANDLE;
            vkCreateShaderModule(dev, &si, nullptr, &csM);

            VkPipelineShaderStageCreateInfo stage{};
            stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
            stage.module = csM;
            stage.pName  = "main";

            VkComputePipelineCreateInfo cpi{}; cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            cpi.stage  = stage;
            cpi.layout = _vkSSRPipeLayout;
            vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &_vkSSRPipeline);
            vkDestroyShaderModule(dev, csM, nullptr);
        }
    }

    // ?�?�?Phase 18B: Motion Blur target (full-res RGBA16F, COLOR_ATTACHMENT | SAMPLED) ?�?�?
    {
        if (CreateImage(W, H, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, _mbImage, _mbMemory))
        {
            _mbView = CreateImageView(_mbImage, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT);
            TransitionImageLayout(_mbImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            // Framebuffer uses _ppRenderPass (UNDEFINED ??SHADER_READ_ONLY)
            VkFramebufferCreateInfo fi{}; fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fi.renderPass = _ppRenderPass; fi.attachmentCount = 1; fi.pAttachments = &_mbView;
            fi.width = W; fi.height = H; fi.layers = 1;
            vkCreateFramebuffer(dev, &fi, nullptr, &_mbFB);
        }
        // MB descriptor set layout: UBO(0) + SAMPLED_IMAGE(1,2) + SAMPLER(3)
        VkDescriptorSetLayoutBinding bins[4]{};
        bins[0] = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,  1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        bins[1] = { 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,   1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        bins[2] = { 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,   1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        bins[3] = { 3, VK_DESCRIPTOR_TYPE_SAMPLER,         1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        VkDescriptorSetLayoutCreateInfo li{}; li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 4; li.pBindings = bins;
        vkCreateDescriptorSetLayout(dev, &li, nullptr, &_vkMBLayout);

        // Per-frame descriptor pool + sets + UBOs
        VkDescriptorPoolSize ps[4]{ {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,FRAMES_IN_FLIGHT},
                                    {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  FRAMES_IN_FLIGHT*2},
                                    {VK_DESCRIPTOR_TYPE_SAMPLER,        FRAMES_IN_FLIGHT} };
        VkDescriptorPoolCreateInfo pi{}; pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pi.maxSets = FRAMES_IN_FLIGHT; pi.poolSizeCount = 3; pi.pPoolSizes = ps;
        vkCreateDescriptorPool(dev, &pi, nullptr, &_vkMBDescPool);

        for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++)
        {
            // UBO
            CreateBuffer(256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                _vkMBCB[i], _vkMBCBMem[i]);
            vkMapMemory(dev, _vkMBCBMem[i], 0, VK_WHOLE_SIZE, 0, &_vkMBCBMapped[i]);
            // Descriptor set
            VkDescriptorSetAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            ai.descriptorPool = _vkMBDescPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &_vkMBLayout;
            vkAllocateDescriptorSets(dev, &ai, &_vkMBDescSet[i]);
            // Write descriptors
            VkDescriptorBufferInfo ubi{ _vkMBCB[i], 0, sizeof(VKMotionBlurConstants) };
            VkDescriptorImageInfo hdrI { VK_NULL_HANDLE, _hdrView,   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo depI { VK_NULL_HANDLE, _vkSwapchain.GetDepthView(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo samI { _pointClampSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
            VkWriteDescriptorSet ws[4]{};
            ws[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _vkMBDescSet[i], 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,  nullptr, &ubi,  nullptr };
            ws[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _vkMBDescSet[i], 1, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,   &hdrI,  nullptr, nullptr };
            ws[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _vkMBDescSet[i], 2, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,   &depI,  nullptr, nullptr };
            ws[3] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _vkMBDescSet[i], 3, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER,         &samI,  nullptr, nullptr };
            vkUpdateDescriptorSets(dev, 4, ws, 0, nullptr);
        }
        // Pipeline layout (single set, no push constants)
        VkPipelineLayoutCreateInfo pli{}; pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = 1; pli.pSetLayouts = &_vkMBLayout;
        vkCreatePipelineLayout(dev, &pli, nullptr, &_vkMBPipeLayout);
    }

    // ?�?�?Phase 17: TAA + Bloom + Full Tonemap ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?
    uint32_t halfW = std::max(1u, W / 2), halfH = std::max(1u, H / 2);

    // TAA history images (full-res × 2, COLOR_ATTACHMENT | SAMPLED)
    for (int i = 0; i < 2; i++) {
        if (!CreateImage(W, H, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, _taaHistoryImage[i], _taaHistoryMemory[i]))
            return false;
        _taaHistoryView[i] = CreateImageView(_taaHistoryImage[i], VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT);
        if (!_taaHistoryView[i]) return false;
        // Pre-transition to SHADER_READ_ONLY so TAA history reads in frame 0 don't hit UNDEFINED
        TransitionImageLayout(_taaHistoryImage[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        VkFramebufferCreateInfo fi{}; fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fi.renderPass = _ppRenderPass; fi.attachmentCount = 1; fi.pAttachments = &_taaHistoryView[i];
        fi.width = W; fi.height = H; fi.layers = 1;
        if (vkCreateFramebuffer(dev, &fi, nullptr, &_taaFramebuffer[i]) != VK_SUCCESS) return false;
    }

    // Bloom bright (half-res)
    if (!CreateImage(halfW, halfH, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, _bloomBrightImage, _bloomBrightMemory))
        return false;
    _bloomBrightView = CreateImageView(_bloomBrightImage, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT);
    {
        VkFramebufferCreateInfo fi{}; fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fi.renderPass = _ppRenderPass; fi.attachmentCount = 1; fi.pAttachments = &_bloomBrightView;
        fi.width = halfW; fi.height = halfH; fi.layers = 1;
        if (vkCreateFramebuffer(dev, &fi, nullptr, &_bloomBrightFramebuffer) != VK_SUCCESS) return false;
    }

    // Bloom blur (half-res)
    if (!CreateImage(halfW, halfH, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, _bloomBlurImage, _bloomBlurMemory))
        return false;
    _bloomBlurView = CreateImageView(_bloomBlurImage, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT);
    {
        VkFramebufferCreateInfo fi{}; fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fi.renderPass = _ppRenderPass; fi.attachmentCount = 1; fi.pAttachments = &_bloomBlurView;
        fi.width = halfW; fi.height = halfH; fi.layers = 1;
        if (vkCreateFramebuffer(dev, &fi, nullptr, &_bloomBlurFramebuffer) != VK_SUCCESS) return false;
    }

    // TAA DSL: b0=UBO, t0=currentFrame, t1=historyFrame, t2=depthTex, s0=bilinear, s1=point
    {
        VkDescriptorSetLayoutBinding bs[6]{};
        bs[0] = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        bs[1] = { 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        bs[2] = { 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        bs[3] = { 3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        bs[4] = { 4, VK_DESCRIPTOR_TYPE_SAMPLER,        1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        bs[5] = { 5, VK_DESCRIPTOR_TYPE_SAMPLER,        1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        VkDescriptorSetLayoutCreateInfo li{}; li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 6; li.pBindings = bs;
        if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &_vkTAALayout) != VK_SUCCESS) return false;
    }

    // PP1SRV DSL: 1 SAMPLED_IMAGE + 1 SAMPLER (shared by bloom bright/blur passes)
    {
        VkDescriptorSetLayoutBinding bs[2]{};
        bs[0] = { 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        bs[1] = { 1, VK_DESCRIPTOR_TYPE_SAMPLER,       1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        VkDescriptorSetLayoutCreateInfo li{}; li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 2; li.pBindings = bs;
        if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &_vkPP1SRVLayout) != VK_SUCCESS) return false;
    }

    // Full tonemap DSL: 3 SAMPLED_IMAGE + 1 SAMPLER (TAA + bloom + SSR ??swapchain)
    {
        VkDescriptorSetLayoutBinding bs[4]{};
        bs[0] = { 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        bs[1] = { 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        bs[2] = { 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        bs[3] = { 3, VK_DESCRIPTOR_TYPE_SAMPLER,       1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        VkDescriptorSetLayoutCreateInfo li{}; li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 4; li.pBindings = bs;
        if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &_vkPP2SRVLayout) != VK_SUCCESS) return false;
    }

    // Pipeline layouts
    {
        VkPipelineLayoutCreateInfo pli{}; pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = 1; pli.pSetLayouts = &_vkTAALayout;
        if (vkCreatePipelineLayout(dev, &pli, nullptr, &_vkTAAPipelineLayout) != VK_SUCCESS) return false;
    }
    {
        VkPushConstantRange pcr{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16 };
        VkPipelineLayoutCreateInfo pli{}; pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = 1; pli.pSetLayouts = &_vkPP1SRVLayout;
        pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
        if (vkCreatePipelineLayout(dev, &pli, nullptr, &_vkBloomPipelineLayout) != VK_SUCCESS) return false;
    }
    {
        VkPushConstantRange pcr{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16 };
        VkPipelineLayoutCreateInfo pli{}; pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = 1; pli.pSetLayouts = &_vkPP2SRVLayout;
        pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
        if (vkCreatePipelineLayout(dev, &pli, nullptr, &_vkTonemapPipelineLayout) != VK_SUCCESS) return false;
    }

    // Phase 17 descriptor pool
    {
        uint32_t nFIF = FRAMES_IN_FLIGHT;
        VkDescriptorPoolSize ps[] = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nFIF },
            { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  nFIF * 3 + 2 + 1 + 1 + 2 * 3 },  // 19
            { VK_DESCRIPTOR_TYPE_SAMPLER,        nFIF * 2 + 2 + 1 + 1 + 2 },       // 12
        };
        VkDescriptorPoolCreateInfo dpi{}; dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpi.poolSizeCount = (uint32_t)std::size(ps); dpi.pPoolSizes = ps;
        dpi.maxSets = nFIF + 2 + 1 + 1 + 2;  // FIF TAA + 2 bloom bright + blurH + blurV + 2 tonemap = 9
        if (vkCreateDescriptorPool(dev, &dpi, nullptr, &_vkPPDescPool) != VK_SUCCESS) return false;
    }

    // Per-frame TAA UBOs
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
        CreateBuffer(sizeof(VKTAAConstants),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            _vkTaaCB[i], _vkTaaCBMemory[i]);
        vkMapMemory(dev, _vkTaaCBMemory[i], 0, sizeof(VKTAAConstants), 0, &_vkTaaCBMapped[i]);
    }

    // TAA descriptor sets (per-frame); history binding (2) updated per-frame via UpdatePPDescriptors
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
        VkDescriptorSetAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = _vkPPDescPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &_vkTAALayout;
        vkAllocateDescriptorSets(dev, &ai, &_vkTAADescSet[i]);

        VkDescriptorBufferInfo ubi{ _vkTaaCB[i], 0, sizeof(VKTAAConstants) };
        // Phase 18B: if motion blur is available, TAA reads from MB output (_mbView)
        VkImageView curFrameView = (_mbView != VK_NULL_HANDLE) ? _mbView : _hdrView;
        VkDescriptorImageInfo curImg  { VK_NULL_HANDLE, curFrameView,      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo histImg { VK_NULL_HANDLE, _taaHistoryView[1], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo depthImg{ VK_NULL_HANDLE, _vkSwapchain.GetDepthView(),        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo bilinear{ _linearSampler,     VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
        VkDescriptorImageInfo point   { _pointClampSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
        VkWriteDescriptorSet ws[6]{};
        ws[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _vkTAADescSet[i], 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr,   &ubi,     nullptr };
        ws[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _vkTAADescSet[i], 1, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  &curImg,  nullptr, nullptr };
        ws[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _vkTAADescSet[i], 2, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  &histImg, nullptr, nullptr };
        ws[3] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _vkTAADescSet[i], 3, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  &depthImg,nullptr, nullptr };
        ws[4] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _vkTAADescSet[i], 4, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER,        &bilinear,nullptr, nullptr };
        ws[5] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _vkTAADescSet[i], 5, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER,        &point,   nullptr, nullptr };
        vkUpdateDescriptorSets(dev, 6, ws, 0, nullptr);
    }

    // BloomBright descriptor sets [2] ??set[i] reads taaHistory[i]
    for (int i = 0; i < 2; i++) {
        VkDescriptorSetAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = _vkPPDescPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &_vkPP1SRVLayout;
        vkAllocateDescriptorSets(dev, &ai, &_vkBloomBrightDescSet[i]);
        VkDescriptorImageInfo img{ VK_NULL_HANDLE, _taaHistoryView[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo sam{ _linearSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
        VkWriteDescriptorSet ws[2]{};
        ws[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _vkBloomBrightDescSet[i], 0, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &img, nullptr, nullptr };
        ws[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _vkBloomBrightDescSet[i], 1, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER,       &sam, nullptr, nullptr };
        vkUpdateDescriptorSets(dev, 2, ws, 0, nullptr);
    }

    // BloomBlurH descriptor set ??reads bloomBrightImage
    {
        VkDescriptorSetAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = _vkPPDescPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &_vkPP1SRVLayout;
        vkAllocateDescriptorSets(dev, &ai, &_vkBloomBlurHDescSet);
        VkDescriptorImageInfo img{ VK_NULL_HANDLE, _bloomBrightView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo sam{ _linearSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
        VkWriteDescriptorSet ws[2]{};
        ws[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _vkBloomBlurHDescSet, 0, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &img, nullptr, nullptr };
        ws[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _vkBloomBlurHDescSet, 1, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER,       &sam, nullptr, nullptr };
        vkUpdateDescriptorSets(dev, 2, ws, 0, nullptr);
    }

    // BloomBlurV descriptor set ??reads bloomBlurImage
    {
        VkDescriptorSetAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = _vkPPDescPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &_vkPP1SRVLayout;
        vkAllocateDescriptorSets(dev, &ai, &_vkBloomBlurVDescSet);
        VkDescriptorImageInfo img{ VK_NULL_HANDLE, _bloomBlurView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo sam{ _linearSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
        VkWriteDescriptorSet ws[2]{};
        ws[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _vkBloomBlurVDescSet, 0, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &img, nullptr, nullptr };
        ws[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _vkBloomBlurVDescSet, 1, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER,       &sam, nullptr, nullptr };
        vkUpdateDescriptorSets(dev, 2, ws, 0, nullptr);
    }

    // Full tonemap descriptor sets [2] ??set[i] reads taaHistory[i] + bloom + ssr
    for (int i = 0; i < 2; i++) {
        VkDescriptorSetAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = _vkPPDescPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &_vkPP2SRVLayout;
        vkAllocateDescriptorSets(dev, &ai, &_vkTonemapDescSet[i]);
        VkDescriptorImageInfo taaImg  { VK_NULL_HANDLE, _taaHistoryView[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo bloomImg{ VK_NULL_HANDLE, _bloomBrightView,   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo ssrImg  { VK_NULL_HANDLE, _ssrView,           VK_IMAGE_LAYOUT_GENERAL };
        VkDescriptorImageInfo sam     { _pointClampSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
        VkWriteDescriptorSet ws[4]{};
        ws[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _vkTonemapDescSet[i], 0, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &taaImg,   nullptr, nullptr };
        ws[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _vkTonemapDescSet[i], 1, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &bloomImg, nullptr, nullptr };
        ws[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _vkTonemapDescSet[i], 2, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &ssrImg,   nullptr, nullptr };
        ws[3] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _vkTonemapDescSet[i], 3, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER,       &sam,      nullptr, nullptr };
        vkUpdateDescriptorSets(dev, 4, ws, 0, nullptr);
    }

    // Compile Phase 17 shaders and build pipelines
    {
        std::vector<uint32_t> vsS, taaFS, bloomBrightFS, bloomBlurFS, tonemapFS;
        bool shadersOK =
            CompileHLSLtoSPIRV(GetShaderFullPath(L"fullscreen.vert.hlsl").wstring(),          L"vs_6_0", vsS)         &&
            CompileHLSLtoSPIRV(GetShaderFullPath(L"taa.frag.hlsl").wstring(),                 L"ps_6_0", taaFS)       &&
            CompileHLSLtoSPIRV(GetShaderFullPath(L"bloom_bright.frag.hlsl").wstring(),        L"ps_6_0", bloomBrightFS) &&
            CompileHLSLtoSPIRV(GetShaderFullPath(L"bloom_blur.frag.hlsl").wstring(),          L"ps_6_0", bloomBlurFS) &&
            CompileGLSLtoSPIRV(GetShaderFullPath(L"tonemapping_vk_full.frag.glsl").wstring(), tonemapFS);

        if (!shadersOK)
        {
            LUNA_LOG_WARN("VK PP17: shader compile failed ??TAA + bloom disabled, using Phase 16C tonemap");
        }
        else
        {
            auto mkMod = [&](const std::vector<uint32_t>& sp) {
                VkShaderModuleCreateInfo si{}; si.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
                si.codeSize = sp.size() * 4; si.pCode = sp.data();
                VkShaderModule m = VK_NULL_HANDLE; vkCreateShaderModule(dev, &si, nullptr, &m); return m;
            };

            auto mkGfxPipeline = [&](VkShaderModule vsM, VkShaderModule fsM,
                                      VkPipelineLayout layout, VkRenderPass rp) -> VkPipeline
            {
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
                VkPipeline p = VK_NULL_HANDLE;
                vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, nullptr, &p);
                return p;
            };

            VkShaderModule vsM           = mkMod(vsS);
            VkShaderModule taaFsM        = mkMod(taaFS);
            VkShaderModule bloomBrightFsM = mkMod(bloomBrightFS);
            VkShaderModule bloomBlurFsM  = mkMod(bloomBlurFS);
            VkShaderModule tonemapFsM    = mkMod(tonemapFS);

            _vkTAAPipeline         = mkGfxPipeline(vsM, taaFsM,         _vkTAAPipelineLayout,   _ppRenderPass);
            _vkBloomBrightPipeline = mkGfxPipeline(vsM, bloomBrightFsM, _vkBloomPipelineLayout, _ppRenderPass);
            _vkBloomBlurPipeline   = mkGfxPipeline(vsM, bloomBlurFsM,   _vkBloomPipelineLayout, _ppRenderPass);
            _vkTonemapPipeline     = mkGfxPipeline(vsM, tonemapFsM,     _vkTonemapPipelineLayout, _tonemapRenderPass);

            // Phase 18B: Motion Blur pipeline (uses _vkMBPipeLayout + _ppRenderPass)
            if (_vkMBPipeLayout && _mbFB)
            {
                std::vector<uint32_t> mbFS;
                if (CompileHLSLtoSPIRV(GetShaderFullPath(L"motion_blur.frag.hlsl").wstring(), L"ps_6_0", mbFS))
                {
                    VkShaderModule mbFsM = mkMod(mbFS);
                    _vkMBPipeline = mkGfxPipeline(vsM, mbFsM, _vkMBPipeLayout, _ppRenderPass);
                    vkDestroyShaderModule(dev, mbFsM, nullptr);
                }
                else
                    LUNA_LOG_WARN("VK PP18B: motion_blur.frag.hlsl compile failed ??motion blur disabled");
            }

            vkDestroyShaderModule(dev, vsM, nullptr);
            vkDestroyShaderModule(dev, taaFsM, nullptr);
            vkDestroyShaderModule(dev, bloomBrightFsM, nullptr);
            vkDestroyShaderModule(dev, bloomBlurFsM, nullptr);
            vkDestroyShaderModule(dev, tonemapFsM, nullptr);

            if (_vkTAAPipeline && _vkBloomBrightPipeline && _vkBloomBlurPipeline && _vkTonemapPipeline)
                LUNA_LOG_INFO("VK PP17: TAA + Bloom + full tonemap ready (%ux%u, bloom half=%ux%u)", W, H, halfW, halfH);
            else
                LUNA_LOG_WARN("VK PP17: one or more pipelines failed ??Phase 17 PP disabled");
        }
    }

    _vkPPResourcesValid = true;
    LUNA_LOG_INFO("VK PP: HDR RT + SSR + tonemap resources created (%ux%u)", W, H);
    return true;
}

void VulkanBackend::DestroyPPResources()
{
    if (!_device || _device->GetDevice() == VK_NULL_HANDLE) return;
    VkDevice dev = _device->GetDevice();

    _vkPPResourcesValid = false;

    auto dI = [&](VkImageView& v, VkImage& i, VkDeviceMemory& m) {
        if (v) { vkDestroyImageView(dev, v, nullptr); v = VK_NULL_HANDLE; }
        if (i) { vkDestroyImage(dev, i, nullptr);     i = VK_NULL_HANDLE; }
        if (m) { vkFreeMemory(dev, m, nullptr);       m = VK_NULL_HANDLE; }
    };

    // Phase 17: TAA + Bloom + full tonemap pipelines
    if (_vkTAAPipeline)         { vkDestroyPipeline(dev, _vkTAAPipeline,          nullptr); _vkTAAPipeline         = VK_NULL_HANDLE; }
    if (_vkBloomBrightPipeline) { vkDestroyPipeline(dev, _vkBloomBrightPipeline,  nullptr); _vkBloomBrightPipeline = VK_NULL_HANDLE; }
    if (_vkBloomBlurPipeline)   { vkDestroyPipeline(dev, _vkBloomBlurPipeline,    nullptr); _vkBloomBlurPipeline   = VK_NULL_HANDLE; }
    if (_vkTonemapPipeline)     { vkDestroyPipeline(dev, _vkTonemapPipeline,      nullptr); _vkTonemapPipeline     = VK_NULL_HANDLE; }
    if (_vkTAAPipelineLayout)   { vkDestroyPipelineLayout(dev, _vkTAAPipelineLayout,   nullptr); _vkTAAPipelineLayout   = VK_NULL_HANDLE; }
    if (_vkBloomPipelineLayout) { vkDestroyPipelineLayout(dev, _vkBloomPipelineLayout, nullptr); _vkBloomPipelineLayout = VK_NULL_HANDLE; }
    if (_vkTonemapPipelineLayout){ vkDestroyPipelineLayout(dev, _vkTonemapPipelineLayout, nullptr); _vkTonemapPipelineLayout = VK_NULL_HANDLE; }
    if (_vkPPDescPool) { vkDestroyDescriptorPool(dev, _vkPPDescPool, nullptr); _vkPPDescPool = VK_NULL_HANDLE; }
    if (_vkTAALayout)     { vkDestroyDescriptorSetLayout(dev, _vkTAALayout,    nullptr); _vkTAALayout    = VK_NULL_HANDLE; }
    if (_vkPP1SRVLayout)  { vkDestroyDescriptorSetLayout(dev, _vkPP1SRVLayout, nullptr); _vkPP1SRVLayout = VK_NULL_HANDLE; }
    if (_vkPP2SRVLayout)  { vkDestroyDescriptorSetLayout(dev, _vkPP2SRVLayout, nullptr); _vkPP2SRVLayout = VK_NULL_HANDLE; }
    for (int i = 0; i < 2; i++) { _vkBloomBrightDescSet[i] = VK_NULL_HANDLE; _vkTonemapDescSet[i] = VK_NULL_HANDLE; }
    _vkBloomBlurHDescSet = VK_NULL_HANDLE; _vkBloomBlurVDescSet = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
        _vkTAADescSet[i] = VK_NULL_HANDLE;
        if (_vkTaaCBMapped[i])  { vkUnmapMemory(dev, _vkTaaCBMemory[i]); _vkTaaCBMapped[i]  = nullptr; }
        if (_vkTaaCB[i])        { vkDestroyBuffer(dev, _vkTaaCB[i],      nullptr); _vkTaaCB[i]       = VK_NULL_HANDLE; }
        if (_vkTaaCBMemory[i])  { vkFreeMemory(dev, _vkTaaCBMemory[i],   nullptr); _vkTaaCBMemory[i] = VK_NULL_HANDLE; }
    }
    // Bloom framebuffers + images
    if (_bloomBlurFramebuffer)   { vkDestroyFramebuffer(dev, _bloomBlurFramebuffer,   nullptr); _bloomBlurFramebuffer   = VK_NULL_HANDLE; }
    if (_bloomBrightFramebuffer) { vkDestroyFramebuffer(dev, _bloomBrightFramebuffer, nullptr); _bloomBrightFramebuffer = VK_NULL_HANDLE; }
    dI(_bloomBlurView,   _bloomBlurImage,   _bloomBlurMemory);
    dI(_bloomBrightView, _bloomBrightImage, _bloomBrightMemory);
    // TAA history framebuffers + images
    for (int i = 0; i < 2; i++) {
        if (_taaFramebuffer[i]) { vkDestroyFramebuffer(dev, _taaFramebuffer[i], nullptr); _taaFramebuffer[i] = VK_NULL_HANDLE; }
        dI(_taaHistoryView[i], _taaHistoryImage[i], _taaHistoryMemory[i]);
    }

    // SSR compute
    if (_vkSSRPipeline)   { vkDestroyPipeline(dev, _vkSSRPipeline, nullptr);           _vkSSRPipeline   = VK_NULL_HANDLE; }
    if (_vkSSRPipeLayout) { vkDestroyPipelineLayout(dev, _vkSSRPipeLayout, nullptr);   _vkSSRPipeLayout = VK_NULL_HANDLE; }
    if (_vkSSRDescPool)   { vkDestroyDescriptorPool(dev, _vkSSRDescPool, nullptr);     _vkSSRDescPool   = VK_NULL_HANDLE; }
    if (_vkSSRLayout)     { vkDestroyDescriptorSetLayout(dev, _vkSSRLayout, nullptr);  _vkSSRLayout     = VK_NULL_HANDLE; }
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
        if (_vkSSRCBMapped[i]) { vkUnmapMemory(dev, _vkSSRCBMem[i]); _vkSSRCBMapped[i] = nullptr; }
        if (_vkSSRCB[i])    { vkDestroyBuffer(dev, _vkSSRCB[i], nullptr);  _vkSSRCB[i]    = VK_NULL_HANDLE; }
        if (_vkSSRCBMem[i]) { vkFreeMemory(dev, _vkSSRCBMem[i], nullptr);  _vkSSRCBMem[i] = VK_NULL_HANDLE; }
        _vkSSRDescSet[i] = VK_NULL_HANDLE;
    }

    // Tonemap
    if (_vkSSRTonemapPipeline)   { vkDestroyPipeline(dev, _vkSSRTonemapPipeline, nullptr);           _vkSSRTonemapPipeline   = VK_NULL_HANDLE; }
    if (_vkSSRTonemapPipeLayout) { vkDestroyPipelineLayout(dev, _vkSSRTonemapPipeLayout, nullptr);   _vkSSRTonemapPipeLayout = VK_NULL_HANDLE; }
    if (_vkSSRTonemapDescPool)   { vkDestroyDescriptorPool(dev, _vkSSRTonemapDescPool, nullptr);     _vkSSRTonemapDescPool   = VK_NULL_HANDLE; }
    if (_vkSSRTonemapLayout)     { vkDestroyDescriptorSetLayout(dev, _vkSSRTonemapLayout, nullptr);  _vkSSRTonemapLayout     = VK_NULL_HANDLE; }
    _vkSSRTonemapDescSet = VK_NULL_HANDLE;

    // SSR image
    dI(_ssrView, _ssrImage, _ssrMemory);

    // Phase 18B: Motion blur
    if (_vkMBPipeline)   { vkDestroyPipeline(dev, _vkMBPipeline, nullptr);           _vkMBPipeline   = VK_NULL_HANDLE; }
    if (_vkMBPipeLayout) { vkDestroyPipelineLayout(dev, _vkMBPipeLayout, nullptr);   _vkMBPipeLayout = VK_NULL_HANDLE; }
    if (_vkMBDescPool)   { vkDestroyDescriptorPool(dev, _vkMBDescPool, nullptr);     _vkMBDescPool   = VK_NULL_HANDLE; }
    if (_vkMBLayout)     { vkDestroyDescriptorSetLayout(dev, _vkMBLayout, nullptr);  _vkMBLayout     = VK_NULL_HANDLE; }
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++)
    {
        if (_vkMBCBMapped[i]) { vkUnmapMemory(dev, _vkMBCBMem[i]); _vkMBCBMapped[i] = nullptr; }
        if (_vkMBCB[i])    { vkDestroyBuffer(dev, _vkMBCB[i], nullptr);  _vkMBCB[i]    = VK_NULL_HANDLE; }
        if (_vkMBCBMem[i]) { vkFreeMemory(dev, _vkMBCBMem[i], nullptr);  _vkMBCBMem[i] = VK_NULL_HANDLE; }
        _vkMBDescSet[i] = VK_NULL_HANDLE;
    }
    if (_mbFB)   { vkDestroyFramebuffer(dev, _mbFB, nullptr); _mbFB = VK_NULL_HANDLE; }
    dI(_mbView, _mbImage, _mbMemory);

    // HDR framebuffer + image
    if (_deferredHDRFramebuffer) { vkDestroyFramebuffer(dev, _deferredHDRFramebuffer, nullptr); _deferredHDRFramebuffer = VK_NULL_HANDLE; }
    dI(_hdrView, _hdrImage, _hdrMemory);
}

// ===========================================================================
// Phase 17: PP helper functions
// ===========================================================================
void VulkanBackend::UpdatePPDescriptors()
{
    if (!_vkTAAPipeline || !_vkPPResourcesValid) { _vkFrameCount++; return; }
    VkDevice dev = _device->GetDevice();

    // Determine write/read ping-pong indices for this frame
    _vkTaaHistoryIndex = (int)(_vkFrameCount & 1);
    int readIdx        = _vkTaaHistoryIndex ^ 1;

    // Update binding=2 (historyFrame) in the TAA descriptor set for this CPU frame slot
    VkDescriptorImageInfo histImg{ VK_NULL_HANDLE, _taaHistoryView[readIdx], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet w{};
    w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet          = _vkTAADescSet[_frameIndex];
    w.dstBinding      = 2;
    w.descriptorCount = 1;
    w.descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w.pImageInfo      = &histImg;
    vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);

    _vkFrameCount++;
}

void VulkanBackend::DrawVKTAAPass()
{
    if (!_vkTAAPipeline) return;
    auto& frame = _frames[_frameIndex];
    VkCommandBuffer cmd = frame.cmdBuffer;
    uint32_t W = _vkSwapchain.GetExtent().width, H = _vkSwapchain.GetExtent().height;

    // Upload TAA constants
    {
        // Jittered VP for depth reconstruction (G-buffer rendered with jittered proj)
        XMMATRIX V   = XMLoadFloat4x4(&_deferredView);
        XMMATRIX P   = XMLoadFloat4x4(&_deferredProj);  // already jittered
        XMMATRIX VP  = XMMatrixMultiply(V, P);
        XMVECTOR det = XMMatrixDeterminant(VP);
        XMMATRIX iVP = XMMatrixInverse(&det, VP);
        XMFLOAT4X4 iVPF; XMStoreFloat4x4(&iVPF, iVP);

        VKTAAConstants cb{};
        memcpy(cb.invViewProj,  &iVPF, 64);              // jittered inverse VP
        memcpy(cb.prevViewProj, _vkPrevUnjitteredVP, 64); // previous frame's UNJITTERED VP
        cb.jitter[0]     = _vkCurJitter[0];  cb.jitter[1]     = _vkCurJitter[1];
        cb.prevJitter[0] = _vkPrevJitter[0]; cb.prevJitter[1] = _vkPrevJitter[1];
        cb.alpha = (_vkFrameCount < 8) ? 1.0f : 0.1f;  // match DX12: 8-frame warmup
        memcpy(_vkTaaCBMapped[_frameIndex], &cb, sizeof(cb));

        // Store current unjittered VP for next frame's TAA reprojection
        memcpy(_vkPrevUnjitteredVP, _vkUnjitteredVP, 64);
        _vkPrevJitter[0] = _vkCurJitter[0];
        _vkPrevJitter[1] = _vkCurJitter[1];
    }

    // TAA render pass ??_taaFramebuffer[write]
    VkClearValue clear{};
    VkRenderPassBeginInfo rpi{};
    rpi.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpi.renderPass        = _ppRenderPass;
    rpi.framebuffer       = _taaFramebuffer[_vkTaaHistoryIndex];
    rpi.renderArea.extent = _vkSwapchain.GetExtent();
    rpi.clearValueCount   = 1;
    rpi.pClearValues      = &clear;
    vkCmdBeginRenderPass(cmd, &rpi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{ 0, 0, (float)W, (float)H, 0, 1 };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc{ {0,0}, _vkSwapchain.GetExtent() };
    vkCmdSetScissor(cmd, 0, 1, &sc);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _vkTAAPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                             _vkTAAPipelineLayout, 0, 1, &_vkTAADescSet[_frameIndex], 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    // Trailing barrier removed ??VulkanRenderGraph handles inter-pass transitions (Phase 18C)
}

void VulkanBackend::DrawVKBloomBrightPass()
{
    if (!_vkBloomBrightPipeline) return;
    auto& frame = _frames[_frameIndex];
    VkCommandBuffer cmd = frame.cmdBuffer;
    uint32_t halfW = std::max(1u, _vkSwapchain.GetExtent().width  / 2);
    uint32_t halfH = std::max(1u, _vkSwapchain.GetExtent().height / 2);

    VkClearValue clear{};
    VkRenderPassBeginInfo rpi{};
    rpi.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpi.renderPass        = _ppRenderPass;
    rpi.framebuffer       = _bloomBrightFramebuffer;
    rpi.renderArea.extent = { halfW, halfH };
    rpi.clearValueCount   = 1;
    rpi.pClearValues      = &clear;
    vkCmdBeginRenderPass(cmd, &rpi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{ 0, 0, (float)halfW, (float)halfH, 0, 1 };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc{ {0,0}, {halfW, halfH} };
    vkCmdSetScissor(cmd, 0, 1, &sc);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _vkBloomBrightPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                             _vkBloomPipelineLayout, 0, 1, &_vkBloomBrightDescSet[_vkTaaHistoryIndex], 0, nullptr);
    struct { float threshold; float knee; float pad[2]; } pc{ 0.8f, 0.1f, {0.f, 0.f} };
    vkCmdPushConstants(cmd, _vkBloomPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    // Trailing barrier removed ??VulkanRenderGraph handles inter-pass transitions (Phase 18C)
}

void VulkanBackend::DrawVKBloomBlurPass(bool horizontal)
{
    if (!_vkBloomBlurPipeline) return;
    auto& frame = _frames[_frameIndex];
    VkCommandBuffer cmd = frame.cmdBuffer;
    uint32_t halfW = std::max(1u, _vkSwapchain.GetExtent().width  / 2);
    uint32_t halfH = std::max(1u, _vkSwapchain.GetExtent().height / 2);

    // H-blur: bloomBright ??bloomBlur;  V-blur: bloomBlur ??bloomBright
    VkDescriptorSet srcSet = horizontal ? _vkBloomBlurHDescSet : _vkBloomBlurVDescSet;
    VkFramebuffer   dstFB  = horizontal ? _bloomBlurFramebuffer  : _bloomBrightFramebuffer;
    VkImage         dstImg = horizontal ? _bloomBlurImage         : _bloomBrightImage;

    VkClearValue clear{};
    VkRenderPassBeginInfo rpi{};
    rpi.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpi.renderPass        = _ppRenderPass;
    rpi.framebuffer       = dstFB;
    rpi.renderArea.extent = { halfW, halfH };
    rpi.clearValueCount   = 1;
    rpi.pClearValues      = &clear;
    vkCmdBeginRenderPass(cmd, &rpi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{ 0, 0, (float)halfW, (float)halfH, 0, 1 };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc{ {0,0}, {halfW, halfH} };
    vkCmdSetScissor(cmd, 0, 1, &sc);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _vkBloomBlurPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                             _vkBloomPipelineLayout, 0, 1, &srcSet, 0, nullptr);
    struct { float texelX; float texelY; float pad[2]; } pc{};
    if (horizontal) { pc.texelX = 1.0f / (float)halfW; }
    else            { pc.texelY = 1.0f / (float)halfH; }
    vkCmdPushConstants(cmd, _vkBloomPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    // Trailing barrier removed ??VulkanRenderGraph handles inter-pass transitions (Phase 18C)
}

void VulkanBackend::DrawVKTonemapPass()
{
    if (!_vkTonemapPipeline) return;
    auto& frame = _frames[_frameIndex];
    VkCommandBuffer cmd = frame.cmdBuffer;

    VkClearValue clear{};
    VkRenderPassBeginInfo rpi{};
    rpi.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpi.renderPass        = _tonemapRenderPass;
    rpi.framebuffer       = _tonemapFramebuffers[_imageIndex];
    rpi.renderArea.extent = _vkSwapchain.GetExtent();
    rpi.clearValueCount   = 1;
    rpi.pClearValues      = &clear;
    vkCmdBeginRenderPass(cmd, &rpi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{ 0, 0, (float)_vkSwapchain.GetExtent().width, (float)_vkSwapchain.GetExtent().height, 0, 1 };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc{ {0,0}, _vkSwapchain.GetExtent() };
    vkCmdSetScissor(cmd, 0, 1, &sc);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _vkTonemapPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                             _vkTonemapPipelineLayout, 0, 1, &_vkTonemapDescSet[_vkTaaHistoryIndex], 0, nullptr);
    struct { float bloomStrength; float exposure; float pad[2]; } pc{ 0.04f, 1.0f, {0.f, 0.f} };
    vkCmdPushConstants(cmd, _vkTonemapPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
}

void VulkanBackend::DrawSSAOPass(VkCommandBuffer cmd)
{
    if (!_ssaoPipeline) return;
    uint32_t halfW = std::max(1u, _vkSwapchain.GetExtent().width  / 2);
    uint32_t halfH = std::max(1u, _vkSwapchain.GetExtent().height / 2);

    // Update UBO
    XMMATRIX proj = XMLoadFloat4x4(&_lastProj);
    XMMATRIX view = XMLoadFloat4x4(&_lastView);
    XMStoreFloat4x4(&_ssaoKernel.projection,    proj);
    XMStoreFloat4x4(&_ssaoKernel.invProjection, XMMatrixInverse(nullptr, proj));
    XMStoreFloat4x4(&_ssaoKernel.view,          view);
    _ssaoKernel.noiseScale = XMFLOAT2(float(halfW) / 4.f, float(halfH) / 4.f);
    memcpy(_ssaoCBMapped[_frameIndex], &_ssaoKernel, sizeof(SSAOConstants));

    VkClearValue clear{}; clear.color.float32[0] = 1.0f;
    VkRenderPassBeginInfo rpi{}; rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpi.renderPass = _ssaoRenderPass; rpi.framebuffer = _ssaoFramebuffer;
    rpi.renderArea.extent = { halfW, halfH }; rpi.clearValueCount = 1; rpi.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rpi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{ 0, 0, (float)halfW, (float)halfH, 0, 1 }; vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc{ {0,0}, {halfW, halfH} }; vkCmdSetScissor(cmd, 0, 1, &sc);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _ssaoPipeline);
    VkDescriptorSet sets[] = { _ssaoSceneDescSet[_frameIndex], _ssaoTexDescSet };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _ssaoPipeLayout, 0, 2, sets, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
}

void VulkanBackend::DrawSSAOBlurPass(VkCommandBuffer cmd)
{
    if (!_ssaoBlurPipeline) return;
    uint32_t halfW = std::max(1u, _vkSwapchain.GetExtent().width  / 2);
    uint32_t halfH = std::max(1u, _vkSwapchain.GetExtent().height / 2);

    VkClearValue clear{}; clear.color.float32[0] = 1.0f;
    VkRenderPassBeginInfo rpi{}; rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpi.renderPass = _ssaoRenderPass; rpi.framebuffer = _ssaoBlurFramebuffer;
    rpi.renderArea.extent = { halfW, halfH }; rpi.clearValueCount = 1; rpi.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rpi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{ 0, 0, (float)halfW, (float)halfH, 0, 1 }; vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc{ {0,0}, {halfW, halfH} }; vkCmdSetScissor(cmd, 0, 1, &sc);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _ssaoBlurPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _ssaoBlurPipeLayout, 0, 1, &_ssaoBlurDescSet, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
}

void VulkanBackend::DestroyDeferredPipeline()
{
    VkDevice dev = _device->GetDevice();
    DestroyGIDeferredResources();
    if (_deferredIBLPipeline) { vkDestroyPipeline(dev, _deferredIBLPipeline, nullptr);         _deferredIBLPipeline = VK_NULL_HANDLE; }
    if (_deferredPipeline)    { vkDestroyPipeline(dev, _deferredPipeline, nullptr);            _deferredPipeline    = VK_NULL_HANDLE; }
    if (_deferredPipeLayout)  { vkDestroyPipelineLayout(dev, _deferredPipeLayout, nullptr);    _deferredPipeLayout  = VK_NULL_HANDLE; }
    if (_deferredSceneLayout) { vkDestroyDescriptorSetLayout(dev, _deferredSceneLayout, nullptr); _deferredSceneLayout = VK_NULL_HANDLE; }
    if (_deferredGbufLayout)  { vkDestroyDescriptorSetLayout(dev, _deferredGbufLayout, nullptr);  _deferredGbufLayout  = VK_NULL_HANDLE; }
    if (_deferredDescPool)    { vkDestroyDescriptorPool(dev, _deferredDescPool, nullptr);      _deferredDescPool    = VK_NULL_HANDLE; }
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
        if (_deferredSceneCB[i])    { vkDestroyBuffer(dev, _deferredSceneCB[i], nullptr);     _deferredSceneCB[i]    = VK_NULL_HANDLE; }
        if (_deferredSceneCBMem[i]) { vkFreeMemory(dev, _deferredSceneCBMem[i], nullptr);     _deferredSceneCBMem[i] = VK_NULL_HANDLE; }
    }
    if (_pointClampSampler) { vkDestroySampler(dev, _pointClampSampler, nullptr); _pointClampSampler = VK_NULL_HANDLE; }
}

void VulkanBackend::DestroyPipeline() {
    VkDevice dev = _device->GetDevice();
    _atmosphere.Destroy();        // Phase 28: destroy atmosphere LUTs + pipelines
    _ibl.Destroy();               // Phase 15C: destroy IBL textures + compute pipelines
    DestroyMeshShaderResources(); // Phase 27: destroy mesh shader pipeline + meshlet buffers
    DestroyIndirectResources();   // Phase 15B: destroy GPU-driven buffers + pipelines
    DestroyDeferredPipeline();
    DestroySSAOResources();
    _gBuffer.Destroy();
    if (_gbPipeline)        { vkDestroyPipeline(dev, _gbPipeline, nullptr);                   _gbPipeline         = VK_NULL_HANDLE; }
    if (_graphicsPipeline)  { vkDestroyPipeline(dev, _graphicsPipeline, nullptr);             _graphicsPipeline   = VK_NULL_HANDLE; }
    if (_pipelineLayout)    { vkDestroyPipelineLayout(dev, _pipelineLayout, nullptr);         _pipelineLayout     = VK_NULL_HANDLE; }
    if (_mvpDescLayout)     { vkDestroyDescriptorSetLayout(dev, _mvpDescLayout, nullptr);     _mvpDescLayout      = VK_NULL_HANDLE; }
    if (_materialDescLayout){ vkDestroyDescriptorSetLayout(dev, _materialDescLayout, nullptr); _materialDescLayout = VK_NULL_HANDLE; }
    if (_sceneDescPool)     { vkDestroyDescriptorPool(dev, _sceneDescPool, nullptr);          _sceneDescPool      = VK_NULL_HANDLE; }
}

// ===========================================================================
// Phase 15B: GPU-driven indirect rendering
// ===========================================================================

// MeshDrawInfo for Vulkan indirect (same layout as Mesh.h but without D3D12 types)
struct VkMeshDrawInfo
{
    uint32_t indexCount;
    uint32_t firstIndex;
    int32_t  vertexOffset;
    uint32_t _pad;
};
static_assert(sizeof(VkMeshDrawInfo) == 16, "VkMeshDrawInfo must be 16 bytes");

struct VkDrawIndexedIndirectCmd  // VkDrawIndexedIndirectCommand
{
    uint32_t indexCount;
    uint32_t instanceCount;
    uint32_t firstIndex;
    int32_t  vertexOffset;
    uint32_t firstInstance;  // carries objectIndex
};
static_assert(sizeof(VkDrawIndexedIndirectCmd) == 20, "VkDrawIndexedIndirectCmd must be 20 bytes");

void VulkanBackend::BuildMergedGeometry(const std::vector<std::vector<PBRVertex>>& allVerts,
                                         const std::vector<std::vector<uint32_t>>& allIdxs)
{
    if (allVerts.empty()) return;
    VkDevice dev = _device->GetDevice();

    // Flatten all vertex/index data and build meshInfo SSBO
    std::vector<PBRVertex>     mergedV;
    std::vector<uint32_t>      mergedI;
    std::vector<VkMeshDrawInfo> meshInfos;

    _meshASInfoCache.clear();
    for (size_t i = 0; i < allVerts.size(); i++) {
        VkMeshDrawInfo mi{};
        mi.indexCount   = (uint32_t)allIdxs[i].size();
        mi.firstIndex   = (uint32_t)mergedI.size();
        mi.vertexOffset = (int32_t)mergedV.size();
        meshInfos.push_back(mi);

        // Phase 18D: cache per-mesh info for BLAS building
        MeshASInfo asInfo{};
        asInfo.indexCount   = mi.indexCount;
        asInfo.firstIndex   = mi.firstIndex;
        asInfo.vertexOffset = mi.vertexOffset;
        asInfo.vertexCount  = (uint32_t)allVerts[i].size();
        _meshASInfoCache.push_back(asInfo);

        mergedV.insert(mergedV.end(), allVerts[i].begin(), allVerts[i].end());
        mergedI.insert(mergedI.end(), allIdxs[i].begin(), allIdxs[i].end());
    }

    // Merged vertex buffer (device-local via staging)
    {
        VkDeviceSize sz = mergedV.size() * sizeof(PBRVertex);
        VkBuffer stg; VkDeviceMemory stgMem;
        CreateBuffer(sz, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            stg, stgMem);
        void* p; vkMapMemory(dev, stgMem, 0, sz, 0, &p);
        memcpy(p, mergedV.data(), (size_t)sz);
        vkUnmapMemory(dev, stgMem);

        VkBufferUsageFlags vbUsage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
                                   | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;  // Phase 27: mesh shader SSBO access
        if (_rtSupported)  // Phase 18D: AS build requires device-addressable vertex data
            vbUsage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR
                     | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        CreateBuffer(sz, vbUsage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, _mergedVB, _mergedVBMem);
        CopyBuffer(stg, _mergedVB, sz);
        vkDestroyBuffer(dev, stg, nullptr); vkFreeMemory(dev, stgMem, nullptr);
    }

    // Merged index buffer
    {
        VkDeviceSize sz = mergedI.size() * sizeof(uint32_t);
        VkBuffer stg; VkDeviceMemory stgMem;
        CreateBuffer(sz, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            stg, stgMem);
        void* p; vkMapMemory(dev, stgMem, 0, sz, 0, &p);
        memcpy(p, mergedI.data(), (size_t)sz);
        vkUnmapMemory(dev, stgMem);

        VkBufferUsageFlags ibUsage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT
                                   | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;  // Phase 32: vis shade compute reads IB as SSBO
        if (_rtSupported)  // Phase 18D
            ibUsage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR
                     | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        CreateBuffer(sz, ibUsage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, _mergedIB, _mergedIBMem);
        CopyBuffer(stg, _mergedIB, sz);
        vkDestroyBuffer(dev, stg, nullptr); vkFreeMemory(dev, stgMem, nullptr);
    }

    // Mesh info SSBO (device-local via staging)
    {
        VkDeviceSize sz = meshInfos.size() * sizeof(VkMeshDrawInfo);
        VkBuffer stg; VkDeviceMemory stgMem;
        CreateBuffer(sz, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            stg, stgMem);
        void* p; vkMapMemory(dev, stgMem, 0, sz, 0, &p);
        memcpy(p, meshInfos.data(), (size_t)sz);
        vkUnmapMemory(dev, stgMem);

        CreateBuffer(sz,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, _meshInfoBuf, _meshInfoMem);
        CopyBuffer(stg, _meshInfoBuf, sz);
        vkDestroyBuffer(dev, stg, nullptr); vkFreeMemory(dev, stgMem, nullptr);
    }

    LUNA_LOG_INFO("VK: Merged geometry: %zu verts, %zu indices, %zu meshes",
        mergedV.size(), mergedI.size(), meshInfos.size());

    // -----------------------------------------------------------------------
    // Phase 27: Build meshlets and upload to GPU buffers (if mesh shaders supported)
    // -----------------------------------------------------------------------
    if (_device->IsMeshShaderSupported())
    {
        _vkMeshMeshletOffsets.clear();
        _vkMeshMeshletCounts.clear();

        std::vector<Meshlet>       allMeshlets;
        std::vector<MeshletBounds> allBounds;
        std::vector<uint32_t>      allMeshletVerts;
        std::vector<uint32_t>      allMeshletTris;

        for (size_t m = 0; m < allVerts.size(); ++m)
        {
            // Extract positions from PBRVertex array
            std::vector<DirectX::XMFLOAT3> positions(allVerts[m].size());
            for (size_t v = 0; v < allVerts[m].size(); ++v)
            {
                positions[v].x = allVerts[m][v].position.x;
                positions[v].y = allVerts[m][v].position.y;
                positions[v].z = allVerts[m][v].position.z;
            }

            MeshletBuildResult mbr = BuildMeshlets(
                positions.data(), (uint32_t)positions.size(),
                allIdxs[m].data(), (uint32_t)allIdxs[m].size());

            _vkMeshMeshletOffsets.push_back((uint32_t)allMeshlets.size());
            _vkMeshMeshletCounts.push_back((uint32_t)mbr.meshlets.size());

            // Offset meshlet vertex/triangle references to global merged arrays
            uint32_t vertBase = (uint32_t)allMeshletVerts.size();
            uint32_t triBase  = (uint32_t)allMeshletTris.size();
            int32_t meshVertOffset = meshInfos[m].vertexOffset;

            for (auto& ml : mbr.meshlets)
            {
                Meshlet gml = ml;
                gml.vertexOffset   += vertBase;
                gml.triangleOffset += triBase;
                allMeshlets.push_back(gml);
            }
            allBounds.insert(allBounds.end(), mbr.bounds.begin(), mbr.bounds.end());

            // Offset meshlet vertex indices to global merged vertex buffer
            for (uint32_t vi : mbr.meshletVertices)
                allMeshletVerts.push_back(vi + meshVertOffset);

            allMeshletTris.insert(allMeshletTris.end(),
                mbr.meshletTriangles.begin(), mbr.meshletTriangles.end());
        }

        LUNA_LOG_INFO("VK Mesh Shader: %zu meshlets, %zu meshletVerts, %zu meshletTris",
            allMeshlets.size(), allMeshletVerts.size(), allMeshletTris.size());

        // Upload meshlet buffers (staging → device-local SSBO)
        auto uploadSSBO = [&](const void* data, VkDeviceSize sz, VkBuffer& outBuf, VkDeviceMemory& outMem)
        {
            VkBuffer stg; VkDeviceMemory stgMem;
            CreateBuffer(sz, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stg, stgMem);
            void* p; vkMapMemory(dev, stgMem, 0, sz, 0, &p);
            memcpy(p, data, (size_t)sz);
            vkUnmapMemory(dev, stgMem);
            CreateBuffer(sz, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, outBuf, outMem);
            CopyBuffer(stg, outBuf, sz);
            vkDestroyBuffer(dev, stg, nullptr); vkFreeMemory(dev, stgMem, nullptr);
        };

        if (!allMeshlets.empty())
        {
            uploadSSBO(allMeshlets.data(),     allMeshlets.size()     * sizeof(Meshlet),       _vkMeshletBuffer,       _vkMeshletMem);
            uploadSSBO(allBounds.data(),       allBounds.size()       * sizeof(MeshletBounds),  _vkMeshletBoundsBuffer, _vkMeshletBoundsMem);
            uploadSSBO(allMeshletVerts.data(),  allMeshletVerts.size() * sizeof(uint32_t),       _vkMeshletVertBuffer,   _vkMeshletVertMem);
            uploadSSBO(allMeshletTris.data(),   allMeshletTris.size()  * sizeof(uint32_t),       _vkMeshletTriBuffer,    _vkMeshletTriMem);
        }
    }
}

// ===========================================================================
// Phase 27: Mesh shader pipeline creation
// ===========================================================================
bool VulkanBackend::CreateMeshShaderResources()
{
    if (!_device->IsMeshShaderSupported()) return false;
    if (!_vkMeshletBuffer || !_objectDataBuffer) return false;

    VkDevice dev = _device->GetDevice();

    // Load vkCmdDrawMeshTasksEXT function pointer
    pfn_vkCmdDrawMeshTasksEXT = (PFN_vkCmdDrawMeshTasksEXT)
        vkGetDeviceProcAddr(dev, "vkCmdDrawMeshTasksEXT");
    if (!pfn_vkCmdDrawMeshTasksEXT)
    {
        LUNA_LOG_WARN("VK Mesh Shader: vkCmdDrawMeshTasksEXT not found");
        return false;
    }

    // --- Descriptor set layout (set=0): 6 SSBO bindings ---
    VkDescriptorSetLayoutBinding bindings[6]{};
    for (uint32_t i = 0; i < 6; ++i)
    {
        bindings[i].binding         = i;
        bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags      = VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT;
    }
    // binding 3 (vertices) also readable from mesh shader only, but stageFlags already includes both

    VkDescriptorSetLayoutCreateInfo dslci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dslci.bindingCount = 6;
    dslci.pBindings    = bindings;
    if (vkCreateDescriptorSetLayout(dev, &dslci, nullptr, &_meshShaderDescLayout) != VK_SUCCESS)
        return false;

    // --- Descriptor pool ---
    VkDescriptorPoolSize poolSize{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 6 };
    VkDescriptorPoolCreateInfo dpci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpci.maxSets       = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes    = &poolSize;
    if (vkCreateDescriptorPool(dev, &dpci, nullptr, &_meshShaderDescPool) != VK_SUCCESS)
        return false;

    // --- Allocate descriptor set ---
    VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dsai.descriptorPool     = _meshShaderDescPool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts        = &_meshShaderDescLayout;
    if (vkAllocateDescriptorSets(dev, &dsai, &_meshShaderDescSet) != VK_SUCCESS)
        return false;

    // --- Write descriptor set ---
    VkDescriptorBufferInfo bufInfos[6]{};
    bufInfos[0] = { _objectDataBuffer,       0, VK_WHOLE_SIZE };  // binding 0: objectData
    bufInfos[1] = { _vkMeshletBuffer,        0, VK_WHOLE_SIZE };  // binding 1: meshlets
    bufInfos[2] = { _vkMeshletBoundsBuffer,  0, VK_WHOLE_SIZE };  // binding 2: bounds
    bufInfos[3] = { _mergedVB,               0, VK_WHOLE_SIZE };  // binding 3: vertices (PBRVertex SSBO)
    bufInfos[4] = { _vkMeshletVertBuffer,    0, VK_WHOLE_SIZE };  // binding 4: meshletVertices
    bufInfos[5] = { _vkMeshletTriBuffer,     0, VK_WHOLE_SIZE };  // binding 5: meshletTriangles

    VkWriteDescriptorSet writes[6]{};
    for (uint32_t i = 0; i < 6; ++i)
    {
        writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet          = _meshShaderDescSet;
        writes[i].dstBinding      = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo     = &bufInfos[i];
    }
    vkUpdateDescriptorSets(dev, 6, writes, 0, nullptr);

    // --- Pipeline layout: set=0 (mesh SSBOs) + set=1 (bindless materials) + push constants ---
    VkDescriptorSetLayout setLayouts[2] = { _meshShaderDescLayout, _indirectMaterialLayout };
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT;
    pcRange.offset     = 0;
    pcRange.size       = 240;  // viewMatrix(64) + projMatrix(64) + frustumPlanes(96) + 3*uint+pad(16)

    VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.setLayoutCount         = 2;
    plci.pSetLayouts            = setLayouts;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges    = &pcRange;
    if (vkCreatePipelineLayout(dev, &plci, nullptr, &_meshShaderPipeLayout) != VK_SUCCESS)
        return false;

    // --- Compile shaders ---
    std::vector<uint32_t> taskSpv, meshSpv, fragSpv;
    if (!CompileGLSLtoSPIRV(GetShaderFullPath(L"meshlet_cull_vk.task.glsl").wstring(), taskSpv) ||
        !CompileGLSLtoSPIRV(GetShaderFullPath(L"gbuffer_mesh_vk.mesh.glsl").wstring(), meshSpv) ||
        !CompileGLSLtoSPIRV(GetShaderFullPath(L"gbuffer_indirect_vk.frag.glsl").wstring(), fragSpv))
    {
        LUNA_LOG_ERROR("VK Mesh Shader: shader compilation failed");
        return false;
    }

    auto mkMod = [&](const std::vector<uint32_t>& spv) -> VkShaderModule {
        VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        ci.codeSize = spv.size() * 4;
        ci.pCode    = spv.data();
        VkShaderModule m = VK_NULL_HANDLE;
        vkCreateShaderModule(dev, &ci, nullptr, &m);
        return m;
    };
    VkShaderModule taskMod = mkMod(taskSpv);
    VkShaderModule meshMod = mkMod(meshSpv);
    VkShaderModule fragMod = mkMod(fragSpv);

    // --- Graphics pipeline (task + mesh + fragment, no vertex input) ---
    VkPipelineShaderStageCreateInfo stages[3]{};
    stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                  VK_SHADER_STAGE_TASK_BIT_EXT, taskMod, "main" };
    stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                  VK_SHADER_STAGE_MESH_BIT_EXT, meshMod, "main" };
    stages[2] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                  VK_SHADER_STAGE_FRAGMENT_BIT, fragMod, "main" };

    // No vertex input — mesh shader fetches from SSBO
    VkPipelineViewportStateCreateInfo vpState{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vpState.viewportCount = 1;
    vpState.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_BACK_BIT;
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo dss{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    dss.depthTestEnable  = VK_TRUE;
    dss.depthWriteEnable = VK_TRUE;
    dss.depthCompareOp   = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState cbas[3]{};
    for (int i = 0; i < 3; ++i)
        cbas[i].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                 VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cbs{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cbs.attachmentCount = 3;
    cbs.pAttachments    = cbas;

    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynState{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynState.dynamicStateCount = 2;
    dynState.pDynamicStates    = dynStates;

    VkGraphicsPipelineCreateInfo gpci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gpci.stageCount          = 3;
    gpci.pStages             = stages;
    gpci.pVertexInputState   = nullptr;   // no vertex input for mesh shaders
    gpci.pInputAssemblyState = nullptr;   // no input assembly for mesh shaders
    gpci.pViewportState      = &vpState;
    gpci.pRasterizationState = &rs;
    gpci.pMultisampleState   = &ms;
    gpci.pDepthStencilState  = &dss;
    gpci.pColorBlendState    = &cbs;
    gpci.pDynamicState       = &dynState;
    gpci.layout              = _meshShaderPipeLayout;
    gpci.renderPass          = _gBuffer.GetRenderPassLoad();
    gpci.subpass             = 0;

    VkResult vr = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, nullptr, &_meshShaderPipeline);

    vkDestroyShaderModule(dev, taskMod, nullptr);
    vkDestroyShaderModule(dev, meshMod, nullptr);
    vkDestroyShaderModule(dev, fragMod, nullptr);

    if (vr != VK_SUCCESS)
    {
        LUNA_LOG_ERROR("VK Mesh Shader: pipeline creation failed: %d", vr);
        return false;
    }

    _meshShaderReady = true;
    LUNA_LOG_INFO("VK Mesh Shader: pipeline ready (task + mesh + fragment)");
    return true;
}

void VulkanBackend::DestroyMeshShaderResources()
{
    VkDevice dev = _device->GetDevice();
    if (_meshShaderPipeline)   { vkDestroyPipeline(dev, _meshShaderPipeline, nullptr);             _meshShaderPipeline = VK_NULL_HANDLE; }
    if (_meshShaderPipeLayout) { vkDestroyPipelineLayout(dev, _meshShaderPipeLayout, nullptr);     _meshShaderPipeLayout = VK_NULL_HANDLE; }
    if (_meshShaderDescPool)   { vkDestroyDescriptorPool(dev, _meshShaderDescPool, nullptr);       _meshShaderDescPool = VK_NULL_HANDLE; }
    if (_meshShaderDescLayout) { vkDestroyDescriptorSetLayout(dev, _meshShaderDescLayout, nullptr);_meshShaderDescLayout = VK_NULL_HANDLE; }

    auto destroyBuf = [&](VkBuffer& b, VkDeviceMemory& m) {
        if (b) { vkDestroyBuffer(dev, b, nullptr); b = VK_NULL_HANDLE; }
        if (m) { vkFreeMemory(dev, m, nullptr); m = VK_NULL_HANDLE; }
    };
    destroyBuf(_vkMeshletBuffer,       _vkMeshletMem);
    destroyBuf(_vkMeshletBoundsBuffer, _vkMeshletBoundsMem);
    destroyBuf(_vkMeshletVertBuffer,   _vkMeshletVertMem);
    destroyBuf(_vkMeshletTriBuffer,    _vkMeshletTriMem);

    _meshShaderReady = false;
}

bool VulkanBackend::CreateIndirectResources()
{
    if (!_mergedVB || !_mergedIB || !_meshInfoBuf) return false;
    if (_vkSceneMeshes.empty()) return false;

    VkDevice dev = _device->GetDevice();
    uint32_t meshCount = (uint32_t)_vkSceneMeshes.size();

    // ?�?�?objectData SSBO (HOST_VISIBLE|COHERENT, persistently mapped) ?�?�?�?�?�?�?�?�?�?
    {
        VkDeviceSize sz = MAX_GPU_OBJECTS * sizeof(GPUObjectDataVK);
        if (!CreateBuffer(sz,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                _objectDataBuffer, _objectDataMem))
            return false;
        vkMapMemory(dev, _objectDataMem, 0, sz, 0, &_objectDataMapped);
    }

    // ?�?�?Per-frame indirect arg + draw count buffers (DEVICE_LOCAL)
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
        VkDeviceSize argSz   = MAX_GPU_OBJECTS * sizeof(VkDrawIndexedIndirectCmd);
        VkDeviceSize countSz = sizeof(uint32_t);

        if (!CreateBuffer(argSz,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                _indirectArgBuffer[i], _indirectArgMem[i]))
            return false;

        if (!CreateBuffer(countSz,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                _drawCountBuffer[i], _drawCountMem[i]))
            return false;
    }

    // Collect unique materials for factor SSBO and bindless textures ?
    std::vector<VkMaterial*> uniqueMats;
    {
        std::set<VkMaterial*> seen;
        for (auto& sm : _vkSceneMeshes) {
            if (sm->material && seen.find(sm->material.get()) == seen.end()) {
                seen.insert(sm->material.get());
                uniqueMats.push_back(sm->material.get());
            }
        }
    }
    uint32_t matCount = (uint32_t)uniqueMats.size();

    // Material factor SSBO
    {
        std::vector<MaterialFactorsVK> factors(matCount);
        for (uint32_t i = 0; i < matCount; i++) {
            auto* m = uniqueMats[i];
            factors[i].albedoR = m->albedoFactor[0]; factors[i].albedoG = m->albedoFactor[1];
            factors[i].albedoB = m->albedoFactor[2]; factors[i].albedoA = m->albedoFactor[3];
            factors[i].metallicFactor  = m->metallicFactor;
            factors[i].roughnessFactor = m->roughnessFactor;
        }
        VkDeviceSize sz = matCount * sizeof(MaterialFactorsVK);
        VkBuffer stg; VkDeviceMemory stgMem;
        CreateBuffer(sz, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            stg, stgMem);
        void* p; vkMapMemory(dev, stgMem, 0, sz, 0, &p);
        memcpy(p, factors.data(), (size_t)sz);
        vkUnmapMemory(dev, stgMem);

        CreateBuffer(sz,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, _matFactorBuffer, _matFactorMem);
        CopyBuffer(stg, _matFactorBuffer, sz);
        vkDestroyBuffer(dev, stg, nullptr); vkFreeMemory(dev, stgMem, nullptr);
    }

    // Bindless material descriptor set layout (set=1)
    // binding 0: material factor SSBO
    // binding 1: albedo   texture array (runtime, PARTIALLY_BOUND)
    // binding 2: normal   texture array (runtime, PARTIALLY_BOUND)
    // binding 3: metalRough texture array (runtime, PARTIALLY_BOUND)
    // binding 4: sampler
    {
        VkDescriptorSetLayoutBinding bs[6]{};
        // Phase 32: include COMPUTE_BIT so visibility shade compute can also sample textures
        constexpr VkShaderStageFlags kMatStages = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
        bs[0] = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         1,         kMatStages, nullptr };
        bs[1] = { 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,          matCount,  kMatStages, nullptr };
        bs[2] = { 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,          matCount,  kMatStages, nullptr };
        bs[3] = { 3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,          matCount,  kMatStages, nullptr };
        bs[4] = { 4, VK_DESCRIPTOR_TYPE_SAMPLER,                1,         kMatStages, nullptr };
        bs[5] = { 5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,          matCount,  kMatStages, nullptr };

        // PARTIALLY_BOUND_BIT allows slots to be unwritten (e.g., if matCount < array size)
        // VARIABLE_DESCRIPTOR_COUNT_BIT is NOT used ??it can only be on the last binding.
        VkDescriptorBindingFlags bindFlags[6] = {
            0,
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT,
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT,
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT,
            0,
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT
        };
        VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{};
        flagsInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        flagsInfo.bindingCount  = 6;
        flagsInfo.pBindingFlags = bindFlags;

        VkDescriptorSetLayoutCreateInfo li{};
        li.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.pNext        = &flagsInfo;
        li.bindingCount = 6;
        li.pBindings    = bs;
        vkCreateDescriptorSetLayout(dev, &li, nullptr, &_indirectMaterialLayout);
    }

    // VS descriptor set layout (set=0): ViewProj UBO + ObjectData SSBO
    {
        VkDescriptorSetLayoutBinding bs[2]{};
        bs[0] = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,  1, VK_SHADER_STAGE_VERTEX_BIT, nullptr };
        bs[1] = { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  1, VK_SHADER_STAGE_VERTEX_BIT, nullptr };

        VkDescriptorSetLayoutCreateInfo li{};
        li.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 2;
        li.pBindings    = bs;
        vkCreateDescriptorSetLayout(dev, &li, nullptr, &_indirectVSLayout);
    }

    // Cull descriptor set layout (set=0): CullConstants (push) + 4 SSBOs
    {
        VkDescriptorSetLayoutBinding bs[6]{};
        bs[0] = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }; // gObjects
        bs[1] = { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }; // gMeshInfo
        bs[2] = { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }; // gDrawArgs
        bs[3] = { 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }; // gDrawCount
        bs[4] = { 4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }; // hizParams (Phase 23)
        bs[5] = { 5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }; // gHiZ (Phase 23)

        VkDescriptorSetLayoutCreateInfo li{};
        li.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 6;
        li.pBindings    = bs;
        vkCreateDescriptorSetLayout(dev, &li, nullptr, &_vkCullDescLayout);
    }

    // Descriptor pools 
    // Bindless material pool: 1 SSBO + 4*matCount images + 1 sampler
    {
        VkDescriptorPoolSize psz[] = {
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 },
            { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  4 * std::max(matCount, 1u) },
            { VK_DESCRIPTOR_TYPE_SAMPLER,        1 },
        };
        VkDescriptorPoolCreateInfo pi{};
        pi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pi.maxSets       = 1;
        pi.poolSizeCount = 3;
        pi.pPoolSizes    = psz;
        vkCreateDescriptorPool(dev, &pi, nullptr, &_indirectDescPool);
    }

    // VS pool: 1 UBO + 1 SSBO
    {
        VkDescriptorPoolSize psz[] = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 },
        };
        VkDescriptorPoolCreateInfo pi{};
        pi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pi.maxSets       = 1;
        pi.poolSizeCount = 2;
        pi.pPoolSizes    = psz;
        vkCreateDescriptorPool(dev, &pi, nullptr, &_indirectVSDescPool);
    }

    // Cull pool: FRAMES_IN_FLIGHT sets × (4 SSBOs + 1 UBO + 1 sampler) for Phase 23 Hi-Z
    {
        VkDescriptorPoolSize psz[] = {
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         4 * FRAMES_IN_FLIGHT },
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         1 * FRAMES_IN_FLIGHT },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 * FRAMES_IN_FLIGHT },
        };
        VkDescriptorPoolCreateInfo pi{};
        pi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pi.maxSets       = FRAMES_IN_FLIGHT;
        pi.poolSizeCount = 3;
        pi.pPoolSizes    = psz;
        vkCreateDescriptorPool(dev, &pi, nullptr, &_vkCullDescPool);
    }

    // ?�?�?Allocate descriptor sets ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?
    // Bindless material set (fixed-size arrays of matCount each, partially bound)
    {
        VkDescriptorSetAllocateInfo ai{};
        ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool     = _indirectDescPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts        = &_indirectMaterialLayout;
        vkAllocateDescriptorSets(dev, &ai, &_indirectMaterialSet);
    }

    // VS descriptor set
    {
        VkDescriptorSetAllocateInfo ai{};
        ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool     = _indirectVSDescPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts        = &_indirectVSLayout;
        vkAllocateDescriptorSets(dev, &ai, &_indirectVSDescSet);
    }

    // Cull descriptor sets (one per frame)
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
        VkDescriptorSetAllocateInfo ai{};
        ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool     = _vkCullDescPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts        = &_vkCullDescLayout;
        vkAllocateDescriptorSets(dev, &ai, &_vkCullDescSet[i]);
    }

    // ?�?�?ViewProj UBO for VS set ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?
    {
        if (!CreateBuffer(128,
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                _indirectViewProjBuf, _indirectViewProjMem))
            return false;
        vkMapMemory(dev, _indirectViewProjMem, 0, 128, 0, &_indirectViewProjMapped);
    }

    // ?�?�?Write VS descriptor set ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?
    {
        VkDescriptorBufferInfo vpBI{ _indirectViewProjBuf, 0, 128 };
        VkDescriptorBufferInfo objBI{ _objectDataBuffer, 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet ws[2]{};
        ws[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _indirectVSDescSet, 0, 0, 1,
                  VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &vpBI, nullptr };
        ws[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _indirectVSDescSet, 1, 0, 1,
                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &objBI, nullptr };
        vkUpdateDescriptorSets(dev, 2, ws, 0, nullptr);
    }

    // ?�?�?Write bindless material descriptor set ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?
    if (matCount > 0) {
        VkDescriptorBufferInfo matBI{ _matFactorBuffer, 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet matW{};
        matW.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        matW.dstSet          = _indirectMaterialSet;
        matW.dstBinding      = 0;
        matW.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        matW.descriptorCount = 1;
        matW.pBufferInfo     = &matBI;
        vkUpdateDescriptorSets(dev, 1, &matW, 0, nullptr);

        // Per-material texture arrays
        std::vector<VkDescriptorImageInfo> albedoII(matCount), normalII(matCount), mrII(matCount), emissiveII(matCount);
        for (uint32_t i = 0; i < matCount; i++) {
            auto* m = uniqueMats[i];
            VkImageView av = m->albedo.view;
            VkImageView nv = m->normalMap.view    ? m->normalMap.view    : av;
            VkImageView mv = m->metalRough.view   ? m->metalRough.view   : av;
            VkImageView ev = m->emissive.view      ? m->emissive.view     : av;
            albedoII[i]   = { VK_NULL_HANDLE, av, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            normalII[i]   = { VK_NULL_HANDLE, nv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            mrII[i]       = { VK_NULL_HANDLE, mv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            emissiveII[i] = { VK_NULL_HANDLE, ev, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        }
        VkDescriptorImageInfo samplerII{ _linearSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };

        VkWriteDescriptorSet ws[5]{};
        ws[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _indirectMaterialSet, 1, 0,
                  matCount, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, albedoII.data(), nullptr, nullptr };
        ws[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _indirectMaterialSet, 2, 0,
                  matCount, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, normalII.data(), nullptr, nullptr };
        ws[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _indirectMaterialSet, 3, 0,
                  matCount, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, mrII.data(), nullptr, nullptr };
        ws[3] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _indirectMaterialSet, 4, 0,
                  1, VK_DESCRIPTOR_TYPE_SAMPLER, &samplerII, nullptr, nullptr };
        ws[4] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _indirectMaterialSet, 5, 0,
                  matCount, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, emissiveII.data(), nullptr, nullptr };
        vkUpdateDescriptorSets(dev, 5, ws, 0, nullptr);
    }

    // ?�?�?Write cull descriptor sets (per-frame, bind indirectArg + drawCount per frame) ?�?
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
        VkDescriptorBufferInfo objBI { _objectDataBuffer,     0, VK_WHOLE_SIZE };
        VkDescriptorBufferInfo miBI  { _meshInfoBuf,          0, VK_WHOLE_SIZE };
        VkDescriptorBufferInfo argBI { _indirectArgBuffer[i], 0, VK_WHOLE_SIZE };
        VkDescriptorBufferInfo cntBI { _drawCountBuffer[i],   0, sizeof(uint32_t) };

        VkWriteDescriptorSet ws[4]{};
        ws[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _vkCullDescSet[i], 0, 0, 1,
                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &objBI,  nullptr };
        ws[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _vkCullDescSet[i], 1, 0, 1,
                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &miBI,  nullptr };
        ws[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _vkCullDescSet[i], 2, 0, 1,
                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &argBI,  nullptr };
        ws[3] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _vkCullDescSet[i], 3, 0, 1,
                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &cntBI,  nullptr };
        vkUpdateDescriptorSets(dev, 4, ws, 0, nullptr);
    }

    // ?�?�?Pipeline layouts ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?
    // Indirect G-buffer pipeline layout (set=0: VS layout, set=1: bindless material)
    {
        VkDescriptorSetLayout dsl[] = { _indirectVSLayout, _indirectMaterialLayout };
        VkPipelineLayoutCreateInfo pli{};
        pli.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount         = 2;
        pli.pSetLayouts            = dsl;
        vkCreatePipelineLayout(dev, &pli, nullptr, &_indirectPipeLayout);
    }

    // Cull pipeline layout (set=0: cull desc, push constant: CullConstants 112B)
    {
        VkPushConstantRange pcr{};
        pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pcr.offset     = 0;
        pcr.size       = 128;  // 6×float4 frustumPlanes + uint objectCount + Hi-Z params + projParams
        VkPipelineLayoutCreateInfo pli{};
        pli.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount         = 1;
        pli.pSetLayouts            = &_vkCullDescLayout;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges    = &pcr;
        vkCreatePipelineLayout(dev, &pli, nullptr, &_vkCullPipeLayout);
    }

    // ?�?�?GPU cull compute pipeline ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?
    {
        std::vector<uint32_t> csS;
        if (!CompileGLSLtoSPIRV(GetShaderFullPath(L"gpu_cull_vk.comp.glsl").wstring(), csS))
        { LUNA_LOG_ERROR("VK: gpu_cull_vk compile failed"); return false; }

        VkShaderModuleCreateInfo smi{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        smi.codeSize = csS.size() * 4; smi.pCode = csS.data();
        VkShaderModule csM = VK_NULL_HANDLE;
        vkCreateShaderModule(dev, &smi, nullptr, &csM);

        VkComputePipelineCreateInfo cpi{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        cpi.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpi.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        cpi.stage.module = csM;
        cpi.stage.pName  = "main";
        cpi.layout       = _vkCullPipeLayout;
        vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &_vkCullPipeline);
        vkDestroyShaderModule(dev, csM, nullptr);
    }

    // ?�?�?Indirect G-buffer graphics pipeline ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?
    {
        std::vector<uint32_t> vsS, fsS;
        if (!CompileGLSLtoSPIRV(GetShaderFullPath(L"pbr_indirect_vk.vert.glsl").wstring(),     vsS) ||
            !CompileGLSLtoSPIRV(GetShaderFullPath(L"gbuffer_indirect_vk.frag.glsl").wstring(), fsS))
        { LUNA_LOG_ERROR("VK: indirect G-buffer shaders compile failed"); return false; }

        auto mkM = [&](const std::vector<uint32_t>& sp) {
            VkShaderModuleCreateInfo si{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
            si.codeSize = sp.size() * 4; si.pCode = sp.data();
            VkShaderModule m = VK_NULL_HANDLE;
            vkCreateShaderModule(dev, &si, nullptr, &m);
            return m;
        };
        VkShaderModule vsM = mkM(vsS), fsM = mkM(fsS);

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,   vsM, "main" };
        stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fsM, "main" };

        // PBRVertex: pos(12)+normal(12)+uv(8)+tangent(16) = stride 48
        VkVertexInputBindingDescription vbd{ 0, 48, VK_VERTEX_INPUT_RATE_VERTEX };
        VkVertexInputAttributeDescription vad[4]{};
        vad[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT,    0  };
        vad[1] = { 1, 0, VK_FORMAT_R32G32B32_SFLOAT,    12 };
        vad[2] = { 2, 0, VK_FORMAT_R32G32_SFLOAT,       24 };
        vad[3] = { 3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 32 };

        VkPipelineVertexInputStateCreateInfo vis{};
        vis.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vis.vertexBindingDescriptionCount   = 1;
        vis.pVertexBindingDescriptions      = &vbd;
        vis.vertexAttributeDescriptionCount = 4;
        vis.pVertexAttributeDescriptions    = vad;

        VkPipelineInputAssemblyStateCreateInfo ias{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        ias.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo vps{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        vps.viewportCount = 1; vps.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode    = VK_CULL_MODE_NONE;
        rs.frontFace   = VK_FRONT_FACE_CLOCKWISE;
        rs.lineWidth   = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo dss{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        dss.depthTestEnable  = VK_TRUE;
        dss.depthWriteEnable = VK_TRUE;
        dss.depthCompareOp   = VK_COMPARE_OP_LESS;

        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendAttachmentState cbas[3] = { cba, cba, cba };

        VkPipelineColorBlendStateCreateInfo cbs{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cbs.attachmentCount = 3; cbs.pAttachments = cbas;

        VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dsi{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dsi.dynamicStateCount = 2; dsi.pDynamicStates = dyn;

        VkGraphicsPipelineCreateInfo gpi{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        gpi.stageCount          = 2;
        gpi.pStages             = stages;
        gpi.pVertexInputState   = &vis;
        gpi.pInputAssemblyState = &ias;
        gpi.pViewportState      = &vps;
        gpi.pRasterizationState = &rs;
        gpi.pMultisampleState   = &ms;
        gpi.pDepthStencilState  = &dss;
        gpi.pColorBlendState    = &cbs;
        gpi.pDynamicState       = &dsi;
        gpi.layout              = _indirectPipeLayout;
        gpi.renderPass          = _gBuffer.GetRenderPassLoad();   // re-opened pass (LOAD_OP_LOAD)
        gpi.subpass             = 0;

        VkResult r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, nullptr, &_indirectGBufPipeline);
        vkDestroyShaderModule(dev, vsM, nullptr);
        vkDestroyShaderModule(dev, fsM, nullptr);
        if (r != VK_SUCCESS) { LUNA_LOG_ERROR("VK: indirect G-buffer pipeline failed: %d", (int)r); return false; }
    }

    _gpuDrivenReady = true;
    return true;
}

void VulkanBackend::DestroyIndirectResources()
{
    VkDevice dev = _device->GetDevice();
    _gpuDrivenReady = false;
    _cpuInstances.clear();

    if (_vkCullPipeline)         { vkDestroyPipeline(dev, _vkCullPipeline, nullptr);            _vkCullPipeline   = VK_NULL_HANDLE; }
    if (_vkCullPipeLayout)       { vkDestroyPipelineLayout(dev, _vkCullPipeLayout, nullptr);    _vkCullPipeLayout = VK_NULL_HANDLE; }
    if (_indirectGBufPipeline)   { vkDestroyPipeline(dev, _indirectGBufPipeline, nullptr);      _indirectGBufPipeline = VK_NULL_HANDLE; }
    if (_indirectPipeLayout)     { vkDestroyPipelineLayout(dev, _indirectPipeLayout, nullptr);  _indirectPipeLayout   = VK_NULL_HANDLE; }

    if (_vkCullDescPool)         { vkDestroyDescriptorPool(dev, _vkCullDescPool, nullptr);       _vkCullDescPool   = VK_NULL_HANDLE; }
    if (_vkCullDescLayout)       { vkDestroyDescriptorSetLayout(dev, _vkCullDescLayout, nullptr); _vkCullDescLayout = VK_NULL_HANDLE; }
    if (_indirectDescPool)       { vkDestroyDescriptorPool(dev, _indirectDescPool, nullptr);      _indirectDescPool = VK_NULL_HANDLE; }
    if (_indirectMaterialLayout) { vkDestroyDescriptorSetLayout(dev, _indirectMaterialLayout, nullptr); _indirectMaterialLayout = VK_NULL_HANDLE; }
    if (_indirectVSDescPool)     { vkDestroyDescriptorPool(dev, _indirectVSDescPool, nullptr);    _indirectVSDescPool = VK_NULL_HANDLE; }
    if (_indirectVSLayout)       { vkDestroyDescriptorSetLayout(dev, _indirectVSLayout, nullptr); _indirectVSLayout   = VK_NULL_HANDLE; }

    if (_indirectViewProjMapped) { vkUnmapMemory(dev, _indirectViewProjMem);                     _indirectViewProjMapped = nullptr; }
    if (_indirectViewProjBuf)    { vkDestroyBuffer(dev, _indirectViewProjBuf, nullptr);           _indirectViewProjBuf    = VK_NULL_HANDLE; }
    if (_indirectViewProjMem)    { vkFreeMemory(dev, _indirectViewProjMem, nullptr);              _indirectViewProjMem    = VK_NULL_HANDLE; }

    if (_objectDataMapped)       { vkUnmapMemory(dev, _objectDataMem);                            _objectDataMapped = nullptr; }
    if (_objectDataBuffer)       { vkDestroyBuffer(dev, _objectDataBuffer, nullptr);               _objectDataBuffer = VK_NULL_HANDLE; }
    if (_objectDataMem)          { vkFreeMemory(dev, _objectDataMem, nullptr);                     _objectDataMem    = VK_NULL_HANDLE; }

    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
        if (_indirectArgBuffer[i]) { vkDestroyBuffer(dev, _indirectArgBuffer[i], nullptr); _indirectArgBuffer[i] = VK_NULL_HANDLE; }
        if (_indirectArgMem[i])    { vkFreeMemory(dev, _indirectArgMem[i], nullptr);        _indirectArgMem[i]    = VK_NULL_HANDLE; }
        if (_drawCountBuffer[i])   { vkDestroyBuffer(dev, _drawCountBuffer[i], nullptr);   _drawCountBuffer[i]   = VK_NULL_HANDLE; }
        if (_drawCountMem[i])      { vkFreeMemory(dev, _drawCountMem[i], nullptr);          _drawCountMem[i]      = VK_NULL_HANDLE; }
    }

    if (_matFactorBuffer) { vkDestroyBuffer(dev, _matFactorBuffer, nullptr);  _matFactorBuffer = VK_NULL_HANDLE; }
    if (_matFactorMem)    { vkFreeMemory(dev, _matFactorMem, nullptr);         _matFactorMem    = VK_NULL_HANDLE; }

    if (_mergedVB)    { vkDestroyBuffer(dev, _mergedVB, nullptr);    _mergedVB    = VK_NULL_HANDLE; }
    if (_mergedVBMem) { vkFreeMemory(dev, _mergedVBMem, nullptr);     _mergedVBMem = VK_NULL_HANDLE; }
    if (_mergedIB)    { vkDestroyBuffer(dev, _mergedIB, nullptr);    _mergedIB    = VK_NULL_HANDLE; }
    if (_mergedIBMem) { vkFreeMemory(dev, _mergedIBMem, nullptr);     _mergedIBMem = VK_NULL_HANDLE; }
    if (_meshInfoBuf) { vkDestroyBuffer(dev, _meshInfoBuf, nullptr); _meshInfoBuf = VK_NULL_HANDLE; }
    if (_meshInfoMem) { vkFreeMemory(dev, _meshInfoMem, nullptr);     _meshInfoMem = VK_NULL_HANDLE; }

    DestroyAsyncComputeResources();  // Phase 20
}

// ===========================================================================
// Phase 20: Vulkan Async Compute
// ===========================================================================

bool VulkanBackend::CreateAsyncComputeResources()
{
    if (!_device->IsAsyncComputeSupported()) return false;
    VkDevice dev = _device->GetDevice();

    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++)
    {
        auto& cf = _computeFrames[i];

        // Command pool (compute queue family)
        VkCommandPoolCreateInfo poolCI{};
        poolCI.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolCI.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolCI.queueFamilyIndex = _device->GetComputeQueueFamily();
        if (vkCreateCommandPool(dev, &poolCI, nullptr, &cf.cmdPool) != VK_SUCCESS)
        {
            LUNA_LOG_WARN("Phase 20: Failed to create compute command pool [%u]", i);
            return false;
        }

        // Command buffer
        VkCommandBufferAllocateInfo allocCI{};
        allocCI.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocCI.commandPool        = cf.cmdPool;
        allocCI.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocCI.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(dev, &allocCI, &cf.cmdBuffer) != VK_SUCCESS)
        {
            LUNA_LOG_WARN("Phase 20: Failed to allocate compute command buffer [%u]", i);
            return false;
        }

        // Semaphore (compute done ??graphics wait)
        VkSemaphoreCreateInfo semCI{};
        semCI.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (vkCreateSemaphore(dev, &semCI, nullptr, &cf.doneSemaphore) != VK_SUCCESS)
        {
            LUNA_LOG_WARN("Phase 20: Failed to create compute semaphore [%u]", i);
            return false;
        }

        // Fence (CPU wait, initially signaled so first WaitForFences succeeds)
        VkFenceCreateInfo fenceCI{};
        fenceCI.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceCI.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (vkCreateFence(dev, &fenceCI, nullptr, &cf.fence) != VK_SUCCESS)
        {
            LUNA_LOG_WARN("Phase 20: Failed to create compute fence [%u]", i);
            return false;
        }
    }

    // DEBUGGING: Disable async compute to test sync issues
    _asyncComputeReady = false;  // was: true
    LUNA_LOG_INFO("Phase 20: Vulkan async compute resources created (DISABLED FOR DEBUG)");
    return true;
}

void VulkanBackend::DestroyAsyncComputeResources()
{
    if (!_device || _device->GetDevice() == VK_NULL_HANDLE) return;
    VkDevice dev = _device->GetDevice();
    _asyncComputeReady = false;

    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++)
    {
        auto& cf = _computeFrames[i];
        if (cf.fence)         { vkDestroyFence(dev, cf.fence, nullptr);           cf.fence         = VK_NULL_HANDLE; }
        if (cf.doneSemaphore) { vkDestroySemaphore(dev, cf.doneSemaphore, nullptr); cf.doneSemaphore = VK_NULL_HANDLE; }
        if (cf.cmdPool)       { vkDestroyCommandPool(dev, cf.cmdPool, nullptr);   cf.cmdPool       = VK_NULL_HANDLE; }
        cf.cmdBuffer = VK_NULL_HANDLE;  // freed with pool
    }
}

void VulkanBackend::DispatchCullAsync()
{
    uint32_t count = (uint32_t)_cpuInstances.size();
    auto& cf = _computeFrames[_frameIndex];
    VkDevice dev = _device->GetDevice();

    uint32_t computeFamily  = _device->GetComputeQueueFamily();
    uint32_t graphicsFamily = _device->GetGraphicsQueueFamily();

    // Wait for previous compute work on this frame slot
    vkWaitForFences(dev, 1, &cf.fence, VK_TRUE, UINT64_MAX);
    vkResetFences(dev, 1, &cf.fence);

    // Reset and begin compute command buffer
    vkResetCommandBuffer(cf.cmdBuffer, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cf.cmdBuffer, &bi);

    VkCommandBuffer cmd = cf.cmdBuffer;

    // Upload object data (HOST_COHERENT ??no barrier needed)
    memcpy(_objectDataMapped, _cpuInstances.data(), count * sizeof(GPUObjectDataVK));

    // Clear draw count
    vkCmdFillBuffer(cmd, _drawCountBuffer[_frameIndex], 0, sizeof(uint32_t), 0);

    // Barrier: TRANSFER_WRITE ??COMPUTE_SHADER_READ (drawCount)
    {
        VkBufferMemoryBarrier bmb{};
        bmb.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        bmb.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        bmb.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        bmb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bmb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bmb.buffer              = _drawCountBuffer[_frameIndex];
        bmb.offset              = 0;
        bmb.size                = sizeof(uint32_t);
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 1, &bmb, 0, nullptr);
    }

    // GPU frustum + occlusion cull dispatch
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _vkCullPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _vkCullPipeLayout,
            0, 1, &_vkCullDescSet[_frameIndex], 0, nullptr);

        // Build frustum planes from ViewProj (push constants: 6 planes + objectCount)
        XMMATRIX V  = XMLoadFloat4x4(&_deferredView);
        XMMATRIX P  = XMLoadFloat4x4(&_deferredProj);
        XMMATRIX VP = XMMatrixMultiply(V, P);
        struct CullConstants
        {
            float frustumPlanes[6][4];  // 96 B
            uint32_t objectCount;
            uint32_t enableHiZ;
            uint32_t hizMipCount;
            uint32_t _pad0;
            float    projParams[4];
        } cc{};                         // total 128 B

        XMMATRIX T = XMMatrixTranspose(VP);
        XMFLOAT4X4 tm; XMStoreFloat4x4(&tm, T);

        // Gribb-Hartmann frustum plane extraction
        cc.frustumPlanes[0][0] = tm.m[3][0] + tm.m[0][0]; cc.frustumPlanes[0][1] = tm.m[3][1] + tm.m[0][1];
        cc.frustumPlanes[0][2] = tm.m[3][2] + tm.m[0][2]; cc.frustumPlanes[0][3] = tm.m[3][3] + tm.m[0][3];
        cc.frustumPlanes[1][0] = tm.m[3][0] - tm.m[0][0]; cc.frustumPlanes[1][1] = tm.m[3][1] - tm.m[0][1];
        cc.frustumPlanes[1][2] = tm.m[3][2] - tm.m[0][2]; cc.frustumPlanes[1][3] = tm.m[3][3] - tm.m[0][3];
        cc.frustumPlanes[2][0] = tm.m[3][0] + tm.m[1][0]; cc.frustumPlanes[2][1] = tm.m[3][1] + tm.m[1][1];
        cc.frustumPlanes[2][2] = tm.m[3][2] + tm.m[1][2]; cc.frustumPlanes[2][3] = tm.m[3][3] + tm.m[1][3];
        cc.frustumPlanes[3][0] = tm.m[3][0] - tm.m[1][0]; cc.frustumPlanes[3][1] = tm.m[3][1] - tm.m[1][1];
        cc.frustumPlanes[3][2] = tm.m[3][2] - tm.m[1][2]; cc.frustumPlanes[3][3] = tm.m[3][3] - tm.m[1][3];
        cc.frustumPlanes[4][0] = tm.m[2][0]; cc.frustumPlanes[4][1] = tm.m[2][1];
        cc.frustumPlanes[4][2] = tm.m[2][2]; cc.frustumPlanes[4][3] = tm.m[2][3];
        cc.frustumPlanes[5][0] = tm.m[3][0] - tm.m[2][0]; cc.frustumPlanes[5][1] = tm.m[3][1] - tm.m[2][1];
        cc.frustumPlanes[5][2] = tm.m[3][2] - tm.m[2][2]; cc.frustumPlanes[5][3] = tm.m[3][3] - tm.m[2][3];
        cc.objectCount = count;

        cc.enableHiZ   = _hiZ.IsReady() ? 1u : 0u;
        cc.hizMipCount = _hiZ.GetMipCount();
        {
            XMFLOAT4X4 projF;
            XMStoreFloat4x4(&projF, P);
            cc.projParams[0] = projF._11;
            cc.projParams[1] = projF._22;
            cc.projParams[2] = projF._33;
            cc.projParams[3] = projF._43;
        }

        vkCmdPushConstants(cmd, _vkCullPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 128, &cc);
        vkCmdDispatch(cmd, (count + 63) / 64, 1, 1);
    }

    // Queue ownership release barriers (compute ??graphics)
    // If same family, use IGNORED (no ownership transfer needed, just execution barrier)
    {
        uint32_t srcFamily = (computeFamily != graphicsFamily) ? computeFamily  : VK_QUEUE_FAMILY_IGNORED;
        uint32_t dstFamily = (computeFamily != graphicsFamily) ? graphicsFamily : VK_QUEUE_FAMILY_IGNORED;

        VkBufferMemoryBarrier bmbs[2]{};
        bmbs[0].sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        bmbs[0].srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
        bmbs[0].dstAccessMask       = 0;  // will be acquired on graphics side
        bmbs[0].srcQueueFamilyIndex = srcFamily;
        bmbs[0].dstQueueFamilyIndex = dstFamily;
        bmbs[0].buffer              = _indirectArgBuffer[_frameIndex];
        bmbs[0].offset              = 0;
        bmbs[0].size                = VK_WHOLE_SIZE;
        bmbs[1] = bmbs[0];
        bmbs[1].buffer = _drawCountBuffer[_frameIndex];
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0, 0, nullptr, 2, bmbs, 0, nullptr);
    }

    vkEndCommandBuffer(cmd);

    // Submit to compute queue, signal semaphore + fence
    VkSubmitInfo si{};
    si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount   = 1;
    si.pCommandBuffers      = &cf.cmdBuffer;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores    = &cf.doneSemaphore;
    vkQueueSubmit(_device->GetComputeQueue(), 1, &si, cf.fence);

    _computeSubmittedThisFrame = true;
}

// ===========================================================================
void VulkanBackend::FlushDraws()
{
    if (!_gpuDrivenReady || _cpuInstances.empty()) return;

    auto& frame = _frames[_frameIndex];
    VkCommandBuffer cmd = frame.cmdBuffer;

    uint32_t count = (uint32_t)_cpuInstances.size();

    // Update ViewProj UBO for indirect VS
    {
        struct VP { float view[16]; float proj[16]; };
        VP vp{};
        memcpy(vp.view, &_deferredView, 64);
        // Use jittered projection for G-buffer (matches _lastProj used by legacy path)
        memcpy(vp.proj, &_lastProj, 64);
        memcpy(_indirectViewProjMapped, &vp, 128);
    }

    // 1. End G-buffer render pass (opened in BeginFrame)
    vkCmdEndRenderPass(cmd);
    _gpuProfiler.WriteEndTimestamp(cmd); // end "GBuffer Fill"

    // Phase 27: Mesh shader path — per-object DispatchMeshTasks with task+mesh culling
    if (_meshShaderReady && _meshShaderPipeline && pfn_vkCmdDrawMeshTasksEXT)
    {
        _gpuProfiler.WriteBeginTimestamp(cmd, "GPU Cull");

        // 1b. Transition G-buffer images for re-use
        {
            VkImageMemoryBarrier bars[4]{};
            for (int i = 0; i < 3; i++) {
                bars[i].sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                bars[i].srcAccessMask    = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                bars[i].dstAccessMask    = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                bars[i].oldLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                bars[i].newLayout        = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                bars[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                bars[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                bars[i].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            }
            bars[0].image = _gBuffer.GetAlbedoImage();
            bars[1].image = _gBuffer.GetNormalImage();
            bars[2].image = _gBuffer.GetMetalRoughImage();
            bars[3].sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            bars[3].srcAccessMask    = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            bars[3].dstAccessMask    = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            bars[3].oldLayout        = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            bars[3].newLayout        = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            bars[3].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bars[3].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bars[3].subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
            bars[3].image            = _vkSwapchain.GetDepthImage();
            vkCmdPipelineBarrier(cmd,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                0, 0, nullptr, 0, nullptr, 4, bars);
        }
        _gpuProfiler.WriteEndTimestamp(cmd); // end "GPU Cull" (minimal — cull is inline in task shader)

        // Re-open G-buffer render pass with LOAD_OP_LOAD
        {
            VkRenderPassBeginInfo rpi{};
            rpi.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            rpi.renderPass        = _gBuffer.GetRenderPassLoad();
            rpi.framebuffer       = _gBuffer.GetFramebufferLoad();
            rpi.renderArea.extent = _vkSwapchain.GetExtent();
            vkCmdBeginRenderPass(cmd, &rpi, VK_SUBPASS_CONTENTS_INLINE);
            VkViewport viewport{ 0, 0, (float)_vkSwapchain.GetExtent().width,
                                 (float)_vkSwapchain.GetExtent().height, 0, 1 };
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            VkRect2D scissor{ {0,0}, _vkSwapchain.GetExtent() };
            vkCmdSetScissor(cmd, 0, 1, &scissor);
        }

        // Upload object data
        memcpy(_objectDataMapped, _cpuInstances.data(), count * sizeof(GPUObjectDataVK));

        // Build frustum planes
        XMMATRIX V = XMLoadFloat4x4(&_deferredView);
        XMMATRIX P = XMLoadFloat4x4(&_lastProj);
        XMMATRIX VP = XMMatrixMultiply(V, P);
        XMFLOAT4X4 vpMat; XMStoreFloat4x4(&vpMat, VP);

        // Extract frustum planes from VP matrix (row-major)
        struct { float x, y, z, w; } frustumPlanes[6];
        frustumPlanes[0] = { vpMat._14 + vpMat._11, vpMat._24 + vpMat._21, vpMat._34 + vpMat._31, vpMat._44 + vpMat._41 };
        frustumPlanes[1] = { vpMat._14 - vpMat._11, vpMat._24 - vpMat._21, vpMat._34 - vpMat._31, vpMat._44 - vpMat._41 };
        frustumPlanes[2] = { vpMat._14 + vpMat._12, vpMat._24 + vpMat._22, vpMat._34 + vpMat._32, vpMat._44 + vpMat._42 };
        frustumPlanes[3] = { vpMat._14 - vpMat._12, vpMat._24 - vpMat._22, vpMat._34 - vpMat._32, vpMat._44 - vpMat._42 };
        frustumPlanes[4] = { vpMat._13, vpMat._23, vpMat._33, vpMat._43 };
        frustumPlanes[5] = { vpMat._14 - vpMat._13, vpMat._24 - vpMat._23, vpMat._34 - vpMat._33, vpMat._44 - vpMat._43 };
        for (int i = 0; i < 6; ++i) {
            float len = std::sqrt(frustumPlanes[i].x*frustumPlanes[i].x + frustumPlanes[i].y*frustumPlanes[i].y + frustumPlanes[i].z*frustumPlanes[i].z);
            if (len > 0.0001f) { float inv = 1.0f/len; frustumPlanes[i].x *= inv; frustumPlanes[i].y *= inv; frustumPlanes[i].z *= inv; frustumPlanes[i].w *= inv; }
        }

        // Bind mesh shader pipeline + descriptor sets
        _gpuProfiler.WriteBeginTimestamp(cmd, "Mesh Shader Draw");
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _meshShaderPipeline);
        VkDescriptorSet dSets[] = { _meshShaderDescSet, _indirectMaterialSet };
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _meshShaderPipeLayout,
            0, 2, dSets, 0, nullptr);

        // Per-object mesh task dispatch
        // Push constant layout: mat4 view(64) + mat4 proj(64) + vec4 planes[6](96) + 4*uint(16) = 240B
        struct MeshShaderPC {
            float viewMatrix[16];       // 64
            float projMatrix[16];       // 64
            float frustumPlanes[6][4];  // 96
            uint32_t objectIndex;       //  4
            uint32_t meshletOffset;     //  4
            uint32_t meshletCount;      //  4
            uint32_t _pad;              //  4 = 240
        } pc{};
        memcpy(pc.viewMatrix, &_deferredView, 64);
        memcpy(pc.projMatrix, &_lastProj, 64);
        memcpy(pc.frustumPlanes, frustumPlanes, 96);

        for (uint32_t obj = 0; obj < count; ++obj)
        {
            const GPUObjectDataVK& inst = _cpuInstances[obj];
            uint32_t meshIdx = inst.meshIndex;
            if (meshIdx >= _vkMeshMeshletOffsets.size()) continue;

            uint32_t meshletOff   = _vkMeshMeshletOffsets[meshIdx];
            uint32_t meshletCount = _vkMeshMeshletCounts[meshIdx];
            if (meshletCount == 0) continue;

            pc.objectIndex   = obj;
            pc.meshletOffset = meshletOff;
            pc.meshletCount  = meshletCount;

            vkCmdPushConstants(cmd, _meshShaderPipeLayout,
                VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT,
                0, 240, &pc);

            uint32_t groupCount = (meshletCount + 31) / 32;
            pfn_vkCmdDrawMeshTasksEXT(cmd, groupCount, 1, 1);
        }

        _gpuProfiler.WriteEndTimestamp(cmd); // end "Mesh Shader Draw"
        _cpuInstances.clear();
        return;
    }

    // Fallback: compute cull + indirect draw path (Phase 15B/20)
    _gpuProfiler.WriteBeginTimestamp(cmd, "GPU Cull");

    // 1b. Transition G-buffer images: SHADER_READ_ONLY ??COLOR_ATTACHMENT (color),
    //     DEPTH_STENCIL_READ_ONLY ??DEPTH_STENCIL_ATTACHMENT (depth).
    {
        VkImageMemoryBarrier bars[4]{};
        for (int i = 0; i < 3; i++) {
            bars[i].sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            bars[i].srcAccessMask    = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            bars[i].dstAccessMask    = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
                                     | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            bars[i].oldLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            bars[i].newLayout        = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            bars[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bars[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bars[i].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        }
        bars[0].image = _gBuffer.GetAlbedoImage();
        bars[1].image = _gBuffer.GetNormalImage();
        bars[2].image = _gBuffer.GetMetalRoughImage();
        bars[3].sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        bars[3].srcAccessMask    = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        bars[3].dstAccessMask    = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                                 | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        bars[3].oldLayout        = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        bars[3].newLayout        = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        bars[3].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bars[3].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bars[3].subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
        bars[3].image            = _vkSwapchain.GetDepthImage();

        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
            0, 0, nullptr, 0, nullptr, 4, bars);
    }

    // Phase 23: Build Hi-Z pyramid from previous frame's depth (stays on graphics queue)
    if (_hiZ.GetMipCount() >= 2)
    {
        _gpuProfiler.WriteBeginTimestamp(cmd, "Hi-Z Build");
        _hiZ.BuildPyramid(cmd, _vkSwapchain.GetDepthImage(), _vkSwapchain.GetExtent());
        _gpuProfiler.WriteEndTimestamp(cmd);

        struct HiZParams { float viewProj[16]; float screenW; float screenH; float hizMipCount; float pad; };
        HiZParams hp{};
        XMMATRIX V  = XMLoadFloat4x4(&_deferredView);
        XMMATRIX P  = XMLoadFloat4x4(&_deferredProj);
        XMMATRIX VP = XMMatrixMultiply(V, P);
        XMFLOAT4X4 vpF; XMStoreFloat4x4(&vpF, VP);
        memcpy(hp.viewProj, &vpF, 64);
        hp.screenW     = (float)_vkSwapchain.GetExtent().width;
        hp.screenH     = (float)_vkSwapchain.GetExtent().height;
        hp.hizMipCount = (float)_hiZ.GetMipCount();
        memcpy(_hiZ.GetParamsMapped(), &hp, sizeof(hp));
    }

    // Phase 20: Async compute path ??dispatch cull on dedicated compute queue
    if (_asyncComputeReady)
    {
        DispatchCullAsync();

        // Queue ownership acquire barriers on graphics queue (compute ??graphics)
        uint32_t computeFamily  = _device->GetComputeQueueFamily();
        uint32_t graphicsFamily = _device->GetGraphicsQueueFamily();
        uint32_t srcFamily = (computeFamily != graphicsFamily) ? computeFamily  : VK_QUEUE_FAMILY_IGNORED;
        uint32_t dstFamily = (computeFamily != graphicsFamily) ? graphicsFamily : VK_QUEUE_FAMILY_IGNORED;

        VkBufferMemoryBarrier bmbs[2]{};
        bmbs[0].sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        bmbs[0].srcAccessMask       = 0;
        bmbs[0].dstAccessMask       = VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
        bmbs[0].srcQueueFamilyIndex = srcFamily;
        bmbs[0].dstQueueFamilyIndex = dstFamily;
        bmbs[0].buffer              = _indirectArgBuffer[_frameIndex];
        bmbs[0].offset              = 0;
        bmbs[0].size                = VK_WHOLE_SIZE;
        bmbs[1] = bmbs[0];
        bmbs[1].buffer = _drawCountBuffer[_frameIndex];
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,  // wait stage: before indirect draw
            VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
            0, 0, nullptr, 2, bmbs, 0, nullptr);
    }
    else
    {
        // Fallback: dispatch cull on graphics queue (single-queue path)

        // Upload object data (HOST_COHERENT ??no barrier needed)
        memcpy(_objectDataMapped, _cpuInstances.data(), count * sizeof(GPUObjectDataVK));

        // Clear draw count
        vkCmdFillBuffer(cmd, _drawCountBuffer[_frameIndex], 0, sizeof(uint32_t), 0);

        // Barrier: TRANSFER_WRITE ??COMPUTE_SHADER_READ (drawCount)
        {
            VkBufferMemoryBarrier bmb{};
            bmb.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            bmb.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
            bmb.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            bmb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bmb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bmb.buffer              = _drawCountBuffer[_frameIndex];
            bmb.offset              = 0;
            bmb.size                = sizeof(uint32_t);
            vkCmdPipelineBarrier(cmd,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0, 0, nullptr, 1, &bmb, 0, nullptr);
        }

        // GPU frustum + occlusion cull dispatch
        {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _vkCullPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _vkCullPipeLayout,
                0, 1, &_vkCullDescSet[_frameIndex], 0, nullptr);

            XMMATRIX V  = XMLoadFloat4x4(&_deferredView);
            XMMATRIX P  = XMLoadFloat4x4(&_deferredProj);
            XMMATRIX VP = XMMatrixMultiply(V, P);
            struct CullConstants
            {
                float frustumPlanes[6][4];
                uint32_t objectCount;
                uint32_t enableHiZ;
                uint32_t hizMipCount;
                uint32_t _pad0;
                float    projParams[4];
            } cc{};

            XMMATRIX T = XMMatrixTranspose(VP);
            XMFLOAT4X4 tm; XMStoreFloat4x4(&tm, T);

            cc.frustumPlanes[0][0] = tm.m[3][0] + tm.m[0][0]; cc.frustumPlanes[0][1] = tm.m[3][1] + tm.m[0][1];
            cc.frustumPlanes[0][2] = tm.m[3][2] + tm.m[0][2]; cc.frustumPlanes[0][3] = tm.m[3][3] + tm.m[0][3];
            cc.frustumPlanes[1][0] = tm.m[3][0] - tm.m[0][0]; cc.frustumPlanes[1][1] = tm.m[3][1] - tm.m[0][1];
            cc.frustumPlanes[1][2] = tm.m[3][2] - tm.m[0][2]; cc.frustumPlanes[1][3] = tm.m[3][3] - tm.m[0][3];
            cc.frustumPlanes[2][0] = tm.m[3][0] + tm.m[1][0]; cc.frustumPlanes[2][1] = tm.m[3][1] + tm.m[1][1];
            cc.frustumPlanes[2][2] = tm.m[3][2] + tm.m[1][2]; cc.frustumPlanes[2][3] = tm.m[3][3] + tm.m[1][3];
            cc.frustumPlanes[3][0] = tm.m[3][0] - tm.m[1][0]; cc.frustumPlanes[3][1] = tm.m[3][1] - tm.m[1][1];
            cc.frustumPlanes[3][2] = tm.m[3][2] - tm.m[1][2]; cc.frustumPlanes[3][3] = tm.m[3][3] - tm.m[1][3];
            cc.frustumPlanes[4][0] = tm.m[2][0]; cc.frustumPlanes[4][1] = tm.m[2][1];
            cc.frustumPlanes[4][2] = tm.m[2][2]; cc.frustumPlanes[4][3] = tm.m[2][3];
            cc.frustumPlanes[5][0] = tm.m[3][0] - tm.m[2][0]; cc.frustumPlanes[5][1] = tm.m[3][1] - tm.m[2][1];
            cc.frustumPlanes[5][2] = tm.m[3][2] - tm.m[2][2]; cc.frustumPlanes[5][3] = tm.m[3][3] - tm.m[2][3];
            cc.objectCount = count;

            cc.enableHiZ   = _hiZ.IsReady() ? 1u : 0u;
            cc.hizMipCount = _hiZ.GetMipCount();
            {
                XMFLOAT4X4 projF;
                XMStoreFloat4x4(&projF, P);
                cc.projParams[0] = projF._11;
                cc.projParams[1] = projF._22;
                cc.projParams[2] = projF._33;
                cc.projParams[3] = projF._43;
            }

            vkCmdPushConstants(cmd, _vkCullPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 128, &cc);
            vkCmdDispatch(cmd, (count + 63) / 64, 1, 1);
        }

        // Barrier: COMPUTE SHADER_WRITE ??DRAW_INDIRECT + VERTEX_INPUT read
        {
            VkBufferMemoryBarrier bmbs[2]{};
            bmbs[0].sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            bmbs[0].srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
            bmbs[0].dstAccessMask       = VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
            bmbs[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bmbs[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bmbs[0].buffer              = _indirectArgBuffer[_frameIndex];
            bmbs[0].offset              = 0;
            bmbs[0].size                = VK_WHOLE_SIZE;
            bmbs[1] = bmbs[0];
            bmbs[1].buffer = _drawCountBuffer[_frameIndex];
            vkCmdPipelineBarrier(cmd,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                0, 0, nullptr, 2, bmbs, 0, nullptr);
        }
    }
    _gpuProfiler.WriteEndTimestamp(cmd); // end "GPU Cull"

    // 7. Re-open G-buffer render pass with LOAD_OP_LOAD
    {
        VkRenderPassBeginInfo rpi{};
        rpi.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpi.renderPass        = _gBuffer.GetRenderPassLoad();
        rpi.framebuffer       = _gBuffer.GetFramebufferLoad();
        rpi.renderArea.extent = _vkSwapchain.GetExtent();
        rpi.clearValueCount   = 0;  // LOAD_OP_LOAD ??no clears needed
        vkCmdBeginRenderPass(cmd, &rpi, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport vp{ 0, 0,
            (float)_vkSwapchain.GetExtent().width, (float)_vkSwapchain.GetExtent().height, 0, 1 };
        vkCmdSetViewport(cmd, 0, 1, &vp);
        VkRect2D sc{ {0,0}, _vkSwapchain.GetExtent() };
        vkCmdSetScissor(cmd, 0, 1, &sc);
    }

    // 8. Indirect G-buffer draw
    _gpuProfiler.WriteBeginTimestamp(cmd, "Indirect Draw");
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _indirectGBufPipeline);
        VkDescriptorSet dSets[] = { _indirectVSDescSet, _indirectMaterialSet };
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _indirectPipeLayout,
            0, 2, dSets, 0, nullptr);

        VkDeviceSize off = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &_mergedVB, &off);
        vkCmdBindIndexBuffer(cmd, _mergedIB, 0, VK_INDEX_TYPE_UINT32);

        // vkCmdDrawIndexedIndirectCount: draw up to MAX_GPU_OBJECTS with GPU draw count
        vkCmdDrawIndexedIndirectCount(cmd,
            _indirectArgBuffer[_frameIndex], 0,
            _drawCountBuffer[_frameIndex],   0,
            MAX_GPU_OBJECTS,
            sizeof(VkDrawIndexedIndirectCmd));
    }

    // Render pass left open ??CompositeFrame will close it via vkCmdEndRenderPass
    _gpuProfiler.WriteEndTimestamp(cmd); // end "Indirect Draw"
    _cpuInstances.clear();
}

// ===========================================================================
// Phase 15C: IBL environment lighting
// ===========================================================================

bool VulkanBackend::LoadHDREnvironment(const std::string& hdrPath)
{
    VkDevice dev = _device->GetDevice();

    // Delegate IBL precompute (equirect?�cube, irradiance, prefilter, BRDF LUT) to VulkanIBL
    if (!_ibl.Init({ &_core })) return false;
    if (!_ibl.LoadHDREnvironment(hdrPath)) return false;

    // Compile IBL deferred lighting pipeline
    {
        std::vector<uint32_t> vsS, fsS;
        if (!CompileHLSLtoSPIRV(GetShaderFullPath(L"fullscreen.vert.hlsl").wstring(),              L"vs_6_0", vsS) ||
            !CompileGLSLtoSPIRV(GetShaderFullPath(L"deferred_lighting_ibl_vk.frag.glsl").wstring(), fsS)) {
            LUNA_LOG_ERROR("VK IBL: deferred_lighting_ibl_vk compile failed");
            return false;
        }
        auto mkMod = [&](const std::vector<uint32_t>& sp) -> VkShaderModule {
            VkShaderModuleCreateInfo si{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
            si.codeSize = sp.size() * 4; si.pCode = sp.data();
            VkShaderModule m = VK_NULL_HANDLE;
            vkCreateShaderModule(dev, &si, nullptr, &m);
            return m;
        };
        VkShaderModule vsM = mkMod(vsS), fsM = mkMod(fsS);

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,   vsM, "main" };
        stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fsM, "main" };

        VkPipelineVertexInputStateCreateInfo    vis{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo  ias{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        ias.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo       vps{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        vps.viewportCount = 1; vps.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo  rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo    ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo   dss{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        dss.depthTestEnable = VK_FALSE;
        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cbs{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cbs.attachmentCount = 1; cbs.pAttachments = &cba;
        VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dsi{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dsi.dynamicStateCount = 2; dsi.pDynamicStates = dyn;

        VkGraphicsPipelineCreateInfo gpi{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        gpi.stageCount          = 2; gpi.pStages          = stages;
        gpi.pVertexInputState   = &vis; gpi.pInputAssemblyState = &ias;
        gpi.pViewportState      = &vps; gpi.pRasterizationState = &rs;
        gpi.pMultisampleState   = &ms;  gpi.pDepthStencilState  = &dss;
        gpi.pColorBlendState    = &cbs; gpi.pDynamicState       = &dsi;
        gpi.layout              = _deferredPipeLayout;
        gpi.renderPass          = _ppRenderPass;  // Phase 16C: writes to HDR intermediate RT
        gpi.subpass             = 0;

        VkResult r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, nullptr, &_deferredIBLPipeline);
        vkDestroyShaderModule(dev, vsM, nullptr);
        vkDestroyShaderModule(dev, fsM, nullptr);
        if (r != VK_SUCCESS) { LUNA_LOG_ERROR("VK IBL: IBL deferred pipeline failed: %d", (int)r); return false; }
    }

    // Write IBL descriptors into the deferred G-buffer set and activate
    UpdateDeferredGbufDescriptors();
    LUNA_LOG_INFO("VK IBL: environment loaded — IBL active");

    // S2b: create shared IBL descriptor set for sensor lighting
    if (_sensorIBLLayout != VK_NULL_HANDLE && _sensorIBLSet == VK_NULL_HANDLE)
        CreateSensorIBLDescriptorSet();

    // Phase 28: Initialize atmosphere rendering (requires HDR + depth views from post-process)
    {
        VulkanAtmosphere::CreateInfo atmoCI{};
        atmoCI.core         = &_core;
        atmoCI.extent       = _vkSwapchain.GetExtent();
        atmoCI.depthView    = _vkSwapchain.GetDepthView();
        atmoCI.hdrView      = _postProcess.GetHDRView();
        atmoCI.hdrImage     = _postProcess.GetHDRImage();
        atmoCI.ppRenderPass = _postProcess.GetPPRenderPass();
        atmoCI.framesInFlight = FRAMES_IN_FLIGHT;
        if (_atmosphere.Create(atmoCI))
            LUNA_LOG_INFO("VK Atmosphere: initialized — sky rendering active");
        else
            LUNA_LOG_WARN("VK Atmosphere: init failed — no sky rendering");
    }

    // Phase 29: Initialize volumetric fog (requires HDR + depth + CSM shadow views)
    {
        VulkanVolumetricFog::CreateInfo fogCI{};
        fogCI.core          = &_core;
        fogCI.extent        = _vkSwapchain.GetExtent();
        fogCI.depthView     = _vkSwapchain.GetDepthView();
        fogCI.hdrView       = _postProcess.GetHDRView();
        fogCI.hdrImage      = _postProcess.GetHDRImage();
        fogCI.hdrRenderPass = _postProcess.GetPPRenderPass();
        fogCI.csmShadowView = _shadows.GetArrayView();
        fogCI.framesInFlight = FRAMES_IN_FLIGHT;
        if (_volumetricFog.Create(fogCI))
            LUNA_LOG_INFO("VK VolFog: initialized — volumetric fog active");
        else
            LUNA_LOG_WARN("VK VolFog: init failed — volumetric fog disabled");
    }

    // Phase 30: Initialize SSGI + probe GI (requires Hi-Z + IBL + G-buffer views)
    if (_hiZ.GetMipCount() >= 2 && _ibl.IsReady())
    {
        VulkanGI::CreateInfo giCI{};
        giCI.core           = &_core;
        giCI.extent         = _vkSwapchain.GetExtent();
        giCI.depthView      = _vkSwapchain.GetDepthView();
        giCI.gbufAlbedoView = _gBuffer.GetAlbedoView();
        giCI.gbufNormalView = _gBuffer.GetNormalView();
        giCI.hdrView        = _postProcess.GetHDRView();
        giCI.hdrImage       = _postProcess.GetHDRImage();
        giCI.hiZView        = _hiZ.GetFullView();
        giCI.hiZSampler     = _hiZ.GetSampler();
        giCI.irrCubeView    = _ibl.GetIrradianceView();
        giCI.iblSampler     = _ibl.GetIBLSampler();
        giCI.framesInFlight = FRAMES_IN_FLIGHT;
        if (_gi.Create(giCI))
        {
            LUNA_LOG_INFO("VK GI: SSGI + probe system active");
            if (!CreateGIDeferredResources())
                LUNA_LOG_WARN("VK GI: deferred pipeline init failed — GI output not wired");
        }
        else
            LUNA_LOG_WARN("VK GI: init failed — GI disabled");
    }

    // Phase 31: Initialize WBOIT (requires HDR view + depth view from swapchain)
    if (!CreateVKOITResources())
        LUNA_LOG_WARN("VK OIT: init failed — transparent objects will be skipped");

    // Phase 32: Visibility buffer (requires merged VB/IB from GPU-driven)
    if (!CreateVKVisibilityResources())
        LUNA_LOG_WARN("VK Phase 32: Visibility buffer init failed — using G-buffer path");

    return true;
}

// ===========================================================================
// Phase 30: GI deferred lighting pipeline — Vulkan wiring
// ===========================================================================

bool VulkanBackend::CreateGIDeferredResources()
{
    VkDevice dev = _device->GetDevice();

    // ── 1. Bilinear-clamp sampler for GI textures ──
    {
        VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        si.magFilter  = VK_FILTER_LINEAR;
        si.minFilter  = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = VK_LOD_CLAMP_NONE;
        if (vkCreateSampler(dev, &si, nullptr, &_giSampler) != VK_SUCCESS)
            return false;
    }

    // ── 2. Per-frame ProbeGridData UBOs (48 bytes, padded to 256) ──
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; ++i)
    {
        if (!CreateBuffer(256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                _giProbeGridUBO[i], _giProbeGridMem[i]))
            return false;
        vkMapMemory(dev, _giProbeGridMem[i], 0, 256, 0, &_giProbeGridMapped[i]);
    }

    // ── 3. Descriptor set layout (4 bindings: ssgiTex, probeIrrTex, sampler, UBO) ──
    {
        VkDescriptorSetLayoutBinding b[4]{};
        b[0].binding         = 0;
        b[0].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        b[0].descriptorCount = 1;
        b[0].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

        b[1].binding         = 1;
        b[1].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        b[1].descriptorCount = 1;
        b[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

        b[2].binding         = 2;
        b[2].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER;
        b[2].descriptorCount = 1;
        b[2].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

        b[3].binding         = 3;
        b[3].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b[3].descriptorCount = 1;
        b[3].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo li{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        li.bindingCount = 4; li.pBindings = b;
        if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &_giDescLayout) != VK_SUCCESS)
            return false;
    }

    // ── 4. Descriptor pool + per-frame descriptor sets ──
    {
        VkDescriptorPoolSize ps[3]{};
        ps[0].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;  ps[0].descriptorCount = 2 * FRAMES_IN_FLIGHT;
        ps[1].type = VK_DESCRIPTOR_TYPE_SAMPLER;        ps[1].descriptorCount = 1 * FRAMES_IN_FLIGHT;
        ps[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; ps[2].descriptorCount = 1 * FRAMES_IN_FLIGHT;
        VkDescriptorPoolCreateInfo dpi{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        dpi.poolSizeCount = 3; dpi.pPoolSizes = ps;
        dpi.maxSets = FRAMES_IN_FLIGHT;
        if (vkCreateDescriptorPool(dev, &dpi, nullptr, &_giDescPool) != VK_SUCCESS)
            return false;

        for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; ++i)
        {
            VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ai.descriptorPool     = _giDescPool;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts        = &_giDescLayout;
            if (vkAllocateDescriptorSets(dev, &ai, &_giDescSet[i]) != VK_SUCCESS)
                return false;
        }
    }

    // Write initial descriptors (ssgi view set by UpdateGIDescriptorSet each frame)
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; ++i)
        UpdateGIDescriptorSet(i);

    // ── 5. Pipeline layout: set={scene, gbuf, cluster, gi} ──
    {
        VkDescriptorSetLayout setLayouts[4] = {
            _deferredSceneLayout,
            _deferredGbufLayout,
            _clusterLightLayout,
            _giDescLayout
        };
        VkPipelineLayoutCreateInfo pli{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pli.setLayoutCount = 4; pli.pSetLayouts = setLayouts;
        if (vkCreatePipelineLayout(dev, &pli, nullptr, &_deferredGIPipeLayout) != VK_SUCCESS)
            return false;
    }

    // ── 6. GI deferred lighting pipeline ──
    {
        std::vector<uint32_t> vsS, fsS;
        if (!CompileHLSLtoSPIRV(GetShaderFullPath(L"fullscreen.vert.hlsl").wstring(), L"vs_6_0", vsS) ||
            !CompileGLSLtoSPIRV(GetShaderFullPath(L"deferred_lighting_gi_vk.frag.glsl").wstring(), fsS))
        {
            LUNA_LOG_ERROR("VK GI: deferred_lighting_gi_vk compile failed");
            return false;
        }
        auto mkMod = [&](const std::vector<uint32_t>& sp) -> VkShaderModule {
            VkShaderModuleCreateInfo si{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
            si.codeSize = sp.size() * 4; si.pCode = sp.data();
            VkShaderModule m = VK_NULL_HANDLE;
            vkCreateShaderModule(dev, &si, nullptr, &m);
            return m;
        };
        VkShaderModule vsM = mkMod(vsS), fsM = mkMod(fsS);

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
        VkPipelineMultisampleStateCreateInfo   ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo  dss{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        dss.depthTestEnable = VK_FALSE;
        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cbs{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cbs.attachmentCount = 1; cbs.pAttachments = &cba;
        VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dsi{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dsi.dynamicStateCount = 2; dsi.pDynamicStates = dyn;

        VkGraphicsPipelineCreateInfo gpi{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        gpi.stageCount        = 2;   gpi.pStages            = stages;
        gpi.pVertexInputState = &vis; gpi.pInputAssemblyState = &ias;
        gpi.pViewportState    = &vps; gpi.pRasterizationState = &rs;
        gpi.pMultisampleState = &ms;  gpi.pDepthStencilState  = &dss;
        gpi.pColorBlendState  = &cbs; gpi.pDynamicState       = &dsi;
        gpi.layout            = _deferredGIPipeLayout;
        gpi.renderPass        = _ppRenderPass;
        gpi.subpass           = 0;

        VkResult r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, nullptr, &_deferredGIPipeline);
        vkDestroyShaderModule(dev, vsM, nullptr);
        vkDestroyShaderModule(dev, fsM, nullptr);
        if (r != VK_SUCCESS)
        {
            LUNA_LOG_ERROR("VK GI: GI deferred pipeline creation failed: %d", (int)r);
            return false;
        }
    }

    LUNA_LOG_INFO("VK GI: deferred lighting GI pipeline active");
    return true;
}

void VulkanBackend::DestroyGIDeferredResources()
{
    VkDevice dev = _device->GetDevice();
    if (_deferredGIPipeline) { vkDestroyPipeline(dev, _deferredGIPipeline, nullptr);           _deferredGIPipeline   = VK_NULL_HANDLE; }
    if (_deferredGIPipeLayout){ vkDestroyPipelineLayout(dev, _deferredGIPipeLayout, nullptr);  _deferredGIPipeLayout = VK_NULL_HANDLE; }
    if (_giDescPool)          { vkDestroyDescriptorPool(dev, _giDescPool, nullptr);            _giDescPool           = VK_NULL_HANDLE; }
    if (_giDescLayout)        { vkDestroyDescriptorSetLayout(dev, _giDescLayout, nullptr);     _giDescLayout         = VK_NULL_HANDLE; }
    if (_giSampler)           { vkDestroySampler(dev, _giSampler, nullptr);                   _giSampler            = VK_NULL_HANDLE; }
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; ++i)
    {
        _giDescSet[i]         = VK_NULL_HANDLE;
        _giProbeGridMapped[i] = nullptr;
        if (_giProbeGridUBO[i]) { vkDestroyBuffer(dev, _giProbeGridUBO[i], nullptr);  _giProbeGridUBO[i] = VK_NULL_HANDLE; }
        if (_giProbeGridMem[i]) { vkFreeMemory(dev, _giProbeGridMem[i], nullptr);     _giProbeGridMem[i] = VK_NULL_HANDLE; }
    }
}

void VulkanBackend::UpdateGIDescriptorSet(uint32_t frameIndex)
{
    if (!_giDescSet[frameIndex] || !_gi.IsReady()) return;
    VkDevice dev = _device->GetDevice();

    // Update probe grid UBO
    ProbeGridUBO pgUBO{};
    pgUBO.origin[0]  = _gi.probeOrigin[0];  pgUBO.origin[1]  = _gi.probeOrigin[1];
    pgUBO.origin[2]  = _gi.probeOrigin[2];  pgUBO.origin[3]  = 0.0f;
    pgUBO.spacing[0] = _gi.probeSpacing[0]; pgUBO.spacing[1] = _gi.probeSpacing[1];
    pgUBO.spacing[2] = _gi.probeSpacing[2]; pgUBO.spacing[3] = 0.0f;
    pgUBO.dims[0]    = VulkanGI::PROBE_GRID_X;
    pgUBO.dims[1]    = VulkanGI::PROBE_GRID_Y;
    pgUBO.dims[2]    = VulkanGI::PROBE_GRID_Z;
    pgUBO.dims[3]    = 0u;
    memcpy(_giProbeGridMapped[frameIndex], &pgUBO, sizeof(pgUBO));

    // SSGI read view (ping-pong alternates each frame after Dispatch)
    VkImageView ssgiView  = _gi.GetSSGIReadView();
    VkImageView probeView = _gi.GetProbeIrrView();
    if (!ssgiView || !probeView) return;

    // VulkanGI keeps SSGI and probe atlas in GENERAL layout throughout
    VkDescriptorImageInfo ssgiII{};
    ssgiII.imageView   = ssgiView;
    ssgiII.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorImageInfo probeII{};
    probeII.imageView   = probeView;
    probeII.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorImageInfo samplerII{};
    samplerII.sampler = _giSampler;

    VkDescriptorBufferInfo uboBI{};
    uboBI.buffer = _giProbeGridUBO[frameIndex];
    uboBI.offset = 0;
    uboBI.range  = sizeof(ProbeGridUBO);

    VkWriteDescriptorSet w[4]{};
    w[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[0].dstSet          = _giDescSet[frameIndex];
    w[0].dstBinding      = 0;
    w[0].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w[0].descriptorCount = 1;
    w[0].pImageInfo      = &ssgiII;

    w[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[1].dstSet          = _giDescSet[frameIndex];
    w[1].dstBinding      = 1;
    w[1].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w[1].descriptorCount = 1;
    w[1].pImageInfo      = &probeII;

    w[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[2].dstSet          = _giDescSet[frameIndex];
    w[2].dstBinding      = 2;
    w[2].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER;
    w[2].descriptorCount = 1;
    w[2].pImageInfo      = &samplerII;

    w[3].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[3].dstSet          = _giDescSet[frameIndex];
    w[3].dstBinding      = 3;
    w[3].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w[3].descriptorCount = 1;
    w[3].pBufferInfo     = &uboBI;

    vkUpdateDescriptorSets(dev, 4, w, 0, nullptr);
}

// ===========================================================================
// Phase 18B: Vulkan Motion Blur pass
// ===========================================================================
void VulkanBackend::DrawVKMotionBlurPass()
{
    if (!_vkMBPipeline || !_mbFB || !_mbView) return;

    VkDevice        dev  = _device->GetDevice();
    VkCommandBuffer cmd  = _frames[_frameIndex].cmdBuffer;
    uint32_t        W    = _vkSwapchain.GetExtent().width;
    uint32_t        H    = _vkSwapchain.GetExtent().height;

    XMMATRIX V   = XMLoadFloat4x4(&_deferredView);
    XMMATRIX P   = XMLoadFloat4x4(&_deferredProj);
    XMMATRIX VP  = XMMatrixMultiply(V, P);
    XMMATRIX iVP = XMMatrixInverse(nullptr, VP);

    VKMotionBlurConstants cb{};
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb.invViewProj),  iVP);
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb.prevViewProj), XMLoadFloat4x4(&_vkMBLastVP));
    cb.screenSizeX  = (float)W;
    cb.screenSizeY  = (float)H;
    cb.shutterScale = 0.5f;
    cb.numSamples   = 8;
    memcpy(_vkMBCBMapped[_frameIndex], &cb, sizeof(cb));

    XMFLOAT4X4 vpF; XMStoreFloat4x4(&vpF, VP);
    _vkMBLastVP = vpF;

    VkClearValue cv{}; cv.color = { {0,0,0,1} };
    VkRenderPassBeginInfo rpi{}; rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpi.renderPass        = _ppRenderPass;
    rpi.framebuffer       = _mbFB;
    rpi.renderArea.extent = { W, H };
    rpi.clearValueCount   = 1;
    rpi.pClearValues      = &cv;
    vkCmdBeginRenderPass(cmd, &rpi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{ 0, 0, (float)W, (float)H, 0, 1 };
    VkRect2D sc{ {0,0}, {W, H} };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _vkMBPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _vkMBPipeLayout,
                            0, 1, &_vkMBDescSet[_frameIndex], 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdEndRenderPass(cmd);
}

// ===========================================================================
// Phase 18D: Vulkan Ray Tracing
// ===========================================================================
bool VulkanBackend::BuildAccelerationStructures()
{
    if (!_rtSupported || _meshASInfoCache.empty() || !_mergedVB || !_mergedIB) return false;
    VkDevice dev = _device->GetDevice();

    auto getAddr = [&](VkBuffer buf) -> VkDeviceAddress {
        VkBufferDeviceAddressInfo i{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
        i.buffer = buf;
        return vkGetBufferDeviceAddress(dev, &i);
    };

    auto makeASBuf = [&](VkDeviceSize sz, VkBufferUsageFlags extra, VkBuffer& b, VkDeviceMemory& m) {
        return CreateBuffer(sz,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | extra,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, b, m);
    };

    VkDeviceAddress vbAddr = getAddr(_mergedVB);
    VkDeviceAddress ibAddr = getAddr(_mergedIB);

    _vkBLASes.clear();
    _vkBLASes.reserve(_meshASInfoCache.size());

    for (size_t mi = 0; mi < _meshASInfoCache.size(); mi++)
    {
        const MeshASInfo& info = _meshASInfoCache[mi];

        VkAccelerationStructureGeometryTrianglesDataKHR tris{};
        tris.sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        tris.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        tris.vertexData.deviceAddress = vbAddr + (VkDeviceSize)info.vertexOffset * 48u;
        tris.vertexStride = 48u;
        tris.maxVertex    = info.vertexCount - 1;
        tris.indexType    = VK_INDEX_TYPE_UINT32;
        tris.indexData.deviceAddress = ibAddr + info.firstIndex * sizeof(uint32_t);

        VkAccelerationStructureGeometryKHR geom{};
        geom.sType            = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        geom.geometryType     = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geom.geometry.triangles = tris;
        geom.flags            = VK_GEOMETRY_OPAQUE_BIT_KHR;

        uint32_t triCount = info.indexCount / 3;

        VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
        buildInfo.sType         = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        buildInfo.type          = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        buildInfo.flags         = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        buildInfo.mode          = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        buildInfo.geometryCount = 1;
        buildInfo.pGeometries   = &geom;

        VkAccelerationStructureBuildSizesInfoKHR sizes{};
        sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
        pfn_vkGetAccelerationStructureBuildSizesKHR(dev,
            VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &triCount, &sizes);

        VKAccelStruct blas{};
        if (!makeASBuf(sizes.accelerationStructureSize, 0, blas.buf, blas.mem)) return false;

        VkAccelerationStructureCreateInfoKHR ci{};
        ci.sType  = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        ci.buffer = blas.buf;
        ci.size   = sizes.accelerationStructureSize;
        ci.type   = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        if (pfn_vkCreateAccelerationStructureKHR(dev, &ci, nullptr, &blas.as) != VK_SUCCESS) {
            vkDestroyBuffer(dev, blas.buf, nullptr); vkFreeMemory(dev, blas.mem, nullptr);
            return false;
        }

        VkBuffer scratchBuf; VkDeviceMemory scratchMem;
        makeASBuf(sizes.buildScratchSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, scratchBuf, scratchMem);

        buildInfo.dstAccelerationStructure  = blas.as;
        buildInfo.scratchData.deviceAddress = getAddr(scratchBuf);

        VkAccelerationStructureBuildRangeInfoKHR range{};
        range.primitiveCount = triCount;
        const VkAccelerationStructureBuildRangeInfoKHR* pRange = &range;

        VkCommandBuffer blasCmd = BeginSingleTimeCommands();
        pfn_vkCmdBuildAccelerationStructuresKHR(blasCmd, 1, &buildInfo, &pRange);
        VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        mb.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        mb.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        vkCmdPipelineBarrier(blasCmd,
            VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            0, 1, &mb, 0, nullptr, 0, nullptr);
        EndSingleTimeCommands(blasCmd);

        if (_deviceLost) {
            vkDestroyBuffer(dev, scratchBuf, nullptr);
            vkFreeMemory(dev, scratchMem, nullptr);
            LUNA_LOG_ERROR("VK RT: device lost during BLAS build (mesh %zu)", mi);
            return false;
        }

        vkDestroyBuffer(dev, scratchBuf, nullptr);
        vkFreeMemory(dev, scratchMem, nullptr);
        _vkBLASes.push_back(blas);
    }

    // Build TLAS
    std::vector<VkAccelerationStructureInstanceKHR> instances(_vkBLASes.size());
    for (size_t i = 0; i < _vkBLASes.size(); i++)
    {
        VkAccelerationStructureDeviceAddressInfoKHR addrInfo{};
        addrInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
        addrInfo.accelerationStructure = _vkBLASes[i].as;

        VkAccelerationStructureInstanceKHR inst{};
        inst.transform.matrix[0][0] = inst.transform.matrix[1][1] = inst.transform.matrix[2][2] = 1.0f;
        inst.instanceCustomIndex    = (uint32_t)i;
        inst.mask                   = 0xFF;
        inst.instanceShaderBindingTableRecordOffset = 0;
        inst.flags                  = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        inst.accelerationStructureReference = pfn_vkGetAccelerationStructureDeviceAddressKHR(dev, &addrInfo);
        instances[i] = inst;
    }

    VkDeviceSize instSize = instances.size() * sizeof(VkAccelerationStructureInstanceKHR);
    CreateBuffer(instSize,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        _vkInstanceBuf, _vkInstanceMem);
    void* instMapped; vkMapMemory(dev, _vkInstanceMem, 0, instSize, 0, &instMapped);
    memcpy(instMapped, instances.data(), (size_t)instSize);
    vkUnmapMemory(dev, _vkInstanceMem);

    VkAccelerationStructureGeometryInstancesDataKHR instData{};
    instData.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    instData.data.deviceAddress = getAddr(_vkInstanceBuf);

    VkAccelerationStructureGeometryKHR tlasGeom{};
    tlasGeom.sType              = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    tlasGeom.geometryType       = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tlasGeom.geometry.instances = instData;

    uint32_t instCount = (uint32_t)instances.size();
    VkAccelerationStructureBuildGeometryInfoKHR tlasBuild{};
    tlasBuild.sType         = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    tlasBuild.type          = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    tlasBuild.flags         = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    tlasBuild.mode          = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    tlasBuild.geometryCount = 1;
    tlasBuild.pGeometries   = &tlasGeom;

    VkAccelerationStructureBuildSizesInfoKHR tlasSizes{};
    tlasSizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    pfn_vkGetAccelerationStructureBuildSizesKHR(dev,
        VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &tlasBuild, &instCount, &tlasSizes);

    makeASBuf(tlasSizes.accelerationStructureSize, 0, _vkTLAS.buf, _vkTLAS.mem);

    VkAccelerationStructureCreateInfoKHR tlasCI{};
    tlasCI.sType  = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    tlasCI.buffer = _vkTLAS.buf;
    tlasCI.size   = tlasSizes.accelerationStructureSize;
    tlasCI.type   = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    if (pfn_vkCreateAccelerationStructureKHR(dev, &tlasCI, nullptr, &_vkTLAS.as) != VK_SUCCESS)
        return false;

    VkBuffer tlasScratch; VkDeviceMemory tlasScratchMem;
    makeASBuf(tlasSizes.buildScratchSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, tlasScratch, tlasScratchMem);

    tlasBuild.dstAccelerationStructure  = _vkTLAS.as;
    tlasBuild.scratchData.deviceAddress = getAddr(tlasScratch);

    VkAccelerationStructureBuildRangeInfoKHR tlasRange{};
    tlasRange.primitiveCount = instCount;
    const VkAccelerationStructureBuildRangeInfoKHR* pTlasRange = &tlasRange;

    VkCommandBuffer tlasCmd = BeginSingleTimeCommands();
    pfn_vkCmdBuildAccelerationStructuresKHR(tlasCmd, 1, &tlasBuild, &pTlasRange);
    EndSingleTimeCommands(tlasCmd);

    if (_deviceLost) {
        vkDestroyBuffer(dev, tlasScratch, nullptr);
        vkFreeMemory(dev, tlasScratchMem, nullptr);
        LUNA_LOG_ERROR("VK RT: device lost during TLAS build");
        return false;
    }

    vkDestroyBuffer(dev, tlasScratch, nullptr);
    vkFreeMemory(dev, tlasScratchMem, nullptr);

    LUNA_LOG_INFO("VK RT: Built %zu BLAS(es) + TLAS (%u instances)", _vkBLASes.size(), instCount);
    return true;
}

void VulkanBackend::DestroyAccelerationStructures()
{
    if (!_device) return;
    VkDevice dev = _device->GetDevice();
    for (auto& blas : _vkBLASes) {
        if (blas.as)  pfn_vkDestroyAccelerationStructureKHR(dev, blas.as, nullptr);
        if (blas.buf) vkDestroyBuffer(dev, blas.buf, nullptr);
        if (blas.mem) vkFreeMemory(dev, blas.mem, nullptr);
    }
    _vkBLASes.clear();
    if (_vkTLAS.as)  { pfn_vkDestroyAccelerationStructureKHR(dev, _vkTLAS.as, nullptr); _vkTLAS.as  = VK_NULL_HANDLE; }
    if (_vkTLAS.buf) { vkDestroyBuffer(dev, _vkTLAS.buf, nullptr);                      _vkTLAS.buf = VK_NULL_HANDLE; }
    if (_vkTLAS.mem) { vkFreeMemory(dev, _vkTLAS.mem, nullptr);                         _vkTLAS.mem = VK_NULL_HANDLE; }
    if (_vkInstanceBuf) { vkDestroyBuffer(dev, _vkInstanceBuf, nullptr); _vkInstanceBuf = VK_NULL_HANDLE; }
    if (_vkInstanceMem) { vkFreeMemory(dev, _vkInstanceMem, nullptr);    _vkInstanceMem = VK_NULL_HANDLE; }
}

bool VulkanBackend::CreateRTPipeline()
{
    if (!_rtSupported || _vkTLAS.as == VK_NULL_HANDLE) return false;
    VkDevice dev = _device->GetDevice();
    uint32_t W = _vkSwapchain.GetExtent().width, H = _vkSwapchain.GetExtent().height;

    if (!CreateImage(W, H, VK_FORMAT_R8_UNORM, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, _vkShadowMaskImage, _vkShadowMaskMem))
        return false;
    _vkShadowMaskView = CreateImageView(_vkShadowMaskImage, VK_FORMAT_R8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);

    auto getPhysProps2 = (PFN_vkGetPhysicalDeviceProperties2)
        vkGetInstanceProcAddr(_instance, "vkGetPhysicalDeviceProperties2");
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProps{};
    rtProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
    VkPhysicalDeviceProperties2 devProps2{};
    devProps2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    devProps2.pNext = &rtProps;
    getPhysProps2(_device->GetPhysicalDevice(), &devProps2);

    const uint32_t handleSize  = rtProps.shaderGroupHandleSize;
    const uint32_t baseAlign   = rtProps.shaderGroupBaseAlignment;
    const uint32_t entryStride = baseAlign;

    {
        VkDescriptorSetLayoutBinding bs[5]{};
        bs[0] = { 0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,  1, VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr };
        bs[1] = { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,               1, VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr };
        bs[2] = { 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,      1, VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr };
        bs[3] = { 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,      1, VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr };
        bs[4] = { 4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,              1, VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr };
        VkDescriptorSetLayoutCreateInfo li{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        li.bindingCount = 5; li.pBindings = bs;
        vkCreateDescriptorSetLayout(dev, &li, nullptr, &_vkRTLayout);
    }

    VkPipelineLayoutCreateInfo pli{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 1; pli.pSetLayouts = &_vkRTLayout;
    vkCreatePipelineLayout(dev, &pli, nullptr, &_vkRTPipeLayout);

    VkDescriptorPoolSize psizes[] = {
        { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, FRAMES_IN_FLIGHT },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,              FRAMES_IN_FLIGHT },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,     2 * FRAMES_IN_FLIGHT },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,             FRAMES_IN_FLIGHT },
    };
    VkDescriptorPoolCreateInfo dpi{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpi.poolSizeCount = (uint32_t)std::size(psizes);
    dpi.pPoolSizes    = psizes;
    dpi.maxSets       = FRAMES_IN_FLIGHT;
    vkCreateDescriptorPool(dev, &dpi, nullptr, &_vkRTDescPool);

    struct RTSceneUBO { float invViewProj[16]; float lightDir[3]; float maxDist; float _pad[44]; };
    static_assert(sizeof(RTSceneUBO) == 256, "RTSceneUBO must be 256B");

    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
        CreateBuffer(256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            _vkRTSceneCB[i], _vkRTSceneCBMem[i]);
        vkMapMemory(dev, _vkRTSceneCBMem[i], 0, 256, 0, &_vkRTSceneCBMapped[i]);

        VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        dsai.descriptorPool     = _vkRTDescPool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts        = &_vkRTLayout;
        vkAllocateDescriptorSets(dev, &dsai, &_vkRTDescSet[i]);

        VkWriteDescriptorSetAccelerationStructureKHR asWrite{};
        asWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
        asWrite.accelerationStructureCount = 1;
        asWrite.pAccelerationStructures    = &_vkTLAS.as;

        VkDescriptorImageInfo shadowII{ VK_NULL_HANDLE,        _vkShadowMaskView, VK_IMAGE_LAYOUT_GENERAL };
        VkDescriptorImageInfo depthII { _pointClampSampler,   _vkSwapchain.GetDepthView(),        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo normalII{ _pointClampSampler,   _gBuffer.GetNormalView(),     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorBufferInfo uboBI  { _vkRTSceneCB[i], 0, 256 };

        VkWriteDescriptorSet ws[5]{};
        ws[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, &asWrite, _vkRTDescSet[i], 0, 0, 1, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, nullptr,   nullptr, nullptr };
        ws[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,  _vkRTDescSet[i], 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,              &shadowII, nullptr, nullptr };
        ws[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,  _vkRTDescSet[i], 2, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,     &depthII,  nullptr, nullptr };
        ws[3] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,  _vkRTDescSet[i], 3, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,     &normalII, nullptr, nullptr };
        ws[4] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,  _vkRTDescSet[i], 4, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,             nullptr,   &uboBI,  nullptr };
        vkUpdateDescriptorSets(dev, 5, ws, 0, nullptr);
    }

    std::vector<uint32_t> rgenSpv, rmissSpv, rchitSpv;
    if (!CompileGLSLtoSPIRV(GetShaderFullPath(L"rt_shadows_vk.rgen.glsl").wstring(),  rgenSpv)  ||
        !CompileGLSLtoSPIRV(GetShaderFullPath(L"rt_shadows_vk.rmiss.glsl").wstring(), rmissSpv) ||
        !CompileGLSLtoSPIRV(GetShaderFullPath(L"rt_shadows_vk.rchit.glsl").wstring(), rchitSpv))
    {
        LUNA_LOG_ERROR("VK RT: shader compilation failed");
        return false;
    }

    auto mkMod = [&](const std::vector<uint32_t>& spv) -> VkShaderModule {
        VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        ci.codeSize = spv.size() * 4; ci.pCode = spv.data();
        VkShaderModule m = VK_NULL_HANDLE;
        vkCreateShaderModule(dev, &ci, nullptr, &m);
        return m;
    };
    VkShaderModule rgenMod  = mkMod(rgenSpv);
    VkShaderModule rmissMod = mkMod(rmissSpv);
    VkShaderModule rchitMod = mkMod(rchitSpv);

    VkPipelineShaderStageCreateInfo stages[3]{};
    stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                  VK_SHADER_STAGE_RAYGEN_BIT_KHR,       rgenMod,  "main", nullptr };
    stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                  VK_SHADER_STAGE_MISS_BIT_KHR,         rmissMod, "main", nullptr };
    stages[2] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                  VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,  rchitMod, "main", nullptr };

    VkRayTracingShaderGroupCreateInfoKHR groups[3]{};
    groups[0].sType              = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[0].type               = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[0].generalShader      = 0;
    groups[0].closestHitShader   = VK_SHADER_UNUSED_KHR;
    groups[0].anyHitShader       = VK_SHADER_UNUSED_KHR;
    groups[0].intersectionShader = VK_SHADER_UNUSED_KHR;
    groups[1] = groups[0];
    groups[1].generalShader = 1;
    groups[2].sType              = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[2].type               = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    groups[2].generalShader      = VK_SHADER_UNUSED_KHR;
    groups[2].closestHitShader   = 2;
    groups[2].anyHitShader       = VK_SHADER_UNUSED_KHR;
    groups[2].intersectionShader = VK_SHADER_UNUSED_KHR;

    VkRayTracingPipelineCreateInfoKHR rtci{};
    rtci.sType                        = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
    rtci.stageCount                   = 3;
    rtci.pStages                      = stages;
    rtci.groupCount                   = 3;
    rtci.pGroups                      = groups;
    rtci.maxPipelineRayRecursionDepth = 1;
    rtci.layout                       = _vkRTPipeLayout;
    if (pfn_vkCreateRayTracingPipelinesKHR(dev, VK_NULL_HANDLE, VK_NULL_HANDLE,
                                            1, &rtci, nullptr, &_vkRTPipeline) != VK_SUCCESS)
    {
        vkDestroyShaderModule(dev, rgenMod, nullptr);
        vkDestroyShaderModule(dev, rmissMod, nullptr);
        vkDestroyShaderModule(dev, rchitMod, nullptr);
        return false;
    }
    vkDestroyShaderModule(dev, rgenMod, nullptr);
    vkDestroyShaderModule(dev, rmissMod, nullptr);
    vkDestroyShaderModule(dev, rchitMod, nullptr);

    VkDeviceSize sbtSize = 3 * entryStride;
    CreateBuffer(sbtSize,
        VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        _vkSBTBuffer, _vkSBTMem);

    std::vector<uint8_t> handles(3 * handleSize);
    pfn_vkGetRayTracingShaderGroupHandlesKHR(dev, _vkRTPipeline, 0, 3, handles.size(), handles.data());

    uint8_t* sbtMapped;
    vkMapMemory(dev, _vkSBTMem, 0, sbtSize, 0, (void**)&sbtMapped);
    memcpy(sbtMapped + 0 * entryStride, handles.data() + 0 * handleSize, handleSize);
    memcpy(sbtMapped + 1 * entryStride, handles.data() + 1 * handleSize, handleSize);
    memcpy(sbtMapped + 2 * entryStride, handles.data() + 2 * handleSize, handleSize);
    vkUnmapMemory(dev, _vkSBTMem);

    VkBufferDeviceAddressInfo bdai{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
    bdai.buffer = _vkSBTBuffer;
    VkDeviceAddress sbtAddr = vkGetBufferDeviceAddress(dev, &bdai);

    _vkRgenRegion = { sbtAddr + 0 * entryStride, entryStride, entryStride };
    _vkMissRegion = { sbtAddr + 1 * entryStride, entryStride, entryStride };
    _vkHitRegion  = { sbtAddr + 2 * entryStride, entryStride, entryStride };
    _vkCallRegion = {};

    UpdateDeferredGbufDescriptors();

    LUNA_LOG_INFO("VK RT: pipeline + SBT created (handle=%uB, entry=%uB)", handleSize, entryStride);
    return true;
}

void VulkanBackend::DestroyRTPipeline()
{
    if (!_device) return;
    VkDevice dev = _device->GetDevice();

    if (_vkRTPipeline)   { vkDestroyPipeline(dev, _vkRTPipeline, nullptr);          _vkRTPipeline   = VK_NULL_HANDLE; }
    if (_vkRTPipeLayout) { vkDestroyPipelineLayout(dev, _vkRTPipeLayout, nullptr);  _vkRTPipeLayout = VK_NULL_HANDLE; }
    if (_vkRTDescPool)   { vkDestroyDescriptorPool(dev, _vkRTDescPool, nullptr);    _vkRTDescPool   = VK_NULL_HANDLE; }
    if (_vkRTLayout)     { vkDestroyDescriptorSetLayout(dev, _vkRTLayout, nullptr); _vkRTLayout     = VK_NULL_HANDLE; }
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
        if (_vkRTSceneCBMapped[i]) { vkUnmapMemory(dev, _vkRTSceneCBMem[i]);    _vkRTSceneCBMapped[i] = nullptr; }
        if (_vkRTSceneCB[i])       { vkDestroyBuffer(dev, _vkRTSceneCB[i], nullptr); _vkRTSceneCB[i] = VK_NULL_HANDLE; }
        if (_vkRTSceneCBMem[i])    { vkFreeMemory(dev, _vkRTSceneCBMem[i], nullptr); _vkRTSceneCBMem[i] = VK_NULL_HANDLE; }
        _vkRTDescSet[i] = VK_NULL_HANDLE;
    }
    if (_vkSBTBuffer)       { vkDestroyBuffer(dev, _vkSBTBuffer, nullptr);   _vkSBTBuffer = VK_NULL_HANDLE; }
    if (_vkSBTMem)          { vkFreeMemory(dev, _vkSBTMem, nullptr);         _vkSBTMem    = VK_NULL_HANDLE; }
    if (_vkShadowMaskView)  { vkDestroyImageView(dev, _vkShadowMaskView, nullptr); _vkShadowMaskView  = VK_NULL_HANDLE; }
    if (_vkShadowMaskImage) { vkDestroyImage(dev, _vkShadowMaskImage, nullptr);    _vkShadowMaskImage = VK_NULL_HANDLE; }
    if (_vkShadowMaskMem)   { vkFreeMemory(dev, _vkShadowMaskMem, nullptr);        _vkShadowMaskMem   = VK_NULL_HANDLE; }
    _vkRgenRegion = _vkMissRegion = _vkHitRegion = _vkCallRegion = {};
}

// ===========================================================================
// Phase 24: Clustered Lighting
// ===========================================================================
bool VulkanBackend::CreateClusteredLightingResources()
{
    VkDevice dev = _device->GetDevice();

    // ── SSBOs ──
    VkDeviceSize lightSSBOSize   = MAX_POINT_LIGHTS * sizeof(GPUPointLight);       // 32 KB
    VkDeviceSize countsSize      = CLUSTER_COUNT * sizeof(uint32_t);                // ~14 KB
    VkDeviceSize indicesSize     = CLUSTER_COUNT * MAX_LIGHTS_PER_CLUSTER * sizeof(uint32_t);  // ~1.7 MB

    // Light SSBO (host-visible for CPU upload)
    if (!CreateBuffer(lightSSBOSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            _lightSSBO, _lightSSBOMem))
        return false;
    vkMapMemory(dev, _lightSSBOMem, 0, lightSSBOSize, 0, &_lightSSBOMapped);

    // Cluster counts SSBO (device-local, written by compute, read by fragment)
    if (!CreateBuffer(countsSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            _clusterCountsSSBO, _clusterCountsSSBOMem))
        return false;

    // Cluster indices SSBO (device-local, written by compute, read by fragment)
    if (!CreateBuffer(indicesSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            _clusterIndicesSSBO, _clusterIndicesSSBOMem))
        return false;

    // Cluster params UBO (host-visible)
    if (!CreateBuffer(256,
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            _clusterParamsCB, _clusterParamsCBMem))
        return false;
    vkMapMemory(dev, _clusterParamsCBMem, 0, 256, 0, &_clusterParamsCBMapped);

    // ── Compute descriptor set layout (set=0: UBO, lights, counts, indices) ──
    {
        VkDescriptorSetLayoutBinding bs[4]{};
        bs[0] = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        bs[1] = { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        bs[2] = { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        bs[3] = { 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };

        VkDescriptorSetLayoutCreateInfo li{};
        li.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 4;
        li.pBindings    = bs;
        vkCreateDescriptorSetLayout(dev, &li, nullptr, &_clusterCompDescLayout);
    }

    // ── Fragment shader descriptor set layout (set=2: UBO, lights, counts, indices — read-only) ──
    {
        VkDescriptorSetLayoutBinding bs[4]{};
        bs[0] = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,  1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        bs[1] = { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        bs[2] = { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        bs[3] = { 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };

        VkDescriptorSetLayoutCreateInfo li{};
        li.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 4;
        li.pBindings    = bs;
        vkCreateDescriptorSetLayout(dev, &li, nullptr, &_clusterLightLayout);
    }

    // ── Compute pipeline layout ──
    {
        VkPipelineLayoutCreateInfo pli{};
        pli.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = 1;
        pli.pSetLayouts    = &_clusterCompDescLayout;
        vkCreatePipelineLayout(dev, &pli, nullptr, &_clusterCompPipeLayout);
    }

    // ── Descriptor pool (1 compute set + 1 fragment set) ──
    {
        VkDescriptorPoolSize dsz[] = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,  2 },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  6 },
        };
        VkDescriptorPoolCreateInfo dpi{};
        dpi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpi.poolSizeCount = (uint32_t)std::size(dsz);
        dpi.pPoolSizes    = dsz;
        dpi.maxSets       = 2;
        vkCreateDescriptorPool(dev, &dpi, nullptr, &_clusterCompDescPool);
    }

    // ── Allocate compute descriptor set ──
    {
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool     = _clusterCompDescPool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts        = &_clusterCompDescLayout;
        vkAllocateDescriptorSets(dev, &dsai, &_clusterCompDescSet);
    }

    // ── Allocate fragment descriptor set (set=2) ──
    {
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool     = _clusterCompDescPool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts        = &_clusterLightLayout;
        vkAllocateDescriptorSets(dev, &dsai, &_clusterLightDescSet);
    }

    // ── Write descriptors (both sets use the same buffers) ──
    auto writeDescs = [&](VkDescriptorSet dset) {
        VkDescriptorBufferInfo uboInfo  { _clusterParamsCB,     0, sizeof(ClusterParamsUBO) };
        VkDescriptorBufferInfo lightInfo{ _lightSSBO,           0, MAX_POINT_LIGHTS * sizeof(GPUPointLight) };
        VkDescriptorBufferInfo countInfo{ _clusterCountsSSBO,   0, CLUSTER_COUNT * sizeof(uint32_t) };
        VkDescriptorBufferInfo idxInfo  { _clusterIndicesSSBO,  0, CLUSTER_COUNT * MAX_LIGHTS_PER_CLUSTER * sizeof(uint32_t) };

        VkWriteDescriptorSet w[4]{};
        for (int i = 0; i < 4; i++) {
            w[i].sType      = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet     = dset;
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
        }
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;  w[0].pBufferInfo = &uboInfo;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;  w[1].pBufferInfo = &lightInfo;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;  w[2].pBufferInfo = &countInfo;
        w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;  w[3].pBufferInfo = &idxInfo;

        vkUpdateDescriptorSets(dev, 4, w, 0, nullptr);
    };

    writeDescs(_clusterCompDescSet);
    writeDescs(_clusterLightDescSet);

    // ── Compile compute shader + create pipeline ──
    std::vector<uint32_t> compSpv;
    if (!CompileGLSLtoSPIRV(GetShaderFullPath(L"cluster_assign_vk.comp.glsl").wstring(), compSpv))
    {
        LUNA_LOG_WARN("VK: Cluster assign shader compile failed — clustered lighting disabled");
        return false;
    }

    VkShaderModuleCreateInfo smi{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smi.codeSize = compSpv.size() * 4;
    smi.pCode    = compSpv.data();
    VkShaderModule csm = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &smi, nullptr, &csm);

    VkComputePipelineCreateInfo cpi{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    cpi.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = csm;
    cpi.stage.pName  = "main";
    cpi.layout       = _clusterCompPipeLayout;
    vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &_clusterCompPipeline);
    vkDestroyShaderModule(dev, csm, nullptr);

    if (!_clusterCompPipeline) {
        LUNA_LOG_WARN("VK: Cluster compute pipeline creation failed");
        return false;
    }

    _clusteredLightingReady = true;
    LUNA_LOG_INFO("VK: Phase 24 clustered lighting ready (%ux%ux%u clusters, max %u lights)",
                  CLUSTER_X, CLUSTER_Y, CLUSTER_Z, MAX_POINT_LIGHTS);
    return true;
}

void VulkanBackend::DestroyClusteredLightingResources()
{
    _clusteredLightingReady = false;
    VkDevice dev = _device->GetDevice();

    if (_clusterCompPipeline)   { vkDestroyPipeline(dev, _clusterCompPipeline, nullptr);              _clusterCompPipeline   = VK_NULL_HANDLE; }
    if (_clusterCompPipeLayout) { vkDestroyPipelineLayout(dev, _clusterCompPipeLayout, nullptr);      _clusterCompPipeLayout = VK_NULL_HANDLE; }
    if (_clusterCompDescLayout) { vkDestroyDescriptorSetLayout(dev, _clusterCompDescLayout, nullptr); _clusterCompDescLayout = VK_NULL_HANDLE; }
    if (_clusterLightLayout)    { vkDestroyDescriptorSetLayout(dev, _clusterLightLayout, nullptr);    _clusterLightLayout    = VK_NULL_HANDLE; }
    if (_clusterCompDescPool)   { vkDestroyDescriptorPool(dev, _clusterCompDescPool, nullptr);        _clusterCompDescPool   = VK_NULL_HANDLE; }
    _clusterCompDescSet   = VK_NULL_HANDLE;
    _clusterLightDescSet  = VK_NULL_HANDLE;

    if (_lightSSBOMapped) { vkUnmapMemory(dev, _lightSSBOMem); _lightSSBOMapped = nullptr; }
    if (_lightSSBO)       { vkDestroyBuffer(dev, _lightSSBO, nullptr);       _lightSSBO       = VK_NULL_HANDLE; }
    if (_lightSSBOMem)    { vkFreeMemory(dev, _lightSSBOMem, nullptr);       _lightSSBOMem    = VK_NULL_HANDLE; }

    if (_clusterCountsSSBO)    { vkDestroyBuffer(dev, _clusterCountsSSBO, nullptr);    _clusterCountsSSBO    = VK_NULL_HANDLE; }
    if (_clusterCountsSSBOMem) { vkFreeMemory(dev, _clusterCountsSSBOMem, nullptr);    _clusterCountsSSBOMem = VK_NULL_HANDLE; }
    if (_clusterIndicesSSBO)    { vkDestroyBuffer(dev, _clusterIndicesSSBO, nullptr);   _clusterIndicesSSBO    = VK_NULL_HANDLE; }
    if (_clusterIndicesSSBOMem) { vkFreeMemory(dev, _clusterIndicesSSBOMem, nullptr);   _clusterIndicesSSBOMem = VK_NULL_HANDLE; }
    if (_clusterParamsCBMapped) { vkUnmapMemory(dev, _clusterParamsCBMem); _clusterParamsCBMapped = nullptr; }
    if (_clusterParamsCB)     { vkDestroyBuffer(dev, _clusterParamsCB, nullptr);       _clusterParamsCB     = VK_NULL_HANDLE; }
    if (_clusterParamsCBMem)  { vkFreeMemory(dev, _clusterParamsCBMem, nullptr);       _clusterParamsCBMem  = VK_NULL_HANDLE; }
}

void VulkanBackend::DispatchClusterAssign(VkCommandBuffer cmd)
{
    if (!_clusteredLightingReady || _pointLights.empty()) return;

    // Upload light data (transform to view space)
    XMMATRIX V = XMLoadFloat4x4(&_deferredView);
    uint32_t numLights = (uint32_t)std::min((size_t)MAX_POINT_LIGHTS, _pointLights.size());

    std::vector<GPUPointLight> viewSpaceLights(numLights);
    for (uint32_t i = 0; i < numLights; i++) {
        const auto& l = _pointLights[i];
        XMVECTOR posWS = XMVectorSet(l.position[0], l.position[1], l.position[2], 1.0f);
        XMVECTOR posVS = XMVector4Transform(posWS, V);
        XMFLOAT4 vs; XMStoreFloat4(&vs, posVS);
        viewSpaceLights[i].position[0] = vs.x;
        viewSpaceLights[i].position[1] = vs.y;
        viewSpaceLights[i].position[2] = vs.z;
        viewSpaceLights[i].radius    = l.radius;
        viewSpaceLights[i].color[0]  = l.color[0];
        viewSpaceLights[i].color[1]  = l.color[1];
        viewSpaceLights[i].color[2]  = l.color[2];
        viewSpaceLights[i].intensity = l.intensity;
    }
    memcpy(_lightSSBOMapped, viewSpaceLights.data(), numLights * sizeof(GPUPointLight));

    // Update cluster params UBO
    {
        XMMATRIX P = XMLoadFloat4x4(&_deferredProj);
        XMMATRIX iP = XMMatrixInverse(nullptr, P);
        XMFLOAT4X4 iPF; XMStoreFloat4x4(&iPF, iP);

        ClusterParamsUBO cp{};
        memcpy(cp.invProj, &iPF, 64);
        cp.nearZ    = 0.1f;
        cp.farZ     = 100.0f;
        cp.screenW  = (float)_vkSwapchain.GetExtent().width;
        cp.screenH  = (float)_vkSwapchain.GetExtent().height;
        cp.numLights = numLights;
        memcpy(_clusterParamsCBMapped, &cp, sizeof(cp));
    }

    // Clear cluster counts
    vkCmdFillBuffer(cmd, _clusterCountsSSBO, 0, CLUSTER_COUNT * sizeof(uint32_t), 0);

    // Barrier: fill → compute read/write
    VkBufferMemoryBarrier clearBarrier{};
    clearBarrier.sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    clearBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    clearBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    clearBarrier.buffer        = _clusterCountsSSBO;
    clearBarrier.offset        = 0;
    clearBarrier.size          = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 1, &clearBarrier, 0, nullptr);

    // Bind + dispatch
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _clusterCompPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                             _clusterCompPipeLayout, 0, 1, &_clusterCompDescSet, 0, nullptr);
    vkCmdDispatch(cmd, CLUSTER_X, CLUSTER_Y, CLUSTER_Z);

    // Barrier: compute write → fragment read
    VkBufferMemoryBarrier readBarriers[2]{};
    for (int i = 0; i < 2; i++) {
        readBarriers[i].sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        readBarriers[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        readBarriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        readBarriers[i].offset        = 0;
        readBarriers[i].size          = VK_WHOLE_SIZE;
    }
    readBarriers[0].buffer = _clusterCountsSSBO;
    readBarriers[1].buffer = _clusterIndicesSSBO;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 2, readBarriers, 0, nullptr);
}

// ===========================================================================
// Phase 31: WBOIT (Weighted Blended OIT) — Vulkan implementation
// ===========================================================================

bool VulkanBackend::CreateVKOITResources()
{
    VkDevice dev = _device->GetDevice();
    const VkExtent2D ext = _vkSwapchain.GetExtent();
    const uint32_t W = ext.width, H = ext.height;

    // ── 1. OIT render target images ──
    if (!CreateImage(W, H, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, _oitAccumImage, _oitAccumMem))
        return false;
    _oitAccumView = CreateImageView(_oitAccumImage, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!_oitAccumView) return false;

    if (!CreateImage(W, H, VK_FORMAT_R8_UNORM, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, _oitRevealImage, _oitRevealMem))
        return false;
    _oitRevealView = CreateImageView(_oitRevealImage, VK_FORMAT_R8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!_oitRevealView) return false;

    // Transition to SHADER_READ_ONLY_OPTIMAL so composite descriptor write is valid at init
    TransitionImageLayout(_oitAccumImage,  VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    TransitionImageLayout(_oitRevealImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // ── 2. OIT Forward render pass: 2 color (CLEAR) + depth (LOAD, read-only) ──
    {
        VkAttachmentDescription atts[3]{};
        // accum (RGBA16F)
        atts[0].format         = VK_FORMAT_R16G16B16A16_SFLOAT;
        atts[0].samples        = VK_SAMPLE_COUNT_1_BIT;
        atts[0].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        atts[0].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        atts[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[0].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        atts[0].finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        // revealage (R8_UNORM)
        atts[1].format         = VK_FORMAT_R8_UNORM;
        atts[1].samples        = VK_SAMPLE_COUNT_1_BIT;
        atts[1].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        atts[1].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        atts[1].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[1].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        atts[1].finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        // depth (D32, LOAD — preserve opaque depth for test, no write)
        atts[2].format         = VK_FORMAT_D32_SFLOAT;
        atts[2].samples        = VK_SAMPLE_COUNT_1_BIT;
        atts[2].loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
        atts[2].storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[2].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[2].initialLayout  = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        atts[2].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        VkAttachmentReference colRefs[2]{};
        colRefs[0] = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        colRefs[1] = { 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkAttachmentReference depRef{ 2, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };

        VkSubpassDescription sp{};
        sp.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sp.colorAttachmentCount    = 2;
        sp.pColorAttachments       = colRefs;
        sp.pDepthStencilAttachment = &depRef;

        VkSubpassDependency dep{};
        dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass    = 0;
        dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;

        VkRenderPassCreateInfo rpi{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        rpi.attachmentCount = 3; rpi.pAttachments  = atts;
        rpi.subpassCount    = 1; rpi.pSubpasses    = &sp;
        rpi.dependencyCount = 1; rpi.pDependencies = &dep;
        if (vkCreateRenderPass(dev, &rpi, nullptr, &_oitFwdRenderPass) != VK_SUCCESS)
            return false;
    }

    // ── 3. OIT Composite render pass: HDR color (LOAD_OP_LOAD, blend transparent onto opaque) ──
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
        sp.colorAttachmentCount = 1; sp.pColorAttachments = &cr;

        VkSubpassDependency dep{};
        dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass    = 0;
        dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;

        VkRenderPassCreateInfo rpi{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        rpi.attachmentCount = 1; rpi.pAttachments  = &att;
        rpi.subpassCount    = 1; rpi.pSubpasses    = &sp;
        rpi.dependencyCount = 1; rpi.pDependencies = &dep;
        if (vkCreateRenderPass(dev, &rpi, nullptr, &_oitCmpRenderPass) != VK_SUCCESS)
            return false;
    }

    // ── 4. Framebuffers ──
    {
        VkImageView fwdAtts[3] = { _oitAccumView, _oitRevealView, _vkSwapchain.GetDepthView() };
        VkFramebufferCreateInfo fi{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fi.renderPass = _oitFwdRenderPass; fi.attachmentCount = 3; fi.pAttachments = fwdAtts;
        fi.width = W; fi.height = H; fi.layers = 1;
        if (vkCreateFramebuffer(dev, &fi, nullptr, &_oitFwdFramebuffer) != VK_SUCCESS)
            return false;
    }
    {
        VkImageView hdrView = _postProcess.GetHDRView();
        VkFramebufferCreateInfo fi{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fi.renderPass = _oitCmpRenderPass; fi.attachmentCount = 1; fi.pAttachments = &hdrView;
        fi.width = W; fi.height = H; fi.layers = 1;
        if (vkCreateFramebuffer(dev, &fi, nullptr, &_oitCmpFramebuffer) != VK_SUCCESS)
            return false;
    }

    // ── 5. Per-frame OITSceneData UBOs ──
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; ++i)
    {
        if (!CreateBuffer(256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                _oitSceneUBO[i], _oitSceneUBOMem[i]))
            return false;
        vkMapMemory(dev, _oitSceneUBOMem[i], 0, 256, 0, &_oitSceneUBOMapped[i]);
    }

    // ── 6. Descriptor set layouts ──
    // set=0 (forward): OITSceneUBO (VS+FS)
    {
        VkDescriptorSetLayoutBinding b{};
        b.binding = 0; b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo li{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        li.bindingCount = 1; li.pBindings = &b;
        if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &_oitSceneLayout) != VK_SUCCESS)
            return false;
    }
    // set=1 (forward): albedoTex (combined image sampler, FS only)
    {
        VkDescriptorSetLayoutBinding b{};
        b.binding = 0; b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo li{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        li.bindingCount = 1; li.pBindings = &b;
        if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &_oitAlbedoLayout) != VK_SUCCESS)
            return false;
    }
    // set=0 (composite): binding0=accum, binding1=revealage (combined image samplers)
    {
        VkDescriptorSetLayoutBinding b[2]{};
        b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo li{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        li.bindingCount = 2; li.pBindings = b;
        if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &_oitCmpLayout) != VK_SUCCESS)
            return false;
    }

    // ── 7. Descriptor pool + sets ──
    {
        const uint32_t albedoSets = FRAMES_IN_FLIGHT * MAX_OIT_MESHES;
        VkDescriptorPoolSize ps[2]{};
        ps[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;         ps[0].descriptorCount = FRAMES_IN_FLIGHT;
        ps[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; ps[1].descriptorCount = albedoSets + 2; // +2 composite
        VkDescriptorPoolCreateInfo dpi{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        dpi.maxSets = FRAMES_IN_FLIGHT + albedoSets + 1;
        dpi.poolSizeCount = 2; dpi.pPoolSizes = ps;
        if (vkCreateDescriptorPool(dev, &dpi, nullptr, &_oitDescPool) != VK_SUCCESS)
            return false;

        // Scene UBO sets
        for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; ++i)
        {
            VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ai.descriptorPool = _oitDescPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &_oitSceneLayout;
            if (vkAllocateDescriptorSets(dev, &ai, &_oitSceneDescSet[i]) != VK_SUCCESS) return false;
        }
        // Pre-allocated albedo sets (updated per-draw in DrawVKOITForward)
        for (uint32_t f = 0; f < FRAMES_IN_FLIGHT; ++f)
        {
            for (uint32_t m = 0; m < MAX_OIT_MESHES; ++m)
            {
                VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
                ai.descriptorPool = _oitDescPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &_oitAlbedoLayout;
                if (vkAllocateDescriptorSets(dev, &ai, &_oitAlbedoDescSets[f][m]) != VK_SUCCESS) return false;
            }
        }
        // Composite set
        {
            VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ai.descriptorPool = _oitDescPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &_oitCmpLayout;
            if (vkAllocateDescriptorSets(dev, &ai, &_oitCmpDescSet) != VK_SUCCESS) return false;
        }
    }

    // ── 8. Write initial scene UBO descriptors ──
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; ++i)
    {
        VkDescriptorBufferInfo bi{ _oitSceneUBO[i], 0, sizeof(OITSceneData) };
        VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w.dstSet = _oitSceneDescSet[i]; w.dstBinding = 0;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w.descriptorCount = 1;
        w.pBufferInfo = &bi;
        vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
    }

    // ── 9. Samplers ──
    {
        VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        si.magFilter = VK_FILTER_LINEAR; si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = VK_LOD_CLAMP_NONE;
        if (vkCreateSampler(dev, &si, nullptr, &_oitLinearSampler) != VK_SUCCESS) return false;
    }
    {
        VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        si.magFilter = VK_FILTER_NEAREST; si.minFilter = VK_FILTER_NEAREST;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = VK_LOD_CLAMP_NONE;
        if (vkCreateSampler(dev, &si, nullptr, &_oitPointSampler) != VK_SUCCESS) return false;
    }

    // Write composite descriptor (accum + revealage as combined samplers)
    {
        VkDescriptorImageInfo ii[2]{};
        ii[0] = { _oitPointSampler, _oitAccumView,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        ii[1] = { _oitPointSampler, _oitRevealView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet w[2]{};
        w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet = _oitCmpDescSet;
        w[0].dstBinding = 0; w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[0].descriptorCount = 1; w[0].pImageInfo = &ii[0];
        w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[1].dstSet = _oitCmpDescSet;
        w[1].dstBinding = 1; w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[1].descriptorCount = 1; w[1].pImageInfo = &ii[1];
        vkUpdateDescriptorSets(dev, 2, w, 0, nullptr);
    }

    // ── 10. Pipeline layouts ──
    // Forward: set=0 scene + set=1 albedo + push_constant 80B (VS+FS)
    {
        VkDescriptorSetLayout layouts[2] = { _oitSceneLayout, _oitAlbedoLayout };
        VkPushConstantRange pc{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 80 };
        VkPipelineLayoutCreateInfo pli{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pli.setLayoutCount = 2; pli.pSetLayouts = layouts;
        pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pc;
        if (vkCreatePipelineLayout(dev, &pli, nullptr, &_oitFwdPipeLayout) != VK_SUCCESS)
            return false;
    }
    // Composite: set=0 (accum+revealage), no push constants
    {
        VkPipelineLayoutCreateInfo pli{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pli.setLayoutCount = 1; pli.pSetLayouts = &_oitCmpLayout;
        if (vkCreatePipelineLayout(dev, &pli, nullptr, &_oitCmpPipeLayout) != VK_SUCCESS)
            return false;
    }

    // ── 11. Pipelines ──
    auto mkMod = [&](const std::vector<uint32_t>& sp) -> VkShaderModule {
        VkShaderModuleCreateInfo si{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        si.codeSize = sp.size() * 4; si.pCode = sp.data();
        VkShaderModule m = VK_NULL_HANDLE;
        vkCreateShaderModule(dev, &si, nullptr, &m);
        return m;
    };

    // OIT Forward pipeline (PBRVertex input, 2 MRT, depth test ON write OFF)
    {
        std::vector<uint32_t> vsS, fsS;
        if (!CompileGLSLtoSPIRV(GetShaderFullPath(L"oit_forward_vk.vert.glsl").wstring(), vsS) ||
            !CompileGLSLtoSPIRV(GetShaderFullPath(L"oit_forward_vk.frag.glsl").wstring(), fsS))
        {
            LUNA_LOG_ERROR("VK OIT: oit_forward_vk shader compile failed");
            return false;
        }
        VkShaderModule vsM = mkMod(vsS), fsM = mkMod(fsS);

        // PBRVertex: pos(12)+normal(12)+uv(8)+tangent(16) = stride 48
        VkVertexInputBindingDescription    vbd{ 0, 48, VK_VERTEX_INPUT_RATE_VERTEX };
        VkVertexInputAttributeDescription  vad[4]{};
        vad[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT,    0  };
        vad[1] = { 1, 0, VK_FORMAT_R32G32B32_SFLOAT,    12 };
        vad[2] = { 2, 0, VK_FORMAT_R32G32_SFLOAT,       24 };
        vad[3] = { 3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 32 };

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,   vsM, "main" };
        stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fsM, "main" };

        VkPipelineVertexInputStateCreateInfo vis{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        vis.vertexBindingDescriptionCount   = 1;  vis.pVertexBindingDescriptions   = &vbd;
        vis.vertexAttributeDescriptionCount = 4;  vis.pVertexAttributeDescriptions = vad;
        VkPipelineInputAssemblyStateCreateInfo ias{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        ias.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vps{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        vps.viewportCount = 1; vps.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;  // no face cull for transparency
        rs.frontFace   = VK_FRONT_FACE_CLOCKWISE; rs.lineWidth = 1.0f;           // -fvk-invert-y convention
        VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo dss{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        dss.depthTestEnable  = VK_TRUE;
        dss.depthWriteEnable = VK_FALSE;                          // depth read-only: transparent objects tested against opaque
        dss.depthCompareOp   = VK_COMPARE_OP_LESS_OR_EQUAL;
        // RT0: accum ONE+ONE additive
        VkPipelineColorBlendAttachmentState cba[2]{};
        cba[0].blendEnable         = VK_TRUE;
        cba[0].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cba[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cba[0].colorBlendOp        = VK_BLEND_OP_ADD;
        cba[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba[0].alphaBlendOp        = VK_BLEND_OP_ADD;
        cba[0].colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        // RT1: revealage ZERO+SRC_COLOR multiplicative
        cba[1].blendEnable         = VK_TRUE;
        cba[1].srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        cba[1].dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
        cba[1].colorBlendOp        = VK_BLEND_OP_ADD;
        cba[1].srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        cba[1].dstAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        cba[1].alphaBlendOp        = VK_BLEND_OP_ADD;
        cba[1].colorWriteMask      = VK_COLOR_COMPONENT_R_BIT;
        VkPipelineColorBlendStateCreateInfo cbs{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cbs.attachmentCount = 2; cbs.pAttachments = cba;
        VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dsi{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dsi.dynamicStateCount = 2; dsi.pDynamicStates = dyn;

        VkGraphicsPipelineCreateInfo gpi{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        gpi.stageCount = 2; gpi.pStages = stages;
        gpi.pVertexInputState = &vis; gpi.pInputAssemblyState = &ias;
        gpi.pViewportState    = &vps; gpi.pRasterizationState = &rs;
        gpi.pMultisampleState = &ms;  gpi.pDepthStencilState  = &dss;
        gpi.pColorBlendState  = &cbs; gpi.pDynamicState       = &dsi;
        gpi.layout = _oitFwdPipeLayout; gpi.renderPass = _oitFwdRenderPass; gpi.subpass = 0;

        VkResult r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, nullptr, &_oitFwdPipeline);
        vkDestroyShaderModule(dev, vsM, nullptr);
        vkDestroyShaderModule(dev, fsM, nullptr);
        if (r != VK_SUCCESS) { LUNA_LOG_ERROR("VK OIT: forward pipeline creation failed: %d", (int)r); return false; }
    }

    // OIT Composite pipeline (fullscreen, 1 HDR RT, src=ONE_MINUS_SRC_ALPHA dst=SRC_ALPHA)
    {
        std::vector<uint32_t> vsS, fsS;
        if (!CompileHLSLtoSPIRV(GetShaderFullPath(L"fullscreen.vert.hlsl").wstring(), L"vs_6_0", vsS) ||
            !CompileGLSLtoSPIRV(GetShaderFullPath(L"oit_composite_vk.frag.glsl").wstring(), fsS))
        {
            LUNA_LOG_ERROR("VK OIT: oit_composite_vk shader compile failed");
            return false;
        }
        VkShaderModule vsM = mkMod(vsS), fsM = mkMod(fsS);

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,   vsM, "main" };
        stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fsM, "main" };

        VkPipelineVertexInputStateCreateInfo   vis{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo ias{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        ias.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vps{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        vps.viewportCount = 1; vps.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo dss{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        dss.depthTestEnable = VK_FALSE;
        VkPipelineColorBlendAttachmentState cba{};
        cba.blendEnable         = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        cba.colorBlendOp        = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        cba.alphaBlendOp        = VK_BLEND_OP_ADD;
        cba.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cbs{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cbs.attachmentCount = 1; cbs.pAttachments = &cba;
        VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dsi{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dsi.dynamicStateCount = 2; dsi.pDynamicStates = dyn;

        VkGraphicsPipelineCreateInfo gpi{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        gpi.stageCount = 2; gpi.pStages = stages;
        gpi.pVertexInputState = &vis; gpi.pInputAssemblyState = &ias;
        gpi.pViewportState    = &vps; gpi.pRasterizationState = &rs;
        gpi.pMultisampleState = &ms;  gpi.pDepthStencilState  = &dss;
        gpi.pColorBlendState  = &cbs; gpi.pDynamicState       = &dsi;
        gpi.layout = _oitCmpPipeLayout; gpi.renderPass = _oitCmpRenderPass; gpi.subpass = 0;

        VkResult r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, nullptr, &_oitCmpPipeline);
        vkDestroyShaderModule(dev, vsM, nullptr);
        vkDestroyShaderModule(dev, fsM, nullptr);
        if (r != VK_SUCCESS) { LUNA_LOG_ERROR("VK OIT: composite pipeline creation failed: %d", (int)r); return false; }
    }

    _vkOitReady = true;
    LUNA_LOG_INFO("VK OIT: WBOIT initialized — transparent object support active");
    return true;
}

void VulkanBackend::DestroyVKOITResources()
{
    if (!_vkOitReady && !_oitFwdPipeline) return;
    VkDevice dev = _device->GetDevice();

    if (_oitFwdPipeline)    { vkDestroyPipeline(dev, _oitFwdPipeline, nullptr);            _oitFwdPipeline    = VK_NULL_HANDLE; }
    if (_oitCmpPipeline)    { vkDestroyPipeline(dev, _oitCmpPipeline, nullptr);            _oitCmpPipeline    = VK_NULL_HANDLE; }
    if (_oitFwdPipeLayout)  { vkDestroyPipelineLayout(dev, _oitFwdPipeLayout, nullptr);    _oitFwdPipeLayout  = VK_NULL_HANDLE; }
    if (_oitCmpPipeLayout)  { vkDestroyPipelineLayout(dev, _oitCmpPipeLayout, nullptr);    _oitCmpPipeLayout  = VK_NULL_HANDLE; }
    if (_oitLinearSampler)  { vkDestroySampler(dev, _oitLinearSampler, nullptr);           _oitLinearSampler  = VK_NULL_HANDLE; }
    if (_oitPointSampler)   { vkDestroySampler(dev, _oitPointSampler, nullptr);            _oitPointSampler   = VK_NULL_HANDLE; }
    if (_oitDescPool)       { vkDestroyDescriptorPool(dev, _oitDescPool, nullptr);         _oitDescPool       = VK_NULL_HANDLE; }
    if (_oitCmpLayout)      { vkDestroyDescriptorSetLayout(dev, _oitCmpLayout, nullptr);   _oitCmpLayout      = VK_NULL_HANDLE; }
    if (_oitAlbedoLayout)   { vkDestroyDescriptorSetLayout(dev, _oitAlbedoLayout, nullptr);_oitAlbedoLayout   = VK_NULL_HANDLE; }
    if (_oitSceneLayout)    { vkDestroyDescriptorSetLayout(dev, _oitSceneLayout, nullptr); _oitSceneLayout    = VK_NULL_HANDLE; }
    if (_oitCmpFramebuffer) { vkDestroyFramebuffer(dev, _oitCmpFramebuffer, nullptr);      _oitCmpFramebuffer = VK_NULL_HANDLE; }
    if (_oitFwdFramebuffer) { vkDestroyFramebuffer(dev, _oitFwdFramebuffer, nullptr);      _oitFwdFramebuffer = VK_NULL_HANDLE; }
    if (_oitCmpRenderPass)  { vkDestroyRenderPass(dev, _oitCmpRenderPass, nullptr);        _oitCmpRenderPass  = VK_NULL_HANDLE; }
    if (_oitFwdRenderPass)  { vkDestroyRenderPass(dev, _oitFwdRenderPass, nullptr);        _oitFwdRenderPass  = VK_NULL_HANDLE; }
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; ++i)
    {
        _oitSceneUBOMapped[i] = nullptr;
        if (_oitSceneUBO[i])    { vkDestroyBuffer(dev, _oitSceneUBO[i], nullptr);    _oitSceneUBO[i]    = VK_NULL_HANDLE; }
        if (_oitSceneUBOMem[i]) { vkFreeMemory(dev, _oitSceneUBOMem[i], nullptr);    _oitSceneUBOMem[i] = VK_NULL_HANDLE; }
    }
    if (_oitAccumView)   { vkDestroyImageView(dev, _oitAccumView, nullptr);    _oitAccumView   = VK_NULL_HANDLE; }
    if (_oitRevealView)  { vkDestroyImageView(dev, _oitRevealView, nullptr);   _oitRevealView  = VK_NULL_HANDLE; }
    if (_oitAccumImage)  { vkDestroyImage(dev, _oitAccumImage, nullptr);       _oitAccumImage  = VK_NULL_HANDLE; }
    if (_oitRevealImage) { vkDestroyImage(dev, _oitRevealImage, nullptr);      _oitRevealImage = VK_NULL_HANDLE; }
    if (_oitAccumMem)    { vkFreeMemory(dev, _oitAccumMem, nullptr);           _oitAccumMem    = VK_NULL_HANDLE; }
    if (_oitRevealMem)   { vkFreeMemory(dev, _oitRevealMem, nullptr);          _oitRevealMem   = VK_NULL_HANDLE; }
    _vkOitReady = false;
}

void VulkanBackend::DrawVKOITForward(VkCommandBuffer cmd)
{
    // Update OITSceneData UBO with current view/proj and scene light data
    {
        static const float kSqrt6Inv = 1.0f / 2.449490f;
        OITSceneData sd{};
        memcpy(sd.view,      &_deferredView, 64);
        memcpy(sd.proj,      &_deferredProj, 64);
        sd.lightDir[0]   = kSqrt6Inv; sd.lightDir[1] = 2.0f * kSqrt6Inv; sd.lightDir[2] = kSqrt6Inv;
        sd.lightColor[0] = sd.lightColor[1] = sd.lightColor[2] = sd.lightColor[3] = 1.0f;
        memcpy(_oitSceneUBOMapped[_frameIndex], &sd, sizeof(OITSceneData));
    }

    const VkExtent2D ext = _vkSwapchain.GetExtent();

    // Clear: accum → (0,0,0,0), revealage → 1.0; depth uses LOAD_OP_LOAD (no clear entry needed)
    VkClearValue clearVals[2]{};
    clearVals[0].color = {{ 0.0f, 0.0f, 0.0f, 0.0f }};
    clearVals[1].color = {{ 1.0f, 0.0f, 0.0f, 0.0f }};

    VkRenderPassBeginInfo rbi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rbi.renderPass       = _oitFwdRenderPass;
    rbi.framebuffer      = _oitFwdFramebuffer;
    rbi.renderArea       = {{ 0, 0 }, ext };
    rbi.clearValueCount  = 2;
    rbi.pClearValues     = clearVals;
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _oitFwdPipeline);
    VkViewport vp{ 0.0f, 0.0f, (float)ext.width, (float)ext.height, 0.0f, 1.0f };
    VkRect2D   sc{ {0, 0}, ext };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
        _oitFwdPipeLayout, 0, 1, &_oitSceneDescSet[_frameIndex], 0, nullptr);

    // push_constant layout: mat4 model (64B) + float alpha (4B) + float[3] pad (12B) = 80B
    struct OITPushData { float model[16]; float alpha; float _pad[3]; };

    VkDevice dev = _device->GetDevice();
    for (uint32_t i = 0; i < (uint32_t)_vkOitMeshes.size(); ++i)
    {
        const auto& draw = _vkOitMeshes[i];
        const auto* m    = draw.mesh;
        if (!m || !m->material) continue;

        // Update albedo descriptor for this slot (safe: GPU hasn't executed yet)
        if (m->material->albedo.view)
        {
            VkDescriptorImageInfo ii{ _oitLinearSampler, m->material->albedo.view,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w.dstSet = _oitAlbedoDescSets[_frameIndex][i]; w.dstBinding = 0;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.descriptorCount = 1; w.pImageInfo = &ii;
            vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
        }

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            _oitFwdPipeLayout, 1, 1, &_oitAlbedoDescSets[_frameIndex][i], 0, nullptr);

        OITPushData pc{};
        memcpy(pc.model, &draw.model, 64);
        pc.alpha = draw.alpha;
        vkCmdPushConstants(cmd, _oitFwdPipeLayout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 80, &pc);

        VkDeviceSize off = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &m->vertexBuffer, &off);
        vkCmdBindIndexBuffer(cmd, m->indexBuffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, m->indexCount, 1, 0, 0, 0);
    }

    vkCmdEndRenderPass(cmd);
}

void VulkanBackend::DrawVKOITComposite(VkCommandBuffer cmd)
{
    const VkExtent2D ext = _vkSwapchain.GetExtent();

    VkRenderPassBeginInfo rbi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rbi.renderPass      = _oitCmpRenderPass;
    rbi.framebuffer     = _oitCmpFramebuffer;
    rbi.renderArea      = {{ 0, 0 }, ext };
    rbi.clearValueCount = 0;
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _oitCmpPipeline);
    VkViewport vp{ 0.0f, 0.0f, (float)ext.width, (float)ext.height, 0.0f, 1.0f };
    VkRect2D   sc{ {0, 0}, ext };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
        _oitCmpPipeLayout, 0, 1, &_oitCmpDescSet, 0, nullptr);

    vkCmdDraw(cmd, 3, 1, 0, 0);  // fullscreen triangle — no vertex buffer needed

    vkCmdEndRenderPass(cmd);
}

// ===========================================================================
// Phase 32: Visibility Buffer — Vulkan
// ===========================================================================

bool VulkanBackend::CreateVKVisibilityResources()
{
    if (!_gpuDrivenReady) return false;

    VkDevice      dev   = _device->GetDevice();
    VkExtent2D    ext   = _vkSwapchain.GetExtent();
    const uint32_t W = ext.width, H = ext.height;

    // ── 1. Visibility image (VK_FORMAT_R32_UINT) ─────────────────────────────
    if (!CreateImage(W, H, VK_FORMAT_R32_UINT, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, _visImage, _visImageMem))
        return false;
    _visImageView = CreateImageView(_visImage, VK_FORMAT_R32_UINT, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!_visImageView) return false;
    TransitionImageLayout(_visImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

    // ── 2. G-buffer storage views (GENERAL layout for compute UAV write) ──────
    auto makeStorageView = [&](VkImage img, VkFormat fmt) -> VkImageView {
        VkImageViewCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        ci.image    = img; ci.viewType = VK_IMAGE_VIEW_TYPE_2D; ci.format = fmt;
        ci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkImageView v = VK_NULL_HANDLE;
        vkCreateImageView(dev, &ci, nullptr, &v);
        return v;
    };
    _visGB0StorageView = makeStorageView(_gBuffer.GetAlbedoImage(),    VK_FORMAT_R8G8B8A8_UNORM);
    _visGB1StorageView = makeStorageView(_gBuffer.GetNormalImage(),     VK_FORMAT_R16G16B16A16_SFLOAT);
    _visGB2StorageView = makeStorageView(_gBuffer.GetMetalRoughImage(), VK_FORMAT_R8G8B8A8_UNORM);
    if (!_visGB0StorageView || !_visGB1StorageView || !_visGB2StorageView) return false;

    // ── 3. Vis render pass (R32_UINT color + depth LOAD_OP_CLEAR) ────────────
    {
        VkAttachmentDescription atts[2]{};
        atts[0].format = VK_FORMAT_R32_UINT; atts[0].samples = VK_SAMPLE_COUNT_1_BIT;
        atts[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; atts[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        atts[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[0].initialLayout = VK_IMAGE_LAYOUT_GENERAL;
        atts[0].finalLayout   = VK_IMAGE_LAYOUT_GENERAL;  // shade compute reads GENERAL

        atts[1].format = VK_FORMAT_D32_SFLOAT; atts[1].samples = VK_SAMPLE_COUNT_1_BIT;
        atts[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; atts[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        atts[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        atts[1].finalLayout   = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        VkAttachmentReference colRef{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkAttachmentReference depRef{ 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sp{};
        sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sp.colorAttachmentCount = 1; sp.pColorAttachments = &colRef;
        sp.pDepthStencilAttachment = &depRef;

        VkSubpassDependency dep{};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL; dep.dstSubpass = 0;
        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo rpi{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        rpi.attachmentCount = 2; rpi.pAttachments = atts;
        rpi.subpassCount = 1; rpi.pSubpasses = &sp;
        rpi.dependencyCount = 1; rpi.pDependencies = &dep;
        if (vkCreateRenderPass(dev, &rpi, nullptr, &_visRenderPass) != VK_SUCCESS) return false;
    }

    // ── 4. Framebuffer ────────────────────────────────────────────────────────
    {
        VkImageView atts[2] = { _visImageView, _vkSwapchain.GetDepthView() };
        VkFramebufferCreateInfo fi{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fi.renderPass = _visRenderPass; fi.attachmentCount = 2; fi.pAttachments = atts;
        fi.width = W; fi.height = H; fi.layers = 1;
        if (vkCreateFramebuffer(dev, &fi, nullptr, &_visFramebuffer) != VK_SUCCESS) return false;
    }

    // ── 5. Shade constants UBOs ───────────────────────────────────────────────
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; ++i)
    {
        if (!CreateBuffer(256,
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                _visShadeUBO[i], _visShadeUBOMem[i]))
            return false;
        vkMapMemory(dev, _visShadeUBOMem[i], 0, 256, 0, &_visShadeUBOMapped[i]);
    }

    // ── 6. Anisotropic sampler for shade compute ──────────────────────────────
    {
        VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        si.magFilter = VK_FILTER_LINEAR; si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        si.anisotropyEnable = VK_TRUE; si.maxAnisotropy = 8.0f;
        si.maxLod = VK_LOD_CLAMP_NONE;
        vkCreateSampler(dev, &si, nullptr, &_visShadeSampler);
    }

    // ── 7. Descriptor set layouts ─────────────────────────────────────────────
    // set=0: binding 0=UBO (UNIFORM_BUFFER), binding 1=vis RT (STORAGE_IMAGE)
    {
        VkDescriptorSetLayoutBinding bs[2]{};
        bs[0].binding = 0; bs[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bs[0].descriptorCount = 1; bs[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bs[1].binding = 1; bs[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bs[1].descriptorCount = 1; bs[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo li{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        li.bindingCount = 2; li.pBindings = bs;
        vkCreateDescriptorSetLayout(dev, &li, nullptr, &_visShadeSet0Layout);
    }
    // set=1: VB(SSBO) + IB(SSBO) + objects(SSBO) + meshInfos(SSBO)
    {
        VkDescriptorSetLayoutBinding bs[4]{};
        for (int i = 0; i < 4; ++i) {
            bs[i].binding = i; bs[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bs[i].descriptorCount = 1; bs[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo li{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        li.bindingCount = 4; li.pBindings = bs;
        vkCreateDescriptorSetLayout(dev, &li, nullptr, &_visShadeSet1Layout);
    }
    // set=2: GB0/1/2 as STORAGE_IMAGE UAVs
    {
        VkDescriptorSetLayoutBinding bs[3]{};
        for (int i = 0; i < 3; ++i) {
            bs[i].binding = i; bs[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bs[i].descriptorCount = 1; bs[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo li{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        li.bindingCount = 3; li.pBindings = bs;
        vkCreateDescriptorSetLayout(dev, &li, nullptr, &_visShadeSet2Layout);
    }

    // ── 8. Descriptor pool + sets ────────────────────────────────────────────
    {
        VkDescriptorPoolSize ps[] = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,  (uint32_t)FRAMES_IN_FLIGHT },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,   (uint32_t)(2 * FRAMES_IN_FLIGHT + 3) },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  4 },
        };
        VkDescriptorPoolCreateInfo pi{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pi.maxSets = FRAMES_IN_FLIGHT + 2; pi.poolSizeCount = 3; pi.pPoolSizes = ps;
        vkCreateDescriptorPool(dev, &pi, nullptr, &_visShadePool);

        // Allocate set0 per frame, set1 and set2 once
        VkDescriptorSetLayout layouts0[FRAMES_IN_FLIGHT];
        for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; ++i) layouts0[i] = _visShadeSet0Layout;
        VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = _visShadePool;
        ai.descriptorSetCount = FRAMES_IN_FLIGHT; ai.pSetLayouts = layouts0;
        vkAllocateDescriptorSets(dev, &ai, _visShadeSet0);

        VkDescriptorSetLayout layouts1[] = { _visShadeSet1Layout, _visShadeSet2Layout };
        VkDescriptorSet sets12[2];
        ai.descriptorSetCount = 2; ai.pSetLayouts = layouts1;
        vkAllocateDescriptorSets(dev, &ai, sets12);
        _visShadeSet1 = sets12[0]; _visShadeSet2 = sets12[1];
    }

    // ── 9. Write descriptors ─────────────────────────────────────────────────
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; ++i)
    {
        VkDescriptorBufferInfo uboInfo{ _visShadeUBO[i], 0, 256 };
        VkDescriptorImageInfo  visInfo{ VK_NULL_HANDLE, _visImageView, VK_IMAGE_LAYOUT_GENERAL };

        VkWriteDescriptorSet ws[2]{};
        ws[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        ws[0].dstSet = _visShadeSet0[i]; ws[0].dstBinding = 0;
        ws[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        ws[0].descriptorCount = 1; ws[0].pBufferInfo = &uboInfo;

        ws[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        ws[1].dstSet = _visShadeSet0[i]; ws[1].dstBinding = 1;
        ws[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        ws[1].descriptorCount = 1; ws[1].pImageInfo = &visInfo;
        vkUpdateDescriptorSets(dev, 2, ws, 0, nullptr);
    }
    // set1: VB, IB, objects, meshInfos
    {
        VkDescriptorBufferInfo bi[4] = {
            { _mergedVB,         0, VK_WHOLE_SIZE },
            { _mergedIB,         0, VK_WHOLE_SIZE },
            { _objectDataBuffer, 0, VK_WHOLE_SIZE },
            { _meshInfoBuf,      0, VK_WHOLE_SIZE },
        };
        VkWriteDescriptorSet ws[4]{};
        for (int j = 0; j < 4; ++j) {
            ws[j] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            ws[j].dstSet = _visShadeSet1; ws[j].dstBinding = j;
            ws[j].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            ws[j].descriptorCount = 1; ws[j].pBufferInfo = &bi[j];
        }
        vkUpdateDescriptorSets(dev, 4, ws, 0, nullptr);
    }
    // set2: G-buffer UAVs
    {
        VkDescriptorImageInfo ii[3] = {
            { VK_NULL_HANDLE, _visGB0StorageView, VK_IMAGE_LAYOUT_GENERAL },
            { VK_NULL_HANDLE, _visGB1StorageView, VK_IMAGE_LAYOUT_GENERAL },
            { VK_NULL_HANDLE, _visGB2StorageView, VK_IMAGE_LAYOUT_GENERAL },
        };
        VkWriteDescriptorSet ws[3]{};
        for (int j = 0; j < 3; ++j) {
            ws[j] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            ws[j].dstSet = _visShadeSet2; ws[j].dstBinding = j;
            ws[j].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            ws[j].descriptorCount = 1; ws[j].pImageInfo = &ii[j];
        }
        vkUpdateDescriptorSets(dev, 3, ws, 0, nullptr);
    }

    // ── 10. Vis pass graphics pipeline ───────────────────────────────────────
    // Reuses _indirectVSLayout (set=0: ViewProj UBO at binding=0, ObjectSSBO at binding=1)
    // — same layout as pbr_indirect_vk.vert.glsl, no push constants needed (gl_InstanceIndex)
    {
        VkDescriptorSetLayout setLayouts[] = { _indirectVSLayout };
        VkPipelineLayoutCreateInfo pli{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pli.setLayoutCount = 1; pli.pSetLayouts = setLayouts;
        vkCreatePipelineLayout(dev, &pli, nullptr, &_visPipeLayout);

        VkShaderModule vsM = VK_NULL_HANDLE, fsM = VK_NULL_HANDLE;
        std::vector<uint32_t> vsSpv, fsSpv;
        if (!CompileGLSLtoSPIRV(GetShaderFullPath(L"visibility_vk.vert.glsl").wstring(), vsSpv) ||
            !CompileGLSLtoSPIRV(GetShaderFullPath(L"visibility_vk.frag.glsl").wstring(), fsSpv))
        {
            LUNA_LOG_ERROR("Phase 32 VK: Failed to compile visibility shaders");
            return false;
        }

        auto mkModule = [&](const std::vector<uint32_t>& spv) {
            VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
            ci.codeSize = spv.size() * 4; ci.pCode = spv.data();
            VkShaderModule m = VK_NULL_HANDLE;
            vkCreateShaderModule(dev, &ci, nullptr, &m);
            return m;
        };
        vsM = mkModule(vsSpv); fsM = mkModule(fsSpv);

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vsM; stages[0].pName = "main";
        stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fsM; stages[1].pName = "main";

        // PBR vertex input layout (pos+normal+uv+tangent, stride=48)
        VkVertexInputBindingDescription vib{ 0, 48, VK_VERTEX_INPUT_RATE_VERTEX };
        VkVertexInputAttributeDescription via[4] = {
            { 0, 0, VK_FORMAT_R32G32B32_SFLOAT,  0  },  // position
            { 1, 0, VK_FORMAT_R32G32B32_SFLOAT,  12 },  // normal
            { 2, 0, VK_FORMAT_R32G32_SFLOAT,     24 },  // uv
            { 3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 32 }, // tangent
        };
        VkPipelineVertexInputStateCreateInfo vis_{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        vis_.vertexBindingDescriptionCount = 1; vis_.pVertexBindingDescriptions = &vib;
        vis_.vertexAttributeDescriptionCount = 4; vis_.pVertexAttributeDescriptions = via;

        VkPipelineInputAssemblyStateCreateInfo ias{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        ias.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vps{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        vps.viewportCount = 1; vps.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_BACK_BIT;
        rs.frontFace = VK_FRONT_FACE_CLOCKWISE; rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo dss{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        dss.depthTestEnable = VK_TRUE; dss.depthWriteEnable = VK_TRUE;
        dss.depthCompareOp = VK_COMPARE_OP_LESS;
        VkPipelineColorBlendAttachmentState cba{}; cba.colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo cbs{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cbs.attachmentCount = 1; cbs.pAttachments = &cba;
        VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dsi{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dsi.dynamicStateCount = 2; dsi.pDynamicStates = dyn;

        VkGraphicsPipelineCreateInfo gpi{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        gpi.stageCount = 2; gpi.pStages = stages;
        gpi.pVertexInputState = &vis_; gpi.pInputAssemblyState = &ias;
        gpi.pViewportState = &vps; gpi.pRasterizationState = &rs;
        gpi.pMultisampleState = &ms; gpi.pDepthStencilState = &dss;
        gpi.pColorBlendState = &cbs; gpi.pDynamicState = &dsi;
        gpi.layout = _visPipeLayout; gpi.renderPass = _visRenderPass; gpi.subpass = 0;
        vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, nullptr, &_visPipeline);

        vkDestroyShaderModule(dev, vsM, nullptr);
        vkDestroyShaderModule(dev, fsM, nullptr);
        if (!_visPipeline) return false;
    }

    // ── 11. Shade compute pipeline ────────────────────────────────────────────
    {
        VkDescriptorSetLayout setLayouts[] = {
            _visShadeSet0Layout, _visShadeSet1Layout, _visShadeSet2Layout,
            _indirectMaterialLayout,  // set=3: bindless material textures (same as indirect draw)
        };
        VkPipelineLayoutCreateInfo pli{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pli.setLayoutCount = 4; pli.pSetLayouts = setLayouts;
        vkCreatePipelineLayout(dev, &pli, nullptr, &_visShadeLayout);

        std::vector<uint32_t> spv;
        if (!CompileGLSLtoSPIRV(GetShaderFullPath(L"visibility_shade_vk.comp.glsl").wstring(), spv))
        {
            LUNA_LOG_ERROR("Phase 32 VK: Failed to compile visibility shade compute shader");
            return false;
        }
        VkShaderModuleCreateInfo mci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        mci.codeSize = spv.size() * 4; mci.pCode = spv.data();
        VkShaderModule csM = VK_NULL_HANDLE;
        vkCreateShaderModule(dev, &mci, nullptr, &csM);

        VkComputePipelineCreateInfo cpi{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        cpi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpi.stage.module = csM; cpi.stage.pName = "main";
        cpi.layout = _visShadeLayout;
        vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &_visShadePipeline);
        vkDestroyShaderModule(dev, csM, nullptr);
        if (!_visShadePipeline) return false;
    }

    _vkVisBufferReady = true;
    LUNA_LOG_INFO("Phase 32 VK: Visibility buffer ready (%ux%u)", W, H);
    return true;
}

void VulkanBackend::DestroyVKVisibilityResources()
{
    _vkVisBufferReady = false;
    _vkVisBufferMode  = false;
    VkDevice dev = _device->GetDevice();

    vkDestroyPipeline(dev, _visShadePipeline, nullptr); _visShadePipeline = VK_NULL_HANDLE;
    vkDestroyPipelineLayout(dev, _visShadeLayout, nullptr); _visShadeLayout = VK_NULL_HANDLE;
    vkDestroyPipeline(dev, _visPipeline, nullptr); _visPipeline = VK_NULL_HANDLE;
    vkDestroyPipelineLayout(dev, _visPipeLayout, nullptr); _visPipeLayout = VK_NULL_HANDLE;

    vkDestroyDescriptorPool(dev, _visShadePool, nullptr); _visShadePool = VK_NULL_HANDLE;
    vkDestroyDescriptorSetLayout(dev, _visShadeSet2Layout, nullptr); _visShadeSet2Layout = VK_NULL_HANDLE;
    vkDestroyDescriptorSetLayout(dev, _visShadeSet1Layout, nullptr); _visShadeSet1Layout = VK_NULL_HANDLE;
    vkDestroyDescriptorSetLayout(dev, _visShadeSet0Layout, nullptr); _visShadeSet0Layout = VK_NULL_HANDLE;

    if (_visShadeSampler) vkDestroySampler(dev, _visShadeSampler, nullptr); _visShadeSampler = VK_NULL_HANDLE;

    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; ++i)
    {
        if (_visShadeUBOMapped[i]) vkUnmapMemory(dev, _visShadeUBOMem[i]);
        if (_visShadeUBO[i])    vkDestroyBuffer(dev,       _visShadeUBO[i],    nullptr);
        if (_visShadeUBOMem[i]) vkFreeMemory(dev,          _visShadeUBOMem[i], nullptr);
        _visShadeUBO[i] = VK_NULL_HANDLE; _visShadeUBOMem[i] = VK_NULL_HANDLE;
        _visShadeUBOMapped[i] = nullptr;
    }

    vkDestroyFramebuffer(dev, _visFramebuffer, nullptr); _visFramebuffer = VK_NULL_HANDLE;
    vkDestroyRenderPass(dev,  _visRenderPass,  nullptr); _visRenderPass  = VK_NULL_HANDLE;

    if (_visGB2StorageView) vkDestroyImageView(dev, _visGB2StorageView, nullptr); _visGB2StorageView = VK_NULL_HANDLE;
    if (_visGB1StorageView) vkDestroyImageView(dev, _visGB1StorageView, nullptr); _visGB1StorageView = VK_NULL_HANDLE;
    if (_visGB0StorageView) vkDestroyImageView(dev, _visGB0StorageView, nullptr); _visGB0StorageView = VK_NULL_HANDLE;
    if (_visImageView) vkDestroyImageView(dev, _visImageView, nullptr); _visImageView = VK_NULL_HANDLE;
    if (_visImage)     vkDestroyImage(dev,     _visImage,     nullptr); _visImage     = VK_NULL_HANDLE;
    if (_visImageMem)  vkFreeMemory(dev,       _visImageMem,  nullptr); _visImageMem  = VK_NULL_HANDLE;
}

void VulkanBackend::DrawVKVisibilityPass(VkCommandBuffer cmd)
{
    if (!_visPipeline || !_visRenderPass || !_visFramebuffer) return;

    VkExtent2D ext = _vkSwapchain.GetExtent();

    // Transition G-buffer images to GENERAL (shade compute will write them)
    VkImageMemoryBarrier gbBarriers[3]{};
    auto makeBarrier = [](VkImage img, VkImageLayout oldL, VkImageLayout newL) {
        VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b.oldLayout = oldL; b.newLayout = newL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img; b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        return b;
    };
    gbBarriers[0] = makeBarrier(_gBuffer.GetAlbedoImage(),    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    gbBarriers[1] = makeBarrier(_gBuffer.GetNormalImage(),     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    gbBarriers[2] = makeBarrier(_gBuffer.GetMetalRoughImage(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 3, gbBarriers);

    // Vis render pass: clear vis RT + depth
    VkClearValue clears[2]{};
    clears[0].color.uint32[0] = 0u;  // sentinel: packed==0 means sky (objectIdx stored as objectIdx+1)
    clears[1].depthStencil = { 1.0f, 0 };

    VkRenderPassBeginInfo rbi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rbi.renderPass = _visRenderPass; rbi.framebuffer = _visFramebuffer;
    rbi.renderArea = { {0,0}, ext }; rbi.clearValueCount = 2; rbi.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _visPipeline);
    VkViewport vp{ 0,0,(float)ext.width,(float)ext.height,0,1 };
    VkRect2D sc{ {0,0}, ext };
    vkCmdSetViewport(cmd, 0, 1, &vp); vkCmdSetScissor(cmd, 0, 1, &sc);

    // Bind the same descriptor set as the GPU-driven G-buffer path:
    // set=0 binding=0: ViewProj UBO, binding=1: GPUObjectData SSBO
    // gl_InstanceIndex = firstInstance (objectIndex) from each VkDrawIndexedIndirectCommand
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
        _visPipeLayout, 0, 1, &_indirectVSDescSet, 0, nullptr);

    // Merged VB/IB — same buffers as GPU-driven path
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &_mergedVB, &off);
    vkCmdBindIndexBuffer(cmd, _mergedIB, 0, VK_INDEX_TYPE_UINT32);

    // Reuse the GPU-cull output indirect arg buffer — already filled by GPU cull in FlushDraws
    // stride = sizeof(VkDrawIndexedIndirectCommand) = 20B
    vkCmdDrawIndexedIndirectCount(cmd,
        _indirectArgBuffer[_frameIndex], 0,
        _drawCountBuffer[_frameIndex],   0,
        (uint32_t)_cpuInstances.size(),  20u);

    vkCmdEndRenderPass(cmd);
}

void VulkanBackend::DispatchVKVisibilityShade(VkCommandBuffer cmd)
{
    if (!_visShadePipeline || !_visShadeLayout) return;

    VkExtent2D ext = _vkSwapchain.GetExtent();

    // Update per-frame shade UBO
    struct VisShadeUBO {
        float view[16], proj[16], viewProj[16];
        uint32_t screenW, screenH, numObjects, _pad;
    };
    VisShadeUBO ubo{};
    memcpy(ubo.view,     &_deferredView, 64);
    memcpy(ubo.proj,     &_deferredProj, 64);
    XMMATRIX vp = XMLoadFloat4x4(&_deferredView) * XMLoadFloat4x4(&_deferredProj);
    XMFLOAT4X4 vpF; XMStoreFloat4x4(&vpF, vp);
    memcpy(ubo.viewProj, &vpF, 64);
    ubo.screenW = ext.width; ubo.screenH = ext.height;
    ubo.numObjects = (uint32_t)_cpuInstances.size();
    memcpy(_visShadeUBOMapped[_frameIndex], &ubo, sizeof(ubo));

    // Barrier: vis image GENERAL → GENERAL (no layout change, but ensure write visibility)
    // G-buffer images are already in GENERAL from DrawVKVisibilityPass
    VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 1, &mb, 0, nullptr, 0, nullptr);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _visShadePipeline);

    VkDescriptorSet sets[] = {
        _visShadeSet0[_frameIndex],
        _visShadeSet1,
        _visShadeSet2,
        _indirectMaterialSet,  // set=3: bindless material textures
    };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        _visShadeLayout, 0, 4, sets, 0, nullptr);

    vkCmdDispatch(cmd, (ext.width + 7) / 8, (ext.height + 7) / 8, 1);

    // Restore G-buffer to SHADER_READ_ONLY_OPTIMAL for render graph
    VkImageMemoryBarrier restoreBarriers[3]{};
    auto makeRestore = [](VkImage img) {
        VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img; b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        return b;
    };
    restoreBarriers[0] = makeRestore(_gBuffer.GetAlbedoImage());
    restoreBarriers[1] = makeRestore(_gBuffer.GetNormalImage());
    restoreBarriers[2] = makeRestore(_gBuffer.GetMetalRoughImage());
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 3, restoreBarriers);
}

// ===========================================================================
// S2b — Vulkan Camera Sensor Rendering
// ===========================================================================

// Helper: create a VkShaderModule from compiled SPIR-V
static VkShaderModule MakeShaderModule(VkDevice dev, const std::vector<uint32_t>& spv)
{
    VkShaderModuleCreateInfo si{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    si.codeSize = spv.size() * 4;
    si.pCode    = spv.data();
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &si, nullptr, &m);
    return m;
}

bool VulkanBackend::CreateSensorLightingPipeline()
{
    VkDevice dev = _device->GetDevice();

    // ── Descriptor set layouts ────────────────────────────────────────────────
    // set=0: sensor scene UBO
    {
        VkDescriptorSetLayoutBinding b{ 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                        VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        VkDescriptorSetLayoutCreateInfo li{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        li.bindingCount = 1; li.pBindings = &b;
        if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &_sensorSceneLayout) != VK_SUCCESS) return false;
    }
    // set=1: G-buffer samplers (4 COMBINED_IMAGE_SAMPLER)
    {
        VkDescriptorSetLayoutBinding bs[4]{};
        for (uint32_t i = 0; i < 4; ++i)
        {
            bs[i].binding        = i;
            bs[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bs[i].descriptorCount= 1;
            bs[i].stageFlags     = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo li{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        li.bindingCount = 4; li.pBindings = bs;
        if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &_sensorGBufLayout) != VK_SUCCESS) return false;
    }
    // set=2: IBL (3 COMBINED_IMAGE_SAMPLER: irrMap, prefilterMap, brdfLUT)
    {
        VkDescriptorSetLayoutBinding bs[3]{};
        for (uint32_t i = 0; i < 3; ++i)
        {
            bs[i].binding        = i;
            bs[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bs[i].descriptorCount= 1;
            bs[i].stageFlags     = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo li{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        li.bindingCount = 3; li.pBindings = bs;
        if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &_sensorIBLLayout) != VK_SUCCESS) return false;
    }

    // ── Pipeline layout ───────────────────────────────────────────────────────
    VkDescriptorSetLayout setLayouts[] = { _sensorSceneLayout, _sensorGBufLayout, _sensorIBLLayout };
    VkPipelineLayoutCreateInfo pli{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 3; pli.pSetLayouts = setLayouts;
    if (vkCreatePipelineLayout(dev, &pli, nullptr, &_sensorLitPipeLayout) != VK_SUCCESS) return false;

    // ── Render pass for litRT (RGBA16F, LOAD_OP_CLEAR → SHADER_READ_ONLY) ────
    // We create a single shared render pass here for litRT.
    // Each sensor has its own litRT framebuffer but reuses this render pass.
    // (Stored in _sensorLitRP, a member not in the per-camera struct — we'll create one
    //  per-camera inside InitVKCameraResources to match that camera's litImage.)

    // ── Shaders ───────────────────────────────────────────────────────────────
    std::vector<uint32_t> vsS, fsS;
    if (!CompileHLSLtoSPIRV(GetShaderFullPath(L"fullscreen.vert.hlsl").wstring(), L"vs_6_0", vsS))
    {
        LUNA_LOG_ERROR("S2b: fullscreen.vert.hlsl compile failed");
        return false;
    }
    if (!CompileGLSLtoSPIRV(GetShaderFullPath(L"sensor_lighting_vk.frag.glsl").wstring(), fsS))
    {
        LUNA_LOG_ERROR("S2b: sensor_lighting_vk.frag.glsl compile failed");
        return false;
    }

    VkShaderModule vsM = MakeShaderModule(dev, vsS);
    VkShaderModule fsM = MakeShaderModule(dev, fsS);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,   vsM, "main" };
    stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fsM, "main" };

    // No vertex input (fullscreen triangle generated in VS)
    VkPipelineVertexInputStateCreateInfo vis{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ias{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ias.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vps{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vps.viewportCount = 1; vps.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cbs{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cbs.attachmentCount = 1; cbs.pAttachments = &cba;
    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dsi{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dsi.dynamicStateCount = 2; dsi.pDynamicStates = dyn;

    // Render pass: 1 color attachment (RGBA16F litRT) — create a temporary one here;
    // per-camera litRP will be created identically in InitVKCameraResources.
    VkAttachmentDescription att{};
    att.format         = VK_FORMAT_R16G16B16A16_SFLOAT;
    att.samples        = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference ref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp{};
    sp.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1;
    sp.pColorAttachments    = &ref;
    VkRenderPassCreateInfo rpi{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpi.attachmentCount = 1; rpi.pAttachments = &att;
    rpi.subpassCount    = 1; rpi.pSubpasses   = &sp;
    VkRenderPass tempRP = VK_NULL_HANDLE;
    vkCreateRenderPass(dev, &rpi, nullptr, &tempRP);

    VkGraphicsPipelineCreateInfo gpi{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gpi.stageCount          = 2;
    gpi.pStages             = stages;
    gpi.pVertexInputState   = &vis;
    gpi.pInputAssemblyState = &ias;
    gpi.pViewportState      = &vps;
    gpi.pRasterizationState = &rs;
    gpi.pMultisampleState   = &ms;
    gpi.pColorBlendState    = &cbs;
    gpi.pDynamicState       = &dsi;
    gpi.layout              = _sensorLitPipeLayout;
    gpi.renderPass          = tempRP;
    gpi.subpass             = 0;

    VkResult r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, nullptr, &_sensorLitPipeline);

    vkDestroyShaderModule(dev, vsM, nullptr);
    vkDestroyShaderModule(dev, fsM, nullptr);
    vkDestroyRenderPass(dev, tempRP, nullptr);  // pipeline keeps its own copy internally

    if (r != VK_SUCCESS)
    {
        LUNA_LOG_ERROR("S2b: sensor lighting pipeline create failed: %d", (int)r);
        return false;
    }
    LUNA_LOG_INFO("S2b: sensor lighting pipeline ready");
    return true;
}

bool VulkanBackend::CreateSensorDistortPipeline()
{
    VkDevice dev = _device->GetDevice();

    // set=0: DistortUBO
    {
        VkDescriptorSetLayoutBinding b{ 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        VkDescriptorSetLayoutCreateInfo li{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        li.bindingCount = 1; li.pBindings = &b;
        if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &_sensorDistUBOLayout) != VK_SUCCESS) return false;
    }
    // set=1: litRT sampler (COMBINED_IMAGE_SAMPLER, compute reads)
    {
        VkDescriptorSetLayoutBinding b{ 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        VkDescriptorSetLayoutCreateInfo li{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        li.bindingCount = 1; li.pBindings = &b;
        if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &_sensorDistInputLayout) != VK_SUCCESS) return false;
    }
    // set=2: distortRT storage image (STORAGE_IMAGE, compute writes)
    {
        VkDescriptorSetLayoutBinding b{ 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                                        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        VkDescriptorSetLayoutCreateInfo li{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        li.bindingCount = 1; li.pBindings = &b;
        if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &_sensorDistOutLayout) != VK_SUCCESS) return false;
    }

    VkDescriptorSetLayout setLayouts[] = { _sensorDistUBOLayout, _sensorDistInputLayout, _sensorDistOutLayout };
    VkPipelineLayoutCreateInfo pli{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 3; pli.pSetLayouts = setLayouts;
    if (vkCreatePipelineLayout(dev, &pli, nullptr, &_sensorDistPipeLayout) != VK_SUCCESS) return false;

    std::vector<uint32_t> csS;
    if (!CompileGLSLtoSPIRV(GetShaderFullPath(L"camera_distort_vk.comp.glsl").wstring(), csS))
    {
        LUNA_LOG_ERROR("S2b: camera_distort_vk.comp.glsl compile failed");
        return false;
    }

    VkShaderModule csM = MakeShaderModule(dev, csS);
    VkPipelineShaderStageCreateInfo stage{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = csM;
    stage.pName = "main";

    VkComputePipelineCreateInfo cpi{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    cpi.stage  = stage;
    cpi.layout = _sensorDistPipeLayout;

    VkResult res = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &_sensorDistPipeline);
    vkDestroyShaderModule(dev, csM, nullptr);

    if (res != VK_SUCCESS)
    {
        LUNA_LOG_ERROR("S2b: sensor distort compute pipeline failed: %d", (int)res);
        return false;
    }
    LUNA_LOG_INFO("S2b: sensor distort compute pipeline ready");
    return true;
}

bool VulkanBackend::CreateSensorIBLDescriptorSet()
{
    if (!_ibl.IsReady() || _sensorIBLLayout == VK_NULL_HANDLE) return false;
    VkDevice dev = _device->GetDevice();

    VkDescriptorPoolSize ps{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 };
    VkDescriptorPoolCreateInfo pi{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pi.maxSets = 1; pi.poolSizeCount = 1; pi.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(dev, &pi, nullptr, &_sensorIBLPool) != VK_SUCCESS) return false;

    VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool     = _sensorIBLPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts        = &_sensorIBLLayout;
    if (vkAllocateDescriptorSets(dev, &ai, &_sensorIBLSet) != VK_SUCCESS) return false;

    VkSampler iblSampler = _ibl.GetIBLSampler();
    VkDescriptorImageInfo iis[3] = {
        { iblSampler, _ibl.GetIrradianceView(),  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
        { iblSampler, _ibl.GetPrefilterView(),   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
        { iblSampler, _ibl.GetBRDFLUTView(),     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
    };
    VkWriteDescriptorSet wrs[3]{};
    for (uint32_t i = 0; i < 3; ++i)
    {
        wrs[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wrs[i].dstSet          = _sensorIBLSet;
        wrs[i].dstBinding      = i;
        wrs[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wrs[i].descriptorCount = 1;
        wrs[i].pImageInfo      = &iis[i];
    }
    vkUpdateDescriptorSets(dev, 3, wrs, 0, nullptr);
    LUNA_LOG_INFO("S2b: sensor IBL descriptor set ready");
    return true;
}

bool VulkanBackend::InitVKCameraResources(CameraSensor* cam)
{
    if (!cam) return false;
    VkDevice dev = _device->GetDevice();
    VulkanCameraResources& r = _vkCameraRTs[cam];
    if (r.ready) return true;

    const uint32_t W = cam->config.width;
    const uint32_t H = cam->config.height;

    // ── 1. Depth image (D32_SFLOAT) ──────────────────────────────────────────
    if (!CreateImage(W, H, VK_FORMAT_D32_SFLOAT, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, r.depthImage, r.depthMemory))
    { LUNA_LOG_ERROR("S2b: depth image failed"); return false; }
    r.depthView = CreateImageView(r.depthImage, VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT);

    // ── 2. VulkanGBuffer (3 MRTs + external depth) ───────────────────────────
    {
        VulkanGBuffer::CreateInfo gbCI{};
        gbCI.core       = &_core;
        gbCI.extent     = { W, H };
        gbCI.depthView  = r.depthView;
        gbCI.depthFormat= VK_FORMAT_D32_SFLOAT;
        if (!r.gbuffer.Create(gbCI))
        { LUNA_LOG_ERROR("S2b: VulkanGBuffer create failed"); return false; }
    }

    // ── 3. Lit HDR image (RGBA16F) ────────────────────────────────────────────
    if (!CreateImage(W, H, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, r.litImage, r.litMemory))
    { LUNA_LOG_ERROR("S2b: litRT image failed"); return false; }
    r.litView = CreateImageView(r.litImage, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT);

    // Render pass for litRT
    {
        VkAttachmentDescription att{};
        att.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp  = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout   = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkAttachmentReference ref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sp{};
        sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sp.colorAttachmentCount = 1; sp.pColorAttachments = &ref;
        VkRenderPassCreateInfo rpi{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        rpi.attachmentCount = 1; rpi.pAttachments = &att;
        rpi.subpassCount    = 1; rpi.pSubpasses   = &sp;
        vkCreateRenderPass(dev, &rpi, nullptr, &r.litRP);

        VkFramebufferCreateInfo fi{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fi.renderPass = r.litRP; fi.attachmentCount = 1; fi.pAttachments = &r.litView;
        fi.width = W; fi.height = H; fi.layers = 1;
        vkCreateFramebuffer(dev, &fi, nullptr, &r.litFB);
    }

    // ── 4. Distorted output image (RGBA8, GENERAL layout for storage write) ──
    if (!CreateImage(W, H, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, r.distortImage, r.distortMemory))
    { LUNA_LOG_ERROR("S2b: distortRT image failed"); return false; }
    r.distortView = CreateImageView(r.distortImage, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);

    // Transition distortImage to GENERAL layout (storage image)
    {
        VkCommandBuffer tc = BeginSingleTimeCommands();
        VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.srcAccessMask = 0; b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = r.distortImage; b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(tc, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);
        EndSingleTimeCommands(tc);
    }

    // ── 5. CPU staging buffer ─────────────────────────────────────────────────
    {
        VkDeviceSize sz = W * H * 4;
        if (!CreateBuffer(sz, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                r.stagingRGB, r.stagingMem))
        { LUNA_LOG_ERROR("S2b: staging buffer failed"); return false; }
        vkMapMemory(dev, r.stagingMem, 0, sz, 0, &r.stagingMapped);
    }

    // ── 6. Sensor MVP UBO for G-buffer fill (triple-buffered, per-draw dynamic) ──
    {
        VkDeviceSize sz = VulkanCameraResources::MAX_DRAWS * 256ull * FRAMES_IN_FLIGHT;
        if (!CreateBuffer(sz, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                r.sensorMVPBuf, r.sensorMVPMem))
        { LUNA_LOG_ERROR("S2b: MVP UBO failed"); return false; }
        vkMapMemory(dev, r.sensorMVPMem, 0, sz, 0, &r.sensorMVPMapped);
    }

    // Allocate mvpDescSets from _mvpDescLayout (existing layout: binding=0 DYNAMIC, binding=1 STATIC)
    {
        VkDescriptorPoolSize ps[2] = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, FRAMES_IN_FLIGHT },
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         FRAMES_IN_FLIGHT },
        };
        VkDescriptorPoolCreateInfo pi{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pi.maxSets = FRAMES_IN_FLIGHT; pi.poolSizeCount = 2; pi.pPoolSizes = ps;
        vkCreateDescriptorPool(dev, &pi, nullptr, &r.gbufPool);

        for (uint32_t fi = 0; fi < FRAMES_IN_FLIGHT; ++fi)
        {
            VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ai.descriptorPool = r.gbufPool; ai.descriptorSetCount = 1;
            ai.pSetLayouts = &_mvpDescLayout;
            vkAllocateDescriptorSets(dev, &ai, &r.gbufSets[fi]);

            VkDescriptorBufferInfo mvpBI{
                r.sensorMVPBuf,
                0, 192  // range = sizeof(model+view+proj) = 3 * 64B
            };
            // scene UBO: reuse _deferredSceneCB[fi] (eyePos + lightDir, not needed for G-buf fill)
            VkDescriptorBufferInfo sceneBI{ _deferredSceneCB[fi], 0, 48 };
            VkWriteDescriptorSet wrs[2]{};
            wrs[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wrs[0].dstSet = r.gbufSets[fi]; wrs[0].dstBinding = 0;
            wrs[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            wrs[0].descriptorCount = 1; wrs[0].pBufferInfo = &mvpBI;
            wrs[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wrs[1].dstSet = r.gbufSets[fi]; wrs[1].dstBinding = 1;
            wrs[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            wrs[1].descriptorCount = 1; wrs[1].pBufferInfo = &sceneBI;
            vkUpdateDescriptorSets(dev, 2, wrs, 0, nullptr);
        }
    }

    // ── 7. Sensor lighting descriptor sets ───────────────────────────────────
    // Create a sampler for G-buffer + litRT reads
    {
        VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        si.magFilter = VK_FILTER_LINEAR; si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = 1.0f;
        vkCreateSampler(dev, &si, nullptr, &r.litSampler);
    }

    // Descriptor pool for lit scene + lit gbuf + distort inputs/outputs
    {
        VkDescriptorPoolSize ps[] = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,          FRAMES_IN_FLIGHT },  // lit scene UBO
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,  4 + FRAMES_IN_FLIGHT + 1 }, // gbuf(4) + distortIn(3) + litSet(1)
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,           1 },  // distortOut
        };
        VkDescriptorPoolCreateInfo pi{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pi.maxSets = FRAMES_IN_FLIGHT + 3; // litSceneSets + gbufSet + distortSets
        pi.poolSizeCount = 3; pi.pPoolSizes = ps;
        vkCreateDescriptorPool(dev, &pi, nullptr, &r.litPool);
    }

    // Per-frame scene UBOs + litSceneSets (set=0)
    for (uint32_t fi = 0; fi < FRAMES_IN_FLIGHT; ++fi)
    {
        if (!CreateBuffer(256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                r.litSceneUBO[fi], r.litSceneUBOMem[fi]))
        { LUNA_LOG_ERROR("S2b: litSceneUBO[%u] failed", fi); return false; }
        vkMapMemory(dev, r.litSceneUBOMem[fi], 0, 256, 0, &r.litSceneUBOMapped[fi]);

        VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = r.litPool; ai.descriptorSetCount = 1;
        ai.pSetLayouts = &_sensorSceneLayout;
        vkAllocateDescriptorSets(dev, &ai, &r.litSceneSets[fi]);

        VkDescriptorBufferInfo bi{ r.litSceneUBO[fi], 0, 256 };
        VkWriteDescriptorSet wr{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        wr.dstSet = r.litSceneSets[fi]; wr.dstBinding = 0;
        wr.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        wr.descriptorCount = 1; wr.pBufferInfo = &bi;
        vkUpdateDescriptorSets(dev, 1, &wr, 0, nullptr);
    }

    // G-buffer samplers set (set=1, static — updated in firstRender)
    {
        VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = r.litPool; ai.descriptorSetCount = 1;
        ai.pSetLayouts = &_sensorGBufLayout;
        vkAllocateDescriptorSets(dev, &ai, &r.litGBufSet);
        // Will be written in RenderVKCameraSensorInternal on firstRender
    }

    // Distort UBOs + distortUBOSets
    for (uint32_t fi = 0; fi < FRAMES_IN_FLIGHT; ++fi)
    {
        if (!CreateBuffer(256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                r.distortUBO[fi], r.distortUBOMem[fi]))
        { LUNA_LOG_ERROR("S2b: distortUBO[%u] failed", fi); return false; }
        vkMapMemory(dev, r.distortUBOMem[fi], 0, 256, 0, &r.distortUBOMapped[fi]);

        VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = r.distortPool != VK_NULL_HANDLE ? r.distortPool : r.litPool;
        ai.descriptorSetCount = 1; ai.pSetLayouts = &_sensorDistUBOLayout;
        vkAllocateDescriptorSets(dev, &ai, &r.distortUBOSets[fi]);

        VkDescriptorBufferInfo bi{ r.distortUBO[fi], 0, 256 };
        VkWriteDescriptorSet wr{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        wr.dstSet = r.distortUBOSets[fi]; wr.dstBinding = 0;
        wr.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        wr.descriptorCount = 1; wr.pBufferInfo = &bi;
        vkUpdateDescriptorSets(dev, 1, &wr, 0, nullptr);
    }

    // distortLitSet (set=1: litRT sampler) + distortOutSet (set=2: distortRT storage)
    {
        VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = r.litPool; ai.descriptorSetCount = 1;
        ai.pSetLayouts = &_sensorDistInputLayout;
        vkAllocateDescriptorSets(dev, &ai, &r.distortLitSet);
        // Written on firstRender

        ai.pSetLayouts = &_sensorDistOutLayout;
        vkAllocateDescriptorSets(dev, &ai, &r.distortOutSet);

        VkDescriptorImageInfo distOutII{ VK_NULL_HANDLE, r.distortView, VK_IMAGE_LAYOUT_GENERAL };
        VkWriteDescriptorSet wr{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        wr.dstSet = r.distortOutSet; wr.dstBinding = 0;
        wr.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        wr.descriptorCount = 1; wr.pImageInfo = &distOutII;
        vkUpdateDescriptorSets(dev, 1, &wr, 0, nullptr);
    }

    // ImGui texture ID (distortRT as COMBINED_IMAGE_SAMPLER via ImGui Vulkan backend)
    r.imguiSet = ImGui_ImplVulkan_AddTexture(r.litSampler, r.distortView,
                                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    cam->gpuTextureHandle = reinterpret_cast<uint64_t>(r.imguiSet);

    r.width = W; r.height = H;
    r.ready = true; r.firstRender = true;
    LUNA_LOG_INFO("S2b: VK camera '%s' resources ready (%ux%u)", cam->GetName().c_str(), W, H);
    return true;
}

void VulkanBackend::DestroyVKCameraResources(CameraSensor* cam)
{
    auto it = _vkCameraRTs.find(cam);
    if (it == _vkCameraRTs.end()) return;
    VulkanCameraResources& r = it->second;
    VkDevice dev = _device->GetDevice();

    vkDeviceWaitIdle(dev);

    r.gbuffer.Destroy();

    // Descriptor pools (frees all allocated sets)
    if (r.gbufPool)  { vkDestroyDescriptorPool(dev, r.gbufPool,  nullptr); r.gbufPool  = VK_NULL_HANDLE; }
    if (r.litPool)   { vkDestroyDescriptorPool(dev, r.litPool,   nullptr); r.litPool   = VK_NULL_HANDLE; }
    if (r.distortPool){ vkDestroyDescriptorPool(dev, r.distortPool, nullptr); r.distortPool = VK_NULL_HANDLE; }

    if (r.litSampler) { vkDestroySampler(dev, r.litSampler, nullptr); r.litSampler = VK_NULL_HANDLE; }

    // Lit RT resources
    if (r.litFB)   { vkDestroyFramebuffer(dev, r.litFB, nullptr); r.litFB = VK_NULL_HANDLE; }
    if (r.litRP)   { vkDestroyRenderPass(dev, r.litRP, nullptr);  r.litRP = VK_NULL_HANDLE; }
    if (r.litView) { vkDestroyImageView(dev, r.litView, nullptr); r.litView = VK_NULL_HANDLE; }
    if (r.litImage){ vkDestroyImage(dev, r.litImage, nullptr);    r.litImage = VK_NULL_HANDLE; }
    if (r.litMemory){ vkFreeMemory(dev, r.litMemory, nullptr);    r.litMemory = VK_NULL_HANDLE; }

    // Distort RT
    if (r.distortView)  { vkDestroyImageView(dev, r.distortView, nullptr); r.distortView = VK_NULL_HANDLE; }
    if (r.distortImage) { vkDestroyImage(dev, r.distortImage, nullptr);    r.distortImage = VK_NULL_HANDLE; }
    if (r.distortMemory){ vkFreeMemory(dev, r.distortMemory, nullptr);     r.distortMemory = VK_NULL_HANDLE; }

    // Depth
    if (r.depthView)  { vkDestroyImageView(dev, r.depthView, nullptr);  r.depthView = VK_NULL_HANDLE; }
    if (r.depthImage) { vkDestroyImage(dev, r.depthImage, nullptr);     r.depthImage = VK_NULL_HANDLE; }
    if (r.depthMemory){ vkFreeMemory(dev, r.depthMemory, nullptr);      r.depthMemory = VK_NULL_HANDLE; }

    // MVP UBO
    if (r.sensorMVPMapped && r.sensorMVPBuf) { vkUnmapMemory(dev, r.sensorMVPMem); r.sensorMVPMapped = nullptr; }
    if (r.sensorMVPBuf) { vkDestroyBuffer(dev, r.sensorMVPBuf, nullptr); r.sensorMVPBuf = VK_NULL_HANDLE; }
    if (r.sensorMVPMem) { vkFreeMemory(dev, r.sensorMVPMem, nullptr);    r.sensorMVPMem = VK_NULL_HANDLE; }

    // Staging
    if (r.stagingMapped && r.stagingMem) { vkUnmapMemory(dev, r.stagingMem); r.stagingMapped = nullptr; }
    if (r.stagingRGB) { vkDestroyBuffer(dev, r.stagingRGB, nullptr); r.stagingRGB = VK_NULL_HANDLE; }
    if (r.stagingMem) { vkFreeMemory(dev, r.stagingMem, nullptr);    r.stagingMem = VK_NULL_HANDLE; }

    // Per-frame UBOs
    for (uint32_t fi = 0; fi < FRAMES_IN_FLIGHT; ++fi)
    {
        auto freeUBO = [&](VkBuffer& b, VkDeviceMemory& m, void*& mp) {
            if (mp && m) { vkUnmapMemory(dev, m); mp = nullptr; }
            if (b) { vkDestroyBuffer(dev, b, nullptr); b = VK_NULL_HANDLE; }
            if (m) { vkFreeMemory(dev, m, nullptr);   m = VK_NULL_HANDLE; }
        };
        freeUBO(r.litSceneUBO[fi],  r.litSceneUBOMem[fi],  r.litSceneUBOMapped[fi]);
        freeUBO(r.distortUBO[fi],   r.distortUBOMem[fi],   r.distortUBOMapped[fi]);
    }

    cam->gpuTextureHandle = 0;
    r.ready = false;
    _vkCameraRTs.erase(it);
}

void VulkanBackend::RenderVKCameraSensorInternal(CameraSensor* cam, VkCommandBuffer cmd, uint32_t fi)
{
    auto it = _vkCameraRTs.find(cam);
    if (it == _vkCameraRTs.end() || !it->second.ready) return;
    VulkanCameraResources& r = it->second;
    VkDevice dev = _device->GetDevice();

    const uint32_t W = r.width;
    const uint32_t H = r.height;

    // ── Update G-buffer sampler set (first render or rebuild) ────────────────
    if (r.firstRender)
    {
        VkDescriptorImageInfo gii[4] = {
            { r.litSampler, r.gbuffer.GetAlbedoView(),    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
            { r.litSampler, r.gbuffer.GetNormalView(),    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
            { r.litSampler, r.gbuffer.GetMetalRoughView(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
            { r.litSampler, r.depthView,                  VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL },
        };
        VkWriteDescriptorSet wrs[4]{};
        for (uint32_t i = 0; i < 4; ++i)
        {
            wrs[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wrs[i].dstSet = r.litGBufSet; wrs[i].dstBinding = i;
            wrs[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            wrs[i].descriptorCount = 1; wrs[i].pImageInfo = &gii[i];
        }
        vkUpdateDescriptorSets(dev, 4, wrs, 0, nullptr);

        // distortLitSet (litRT as sampler)
        VkDescriptorImageInfo litII{ r.litSampler, r.litView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet litWr{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        litWr.dstSet = r.distortLitSet; litWr.dstBinding = 0;
        litWr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        litWr.descriptorCount = 1; litWr.pImageInfo = &litII;
        vkUpdateDescriptorSets(dev, 1, &litWr, 0, nullptr);
    }

    // ── Update sensor lighting scene UBO ─────────────────────────────────────
    struct SensorSceneUBO {
        float invViewProj[16];
        float eyePos[4];
        float lightDir[4];
        float lightColor[4];
    } sceneUBO{};
    {
        XMMATRIX V = XMLoadFloat4x4(&cam->GetViewMatrix());
        XMMATRIX P = XMLoadFloat4x4(&cam->GetProjMatrix());
        XMFLOAT4X4 invVP;
        XMStoreFloat4x4(&invVP, XMMatrixInverse(nullptr, XMMatrixMultiply(V, P)));
        memcpy(sceneUBO.invViewProj, &invVP, 64);
    }
    const XMFLOAT4X4& sw = cam->GetSensorWorld();
    sceneUBO.eyePos[0]   = sw._41; sceneUBO.eyePos[1] = sw._42; sceneUBO.eyePos[2] = sw._43; sceneUBO.eyePos[3] = 1.0f;
    sceneUBO.lightDir[0] = _cachedLightDir.x; sceneUBO.lightDir[1] = _cachedLightDir.y;
    sceneUBO.lightDir[2] = _cachedLightDir.z; sceneUBO.lightDir[3] = 0.0f;
    sceneUBO.lightColor[0] = _cachedLightColor.x; sceneUBO.lightColor[1] = _cachedLightColor.y;
    sceneUBO.lightColor[2] = _cachedLightColor.z; sceneUBO.lightColor[3] = _cachedLightColor.w;
    memcpy(r.litSceneUBOMapped[fi], &sceneUBO, sizeof(sceneUBO));

    // ── Step 1: G-buffer fill ─────────────────────────────────────────────────
    {
        VkClearValue clears[4]{};
        clears[3].depthStencil = { 1.0f, 0 };
        VkRenderPassBeginInfo rbi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        rbi.renderPass = r.gbuffer.GetRenderPass();
        rbi.framebuffer = r.gbuffer.GetFramebuffer();
        rbi.renderArea = { {0,0}, {W, H} };
        rbi.clearValueCount = 4; rbi.pClearValues = clears;
        vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _gbPipeline);
        VkViewport vp{ 0,0,(float)W,(float)H,0,1 };
        VkRect2D sc{ {0,0},{W,H} };
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);

        struct MVPData { XMFLOAT4X4 model, view, proj; };
        uint32_t drawIdx = fi * VulkanCameraResources::MAX_DRAWS;
        uint8_t* mvpBase = static_cast<uint8_t*>(r.sensorMVPMapped);

        for (auto& mesh : _vkSceneMeshes)
        {
            if (!mesh || !mesh->material) continue;
            if (drawIdx >= (fi + 1) * VulkanCameraResources::MAX_DRAWS) break;

            MVPData mvp{};
            DirectX::XMStoreFloat4x4(&mvp.model, DirectX::XMMatrixIdentity());
            mvp.view = cam->GetViewMatrix();
            mvp.proj = cam->GetProjMatrix();
            memcpy(mvpBase + drawIdx * 256, &mvp, sizeof(MVPData));

            uint32_t dynOff = drawIdx * 256;
            VkDescriptorSet sets[] = { r.gbufSets[fi], mesh->material->descSet };
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    _pipelineLayout, 0, 2, sets, 1, &dynOff);

            VkDeviceSize off = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &mesh->vertexBuffer, &off);
            vkCmdBindIndexBuffer(cmd, mesh->indexBuffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, mesh->indexCount, 1, 0, 0, 0);
            drawIdx++;
        }

        vkCmdEndRenderPass(cmd);
    }

    // G-buffer → SHADER_READ_ONLY (the render pass's finalLayout handles this automatically
    // since att.finalLayout = SHADER_READ_ONLY_OPTIMAL in VulkanGBuffer::CreateRenderPasses())

    // ── Step 2: Sensor lighting pass (IBL-only deferred) ─────────────────────
    if (_sensorLitPipeline && _sensorIBLSet)
    {
        VkClearValue cv{}; cv.color = { {0.05f, 0.07f, 0.1f, 1.0f} };
        VkRenderPassBeginInfo rbi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        rbi.renderPass = r.litRP; rbi.framebuffer = r.litFB;
        rbi.renderArea = { {0,0},{W,H} };
        rbi.clearValueCount = 1; rbi.pClearValues = &cv;
        vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _sensorLitPipeline);
        VkViewport vp{ 0,0,(float)W,(float)H,0,1 };
        VkRect2D sc{ {0,0},{W,H} };
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);

        VkDescriptorSet sets[] = { r.litSceneSets[fi], r.litGBufSet, _sensorIBLSet };
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                _sensorLitPipeLayout, 0, 3, sets, 0, nullptr);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
        // litRP finalLayout = SHADER_READ_ONLY_OPTIMAL — ready for distort sampler
    }

    // ── Step 3: Distortion compute ────────────────────────────────────────────
    if (_sensorDistPipeline)
    {
        // Update distort UBO
        struct DistortUBO {
            float k1,k2,k3,p1,p2,fx,fy,cx,cy;
            float exposureEV100, shotNoiseFactor, readNoiseSigma;
            uint32_t screenW, screenH, _p0, _p1;
        } du{};
        du.k1 = cam->config.k1; du.k2 = cam->config.k2; du.k3 = cam->config.k3;
        du.p1 = cam->config.p1; du.p2 = cam->config.p2;
        auto intr = CameraIntrinsics::FromConfig(cam->config);
        du.fx = intr.fx; du.fy = intr.fy; du.cx = intr.cx; du.cy = intr.cy;
        du.exposureEV100   = cam->config.exposureEV100;
        du.shotNoiseFactor = cam->config.shotNoiseFactor;
        du.readNoiseSigma  = cam->config.readNoiseSigma;
        du.screenW = W; du.screenH = H;
        memcpy(r.distortUBOMapped[fi], &du, sizeof(du));

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _sensorDistPipeline);
        VkDescriptorSet sets[] = { r.distortUBOSets[fi], r.distortLitSet, r.distortOutSet };
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                _sensorDistPipeLayout, 0, 3, sets, 0, nullptr);
        vkCmdDispatch(cmd, (W + 7) / 8, (H + 7) / 8, 1);

        // Barrier: compute write → transfer read
        VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = r.distortImage; b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);

        // Copy distortImage → stagingRGB
        VkBufferImageCopy region{};
        region.bufferRowLength = W; region.bufferImageHeight = H;
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent = { W, H, 1 };
        vkCmdCopyImageToBuffer(cmd, r.distortImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               r.stagingRGB, 1, &region);

        // Restore distortImage → GENERAL for next frame
        b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);
    }

    // Restore litImage → COLOR_ATTACHMENT for next frame
    // (litRP finalLayout = SHADER_READ_ONLY_OPTIMAL — need to restore for next CLEAR pass)
    {
        VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = r.litImage; b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);
    }

    cam->hasNewFrame = true;
    r.firstRender    = false;
}

void VulkanBackend::RenderCameraSensors()
{
    if (!_sensorLitPipeline || !_sensorDistPipeline) return;

    auto* sceneMgr = SceneManager::GetInstance();
    if (!sceneMgr) return;
    auto activeScene = sceneMgr->GetActiveScene();
    if (!activeScene) return;

    VkCommandBuffer cmd = _frames[_frameIndex].cmdBuffer;
    const uint32_t fi   = _frameIndex;

    for (auto& go : activeScene->GetGameObjects())
    {
        if (!go) continue;
        auto comp = go->GetComponentByType(ComponentType::SENSOR);
        auto* sc  = comp ? static_cast<SensorComponent*>(comp.get()) : nullptr;
        if (!sc) continue;

        for (auto& sensor : sc->GetSensors())
        {
            if (!sensor || sensor->GetType() != SensorType::Camera) continue;
            auto* cam = static_cast<CameraSensor*>(sensor.get());
            if (!cam->enabled) continue;

            // Lazy init or resolution change
            auto it = _vkCameraRTs.find(cam);
            bool needInit = (it == _vkCameraRTs.end() || !it->second.ready ||
                             it->second.width != cam->config.width ||
                             it->second.height != cam->config.height);
            if (needInit)
            {
                if (it != _vkCameraRTs.end()) DestroyVKCameraResources(cam);
                if (!InitVKCameraResources(cam)) continue;
            }

            // Map previous frame's readback → fill pointCloud
            auto& r = _vkCameraRTs[cam];
            if (!r.firstRender && r.stagingMapped)
            {
                const uint32_t pixCount = r.width * r.height;
                cam->rgbOutput.resize(pixCount * 4);
                memcpy(cam->rgbOutput.data(), r.stagingMapped, pixCount * 4);
            }

            RenderVKCameraSensorInternal(cam, cmd, fi);
        }
    }
}

} // namespace Luna
