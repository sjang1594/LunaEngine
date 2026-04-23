#include "LunaPCH.h"
#include "LunaEngine/Renderer/Vulkan/Public/VulkanBackend.h"
#include "Logger/Logger.h"
#include "Renderer/Vulkan/Public/VulkanDevice.h"
#include "Renderer/Mesh.h"
#include "LunaEngine/Utils/FileSystemUtil.h"
#include "stb_image.h"
#include "cgltf.h"
#include <algorithm>
#include <random>
#include <set>

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
    if (!CreateSwapchain(width, height)) return false;
    if (!CreateDepthResources())        return false;
    if (!CreateRenderPass())            return false;
    if (!CreateFramebuffers())          return false;
    if (!CreateFrameResources())        return false;
    if (!CreatePipeline())              return false;
    if (!CreateGBufferResources())      return false;
    if (!CreateCSMResources())          { LUNA_LOG_WARN("VK: CSM init failed ??shadows disabled"); }
    if (!CreateSSAOResources())         { LUNA_LOG_WARN("VK: SSAO init failed ??AO disabled"); }
    if (!CreateDeferredPipeline())      return false;
    if (!CreatePPResources())           { LUNA_LOG_WARN("VK: PP resources failed ??SSR disabled"); }
    return true;
}

// ===========================================================================
// Shutdown
// ===========================================================================
void VulkanBackend::Shutdown()
{
    if (!_device || _device->GetDevice() == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(_device->GetDevice());
    ShutdownImGui();

    VkDevice dev = _device->GetDevice();
    for (auto& m : _vkSceneMeshes) {
        if (!m) continue;
        if (m->vertexBuffer) vkDestroyBuffer(dev, m->vertexBuffer, nullptr);
        if (m->vertexMemory) vkFreeMemory(dev, m->vertexMemory, nullptr);
        if (m->indexBuffer)  vkDestroyBuffer(dev, m->indexBuffer, nullptr);
        if (m->indexMemory)  vkFreeMemory(dev, m->indexMemory, nullptr);
        if (m->material) {
            auto& mat = *m->material;
            auto dt = [&](VkTexture& t) {
                if (t.view)   vkDestroyImageView(dev, t.view, nullptr);
                if (t.image)  vkDestroyImage(dev, t.image, nullptr);
                if (t.memory) vkFreeMemory(dev, t.memory, nullptr);
            };
            dt(mat.albedo); dt(mat.normalMap); dt(mat.metalRough);
            if (mat.ubo)    vkDestroyBuffer(dev, mat.ubo, nullptr);
            if (mat.uboMem) vkFreeMemory(dev, mat.uboMem, nullptr);
        }
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
        if (_frames[i].cmdPool)     vkDestroyCommandPool(dev, _frames[i].cmdPool, nullptr);
        if (_frames[i].fence)       vkDestroyFence(dev, _frames[i].fence, nullptr);
        if (_frames[i].imageReady)  vkDestroySemaphore(dev, _frames[i].imageReady, nullptr);
        if (_frames[i].renderDone)  vkDestroySemaphore(dev, _frames[i].renderDone, nullptr);
    }
    if (_imguiDescriptorPool) { vkDestroyDescriptorPool(dev, _imguiDescriptorPool, nullptr); _imguiDescriptorPool = VK_NULL_HANDLE; }
    DestroyDepthResources();
    DestroySwapchain();
    if (_renderPass)         { vkDestroyRenderPass(dev, _renderPass,         nullptr); _renderPass         = VK_NULL_HANDLE; }
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

    auto& frame  = _frames[_frameIndex];
    VkDevice dev = _device->GetDevice();

    // Wait for this frame's fence (from FRAMES_IN_FLIGHT ago)
    vkWaitForFences(dev, 1, &frame.fence, VK_TRUE, UINT64_MAX);

    VkResult r = vkAcquireNextImageKHR(dev, _swapchain, UINT64_MAX,
                                        frame.imageReady, VK_NULL_HANDLE, &_imageIndex);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
    {
        vkDeviceWaitIdle(dev);
        RecreateSwapchain();
        return;
    }
    if (r != VK_SUCCESS) { LUNA_LOG_ERROR("vkAcquireNextImageKHR failed: %d", (int)r); return; }

    // Wait for any previous frame that was using this swapchain image
    if (_imagesInFlight[_imageIndex] != VK_NULL_HANDLE)
    {
        vkWaitForFences(dev, 1, &_imagesInFlight[_imageIndex], VK_TRUE, UINT64_MAX);
    }
    // Mark this image as being used by this frame's fence
    _imagesInFlight[_imageIndex] = frame.fence;

    vkResetFences(dev, 1, &frame.fence);

    // Update ping-pong index and per-frame TAA descriptor (after fence ??safe to update GPU resources)
    UpdatePPDescriptors();

    vkResetCommandBuffer(frame.cmdBuffer, 0);

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(frame.cmdBuffer, &bi);

    // CSM shadow depth pre-pass (uses previous frame's cached model matrices)
    if (_csmPipeline && !_lastMeshModels.empty())
    {
        UpdateCSMMatrices(_lastView, _lastProj);
        DrawCSMPass(frame.cmdBuffer);
    }

    // G-buffer pass: 3 colour clears + depth clear
    VkClearValue gbClears[4]{};
    gbClears[3].depthStencil = { 1.0f, 0 };   // depth clear; colour targets default to 0

    VkRenderPassBeginInfo rpi{};
    rpi.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpi.renderPass        = _gbRenderPass;
    rpi.framebuffer       = _gbFramebuffer;
    rpi.renderArea.extent = _swapchainExtent;
    rpi.clearValueCount   = 4;
    rpi.pClearValues      = gbClears;
    vkCmdBeginRenderPass(frame.cmdBuffer, &rpi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{ 0, 0,
        (float)_swapchainExtent.width, (float)_swapchainExtent.height, 0, 1 };
    vkCmdSetViewport(frame.cmdBuffer, 0, 1, &vp);

    VkRect2D sc{ {0,0}, _swapchainExtent };
    vkCmdSetScissor(frame.cmdBuffer, 0, 1, &sc);

    _frameActive = true;
}

void VulkanBackend::DrawFrame() {} // SceneManager calls DrawMesh directly

// CompositeFrame: end G-buffer pass ??deferred lighting ??open ImGui pass
void VulkanBackend::CompositeFrame()
{
    if (!_frameActive) return;
    auto& frame = _frames[_frameIndex];
    VkCommandBuffer cmd = frame.cmdBuffer;
    VkDevice dev = _device->GetDevice();

    // ?�?� End G-buffer render pass ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
    // G-buffer colour attachments transition to SHADER_READ_ONLY_OPTIMAL,
    // depth transitions to DEPTH_STENCIL_READ_ONLY_OPTIMAL (via render pass finalLayout).
    vkCmdEndRenderPass(cmd);

    // ?�?� SSAO passes (between G-buffer end and deferred lighting) ?�?�?�?�?�?�?�?�?�?�?�?�
    if (_ssaoPipeline) {
        DrawSSAOPass(cmd);      // raw SSAO ??_ssaoRTImage (finalLayout = SHADER_READ_ONLY)
        DrawSSAOBlurPass(cmd);  // blur ??_ssaoBlurImage  (finalLayout = SHADER_READ_ONLY)
    }

    // ?�?� Phase 18D: RT shadow ray dispatch (after SSAO, before deferred lighting) ?�?�
    // depth is in DEPTH_STENCIL_READ_ONLY_OPTIMAL, normals in SHADER_READ_ONLY_OPTIMAL
    if (_rtSupported && _vkRTPipeline != VK_NULL_HANDLE && _vkShadowMaskImage != VK_NULL_HANDLE)
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
            RTSceneUBO ubo{};
            memcpy(ubo.invViewProj, &iVPF, 64);
            ubo.lightDir[0] = kSqrt6Inv; ubo.lightDir[1] = 2.0f * kSqrt6Inv; ubo.lightDir[2] = kSqrt6Inv;
            ubo.maxDist = 100.0f;
            memcpy(_vkRTSceneCBMapped[_frameIndex], &ubo, sizeof(ubo));
        }

        // Transition shadow mask: whatever ??GENERAL (storage write)
        VkImageMemoryBarrier rtBarrier{};
        rtBarrier.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        rtBarrier.srcAccessMask    = 0;
        rtBarrier.dstAccessMask    = VK_ACCESS_SHADER_WRITE_BIT;
        rtBarrier.oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED;
        rtBarrier.newLayout        = VK_IMAGE_LAYOUT_GENERAL;
        rtBarrier.image            = _vkShadowMaskImage;
        rtBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            0, 0, nullptr, 0, nullptr, 1, &rtBarrier);

        // Trace shadow rays
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, _vkRTPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, _vkRTPipeLayout,
                                 0, 1, &_vkRTDescSet[_frameIndex], 0, nullptr);
        pfn_vkCmdTraceRaysKHR(cmd, &_vkRgenRegion, &_vkMissRegion, &_vkHitRegion, &_vkCallRegion,
                               _swapchainExtent.width, _swapchainExtent.height, 1);

        // Transition shadow mask: GENERAL ??SHADER_READ_ONLY for deferred fragment reads
        rtBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        rtBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        rtBarrier.oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
        rtBarrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &rtBarrier);
    }

    // ?�?� Update deferred scene UBO ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
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

        // CSM data: viewMatrix, lightVP[4], cascadeSplits
        memcpy(ubo.viewMatrix, &_deferredView, 64);
        for (uint32_t c = 0; c < CSM_CASCADE_COUNT; c++)
            memcpy(ubo.lightVP[c], &_csmLightVP[c], 64);
        memcpy(ubo.cascadeSplits, _csmSplits, 16);

        // Phase 18D: enable RT shadow in shader when pipeline and shadow mask are ready
        ubo.rtEnabled = (_rtSupported && _vkRTPipeline != VK_NULL_HANDLE
                         && _vkShadowMaskView != VK_NULL_HANDLE) ? 1u : 0u;

        memcpy(_deferredSceneCBMapped[_frameIndex], &ubo, sizeof(ubo));
    }

    // ?�?� Deferred lighting ??HDR intermediate (_ppRenderPass / _deferredHDRFramebuffer) ?�?�
    // Phase 16C: deferred pipeline is compiled for _ppRenderPass (R16G16B16A16_SFLOAT).
    // PP resources are required; if missing, open ImGui pass and return early (black frame).
    bool ppReady = _vkPPResourcesValid && _deferredHDRFramebuffer && _vkSSRTonemapPipeline;
    if (!ppReady)
    {
        // PP resources not ready ??open ImGui pass and bail (black frame as error state)
        VkRenderPassBeginInfo irpi{};
        irpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        irpi.renderPass = _renderPass; irpi.framebuffer = _framebuffers[_imageIndex];
        irpi.renderArea.extent = _swapchainExtent; irpi.clearValueCount = 0;
        vkCmdBeginRenderPass(cmd, &irpi, VK_SUBPASS_CONTENTS_INLINE);
        return;
    }

    VkClearValue ltClear{};
    ltClear.color.float32[3] = 1.0f;

    {
        VkRenderPassBeginInfo lrpi{};
        lrpi.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        lrpi.renderPass        = _ppRenderPass;
        lrpi.framebuffer       = _deferredHDRFramebuffer;
        lrpi.renderArea.extent = _swapchainExtent;
        lrpi.clearValueCount   = 1;
        lrpi.pClearValues      = &ltClear;
        vkCmdBeginRenderPass(cmd, &lrpi, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport vp{ 0, 0, (float)_swapchainExtent.width, (float)_swapchainExtent.height, 0, 1 };
        vkCmdSetViewport(cmd, 0, 1, &vp);
        VkRect2D sc{ {0,0}, _swapchainExtent };
        vkCmdSetScissor(cmd, 0, 1, &sc);

        // Phase 15C: use IBL pipeline when environment is loaded
        VkPipeline activeDeferredPipeline = (_iblReady && _deferredIBLPipeline)
                                            ? _deferredIBLPipeline : _deferredPipeline;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, activeDeferredPipeline);
        VkDescriptorSet dSets[] = { _deferredSceneDescSet[_frameIndex], _deferredGbufDescSet };
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 _deferredPipeLayout, 0, 2, dSets, 0, nullptr);
        vkCmdDraw(cmd, 3, 1, 0, 0);  // fullscreen triangle (SV_VertexID)
        vkCmdEndRenderPass(cmd);
        // _ppRenderPass finalLayout = SHADER_READ_ONLY_OPTIMAL ??_hdrImage now readable
    }

    if (ppReady)
    {
        // ?�?� SSR compute dispatch (optional ??skipped if pipeline missing) ?�?�
        // _ssrImage is kept in GENERAL permanently; only a memory barrier needed before reads.
        if (_vkSSRPipeline)
        {
            // Upload SSR constants for this frame
            {
                // SSR cbuffer layout must match ssr_vk.comp.hlsl SSRConstants (256B)
                struct SSRConstants {
                    float view[16];
                    float proj[16];
                    float invViewProj[16];
                    float eyePos[3]; float maxDistance;
                    uint32_t screenW; uint32_t screenH; uint32_t maxSteps; float stepSize;
                    float thickness; float maxRoughness; float _pad0[2];
                    float _pad1[4];
                };
                static_assert(sizeof(SSRConstants) == 256, "SSRConstants must be 256B");

                XMMATRIX V   = XMLoadFloat4x4(&_deferredView);
                XMMATRIX P   = XMLoadFloat4x4(&_deferredProj);
                XMMATRIX VP  = XMMatrixMultiply(V, P);
                XMVECTOR det = XMMatrixDeterminant(VP);
                XMMATRIX iVP = XMMatrixInverse(&det, VP);
                XMFLOAT4X4 iVPF; XMStoreFloat4x4(&iVPF, iVP);

                float negTx = -_deferredView._41, negTy = -_deferredView._42, negTz = -_deferredView._43;
                float eyeX  = _deferredView._11*negTx + _deferredView._12*negTy + _deferredView._13*negTz;
                float eyeY  = _deferredView._21*negTx + _deferredView._22*negTy + _deferredView._23*negTz;
                float eyeZ  = _deferredView._31*negTx + _deferredView._32*negTy + _deferredView._33*negTz;

                SSRConstants cb{};
                memcpy(cb.view,        &_deferredView, 64);
                memcpy(cb.proj,        &_deferredProj, 64);
                memcpy(cb.invViewProj, &iVPF,          64);
                cb.eyePos[0] = eyeX; cb.eyePos[1] = eyeY; cb.eyePos[2] = eyeZ;
                cb.maxDistance = 50.0f;
                cb.screenW = _swapchainExtent.width; cb.screenH = _swapchainExtent.height;
                cb.maxSteps  = 32; cb.stepSize  = 0.05f;
                cb.thickness = 0.1f; cb.maxRoughness = 0.6f;
                memcpy(_vkSSRCBMapped[_frameIndex], &cb, 256);
            }

            // Barrier: ensure _hdrImage write complete; _ssrImage ready for storage write
            VkImageMemoryBarrier barriers[2]{};
            barriers[0].sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barriers[0].srcAccessMask    = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barriers[0].dstAccessMask    = VK_ACCESS_SHADER_READ_BIT;
            barriers[0].oldLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barriers[0].newLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barriers[0].image            = _hdrImage;
            barriers[0].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

            barriers[1].sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barriers[1].srcAccessMask    = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            barriers[1].dstAccessMask    = VK_ACCESS_SHADER_WRITE_BIT;
            barriers[1].oldLayout        = VK_IMAGE_LAYOUT_GENERAL;
            barriers[1].newLayout        = VK_IMAGE_LAYOUT_GENERAL;
            barriers[1].image            = _ssrImage;
            barriers[1].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

            vkCmdPipelineBarrier(cmd,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0, 0, nullptr, 0, nullptr, 2, barriers);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _vkSSRPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                     _vkSSRPipeLayout, 0, 1, &_vkSSRDescSet[_frameIndex], 0, nullptr);
            uint32_t gx = (_swapchainExtent.width  + 7) / 8;
            uint32_t gy = (_swapchainExtent.height + 7) / 8;
            vkCmdDispatch(cmd, gx, gy, 1);

            // Memory barrier: SSR write complete before tonemap reads
            VkMemoryBarrier mb{};
            mb.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                0, 1, &mb, 0, nullptr, 0, nullptr);
        }
        else
        {
            // No SSR pipeline: just ensure _hdrImage is readable for tonemap
            VkImageMemoryBarrier b{};
            b.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.srcAccessMask    = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            b.dstAccessMask    = VK_ACCESS_SHADER_READ_BIT;
            b.oldLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b.newLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b.image            = _hdrImage;
            b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdPipelineBarrier(cmd,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &b);
        }

        // ?�?� Post-process chain ??swapchain ?�?�
        if (_vkTAAPipeline)
        {
            // Phase 18B: motion blur (HDR ??mbRT), runs before TAA
            if (_vkMBPipeline && _mbView) DrawVKMotionBlurPass();

            // Phase 17 full PP: TAA ??Bloom ??Full Tonemap (TAA + Bloom + SSR ??swapchain)
            DrawVKTAAPass();
            DrawVKBloomBrightPass();
            DrawVKBloomBlurPass(true);   // H-blur
            DrawVKBloomBlurPass(false);  // V-blur
            DrawVKTonemapPass();
        }
        else
        {
            // Phase 16C fallback: simple HDR + SSR ??swapchain
            VkRenderPassBeginInfo trpi{};
            trpi.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            trpi.renderPass        = _tonemapRenderPass;
            trpi.framebuffer       = _tonemapFramebuffers[_imageIndex];
            trpi.renderArea.extent = _swapchainExtent;
            trpi.clearValueCount   = 1;
            trpi.pClearValues      = &ltClear;
            vkCmdBeginRenderPass(cmd, &trpi, VK_SUBPASS_CONTENTS_INLINE);

            VkViewport vp{ 0, 0, (float)_swapchainExtent.width, (float)_swapchainExtent.height, 0, 1 };
            vkCmdSetViewport(cmd, 0, 1, &vp);
            VkRect2D sc{ {0,0}, _swapchainExtent };
            vkCmdSetScissor(cmd, 0, 1, &sc);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _vkSSRTonemapPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                     _vkSSRTonemapPipeLayout, 0, 1, &_vkSSRTonemapDescSet, 0, nullptr);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRenderPass(cmd);
        }
    }

    // ?�?� Open ImGui overlay render pass (_renderPass, LOAD from swapchain) ?�?�
    VkRenderPassBeginInfo irpi{};
    irpi.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    irpi.renderPass        = _renderPass;
    irpi.framebuffer       = _framebuffers[_imageIndex];
    irpi.renderArea.extent = _swapchainExtent;
    irpi.clearValueCount   = 0;
    vkCmdBeginRenderPass(cmd, &irpi, VK_SUBPASS_CONTENTS_INLINE);
    // ImGui_ImplVulkan_RenderDrawData will record into this open pass via RenderImGui().
}

void VulkanBackend::EndFrame()
{
    if (!_frameActive) return;
    auto& frame = _frames[_frameIndex];
    vkCmdEndRenderPass(frame.cmdBuffer);   // ends the ImGui overlay pass opened in CompositeFrame
    vkEndCommandBuffer(frame.cmdBuffer);

    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{};
    si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount   = 1;
    si.pWaitSemaphores      = &frame.imageReady;
    si.pWaitDstStageMask    = &ws;
    si.commandBufferCount   = 1;
    si.pCommandBuffers      = &frame.cmdBuffer;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores    = &frame.renderDone;
    vkQueueSubmit(_device->GetGraphicsQueue(), 1, &si, frame.fence);

    VkPresentInfoKHR pi{};
    pi.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores    = &frame.renderDone;
    pi.swapchainCount     = 1;
    pi.pSwapchains        = &_swapchain;
    pi.pImageIndices      = &_imageIndex;
    VkResult r = vkQueuePresentKHR(_device->GetPresentQueue(), &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        vkDeviceWaitIdle(_device->GetDevice());
        RecreateSwapchain();
    }
    _frameIndex = (_frameIndex + 1) % FRAMES_IN_FLIGHT;
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
    }

    // Phase 10: Halton(2,3) jitter ??sub-pixel TAA offset on projection matrix.
    _vkPrevJitter[0] = _vkCurJitter[0];
    _vkPrevJitter[1] = _vkCurJitter[1];
    if (_vkTAAPipeline) {
        auto Halton = [](int i, int b) -> float {
            float r = 0.0f, f = 1.0f / float(b);
            for (; i > 0; i /= b, f /= float(b)) r += f * float(i % b);
            return r;
        };
        int idx = int(_vkFrameCount % 64);
        _vkCurJitter[0] = (Halton(idx + 1, 2) - 0.5f) * 2.0f / float(_swapchainExtent.width);
        _vkCurJitter[1] = (Halton(idx + 1, 3) - 0.5f) * 2.0f / float(_swapchainExtent.height);

        // Apply jitter to both projections (G-buffer + deferred lighting must match)
        _lastProj._31     += _vkCurJitter[0];
        _lastProj._32     += _vkCurJitter[1];
        _deferredProj._31 += _vkCurJitter[0];
        _deferredProj._32 += _vkCurJitter[1];
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

    for (cgltf_size mi = 0; mi < data->meshes_count; mi++) {
        for (cgltf_size pi = 0; pi < data->meshes[mi].primitives_count; pi++) {
            const cgltf_primitive& prim = data->meshes[mi].primitives[pi];
            if (prim.type != cgltf_primitive_type_triangles) continue;

            cgltf_accessor *posA=nullptr, *nrmA=nullptr, *uvA=nullptr, *tanA=nullptr;
            for (cgltf_size ai = 0; ai < prim.attributes_count; ai++) {
                auto& a = prim.attributes[ai];
                if      (a.type==cgltf_attribute_type_position)                posA=a.data;
                else if (a.type==cgltf_attribute_type_normal)                  nrmA=a.data;
                else if (a.type==cgltf_attribute_type_texcoord && a.index==0)  uvA =a.data;
                else if (a.type==cgltf_attribute_type_tangent)                 tanA=a.data;
            }
            if (!posA) continue;

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

            allVerts.push_back(std::move(verts));
            allIdxs.push_back(std::move(idxs));

            auto d = std::make_shared<Mesh>(); d->indexCount = sm->indexCount;
            dummy.push_back(d);
        }
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
        LUNA_LOG_INFO("VK: Phase 15B GPU-driven ready (%zu meshes)", _vkSceneMeshes.size());
    else
        LUNA_LOG_WARN("VK: CreateIndirectResources FAILED");

    // Phase 18D: build acceleration structures + RT pipeline (requires merged geometry)
    if (_rtSupported) {
        if (BuildAccelerationStructures() && CreateRTPipeline())
            LUNA_LOG_INFO("VK RT: ray tracing initialized (%zu BLAS(es))", _vkBLASes.size());
        else
            LUNA_LOG_WARN("VK RT: initialization failed ??falling back to CSM-only shadows");
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
        LUNA_LOG_INFO("VK: Debug quad GPU-driven ready");

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
    ii.ImageCount      = (uint32_t)_swapchainImages.size();
    ii.CheckVkResultFn = [](VkResult e){ if(e!=VK_SUCCESS) LUNA_LOG_ERROR("ImGui Vk: %d",(int)e); };
    ii.PipelineInfoMain.RenderPass   = _renderPass;
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
    ImGuiIO& io = ImGui::GetIO();
    if (io.BackendRendererUserData != nullptr)
        ImGui_ImplVulkan_Shutdown();
    if (io.BackendPlatformUserData != nullptr)
        ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

void VulkanBackend::Resize(uint32_t w, uint32_t h) {
    if (!w || !h) return;
    _width = w; _height = h;
    vkDeviceWaitIdle(_device->GetDevice());
    RecreateSwapchain();
    ImGui_ImplVulkan_SetMinImageCount(2);
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
    VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool        = _frames[0].cmdPool;
    ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(_device->GetDevice(), &ai, &cmd);
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    return cmd;
}

void VulkanBackend::EndSingleTimeCommands(VkCommandBuffer cmd) {
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &cmd;
    vkQueueSubmit(_device->GetGraphicsQueue(), 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(_device->GetGraphicsQueue());
    vkFreeCommandBuffers(_device->GetDevice(), _frames[0].cmdPool, 1, &cmd);
}

// ===========================================================================
// Depth resources
// ===========================================================================
bool VulkanBackend::CreateDepthResources() {
    CreateImage(_swapchainExtent.width, _swapchainExtent.height,
        VK_FORMAT_D32_SFLOAT, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        _depthImage, _depthMemory);
    _depthView = CreateImageView(_depthImage, VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT);
    return _depthView != VK_NULL_HANDLE;
}

void VulkanBackend::DestroyDepthResources() {
    VkDevice dev = _device->GetDevice();
    if (_depthView)   { vkDestroyImageView(dev,_depthView,nullptr);  _depthView   = VK_NULL_HANDLE; }
    if (_depthImage)  { vkDestroyImage(dev,_depthImage,nullptr);     _depthImage  = VK_NULL_HANDLE; }
    if (_depthMemory) { vkFreeMemory(dev,_depthMemory,nullptr);      _depthMemory = VK_NULL_HANDLE; }
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

// ===========================================================================
// Swapchain
// ===========================================================================
bool VulkanBackend::CreateSwapchain(uint32_t width, uint32_t height) {
    VkPhysicalDevice gpu = _device->GetPhysicalDevice();
    VkDevice         dev = _device->GetDevice();
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, _surface, &caps);

    uint32_t fc = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, _surface, &fc, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(fc);
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, _surface, &fc, fmts.data());
    VkSurfaceFormatKHR chosen = fmts[0];
    for (auto& f : fmts)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            { chosen = f; break; }

    VkExtent2D ext;
    if (caps.currentExtent.width != UINT32_MAX) ext = caps.currentExtent;
    else {
        ext.width  = std::clamp(width,  caps.minImageExtent.width,  caps.maxImageExtent.width);
        ext.height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }

    uint32_t ic = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && ic > caps.maxImageCount) ic = caps.maxImageCount;

    VkSwapchainCreateInfoKHR sci{};
    sci.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sci.surface          = _surface;
    sci.minImageCount    = ic;
    sci.imageFormat      = chosen.format;
    sci.imageColorSpace  = chosen.colorSpace;
    sci.imageExtent      = ext;
    sci.imageArrayLayers = 1;
    sci.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    uint32_t qf[] = { _device->GetGraphicsQueueFamily(), _device->GetPresentQueueFamily() };
    if (qf[0] != qf[1]) {
        sci.imageSharingMode      = VK_SHARING_MODE_CONCURRENT;
        sci.queueFamilyIndexCount = 2;
        sci.pQueueFamilyIndices   = qf;
    } else sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;

    sci.preTransform   = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;

    // VSync: FIFO (vsync on) or MAILBOX (triple-buffer, vsync off) with IMMEDIATE fallback
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;  // always supported
    if (!_vsync)
    {
        uint32_t pmCount = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, _surface, &pmCount, nullptr);
        std::vector<VkPresentModeKHR> pms(pmCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, _surface, &pmCount, pms.data());
        for (auto pm : pms)
            if (pm == VK_PRESENT_MODE_MAILBOX_KHR) { presentMode = pm; break; }
        if (presentMode == VK_PRESENT_MODE_FIFO_KHR)
            for (auto pm : pms)
                if (pm == VK_PRESENT_MODE_IMMEDIATE_KHR) { presentMode = pm; break; }
    }
    sci.presentMode = presentMode;
    sci.clipped     = VK_TRUE;

    if (vkCreateSwapchainKHR(dev, &sci, nullptr, &_swapchain) != VK_SUCCESS)
    { LUNA_LOG_ERROR("Failed swapchain"); return false; }

    _swapchainFormat = chosen.format;
    _swapchainExtent = ext;

    uint32_t n = 0;
    vkGetSwapchainImagesKHR(dev, _swapchain, &n, nullptr);
    _swapchainImages.resize(n);
    vkGetSwapchainImagesKHR(dev, _swapchain, &n, _swapchainImages.data());
    _swapchainImageViews.resize(n);
    _imagesInFlight.resize(n, VK_NULL_HANDLE);  // Per-image fence tracking
    for (uint32_t i = 0; i < n; i++)
        _swapchainImageViews[i] = CreateImageView(_swapchainImages[i], _swapchainFormat, VK_IMAGE_ASPECT_COLOR_BIT);

    LUNA_LOG_INFO("Swapchain: %ux%u, %u images", ext.width, ext.height, n);
    return true;
}

void VulkanBackend::DestroySwapchain() {
    VkDevice dev = _device->GetDevice();
    for (auto fb : _framebuffers) vkDestroyFramebuffer(dev, fb, nullptr);
    _framebuffers.clear();
    for (auto iv : _swapchainImageViews) vkDestroyImageView(dev, iv, nullptr);
    _swapchainImageViews.clear(); _swapchainImages.clear();
    _imagesInFlight.clear();
    if (_swapchain) { vkDestroySwapchainKHR(dev, _swapchain, nullptr); _swapchain = VK_NULL_HANDLE; }
}

void VulkanBackend::RecreateSwapchain() {
    DestroyPPResources();   // Phase 16C: destroy HDR RT first (sets _hdrView=null so CreateFramebuffers skips _hdrFramebuffer)
    DestroyGBufferResources();
    DestroyDepthResources(); DestroySwapchain();
    CreateSwapchain(_width, _height); CreateDepthResources();
    CreateGBufferResources();
    CreateFramebuffers();

    // Recreate size-dependent SSAO resources if SSAO is active
    if (_ssaoPipeline && _ssaoDescPool) {
        VkDevice dev = _device->GetDevice();
        uint32_t halfW = std::max(1u, _swapchainExtent.width  / 2);
        uint32_t halfH = std::max(1u, _swapchainExtent.height / 2);

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
        VkDescriptorImageInfo di { _pointClampSampler, _depthView,    VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo ni { _pointClampSampler, _gbNormalView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
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

    // Phase 16C: recreate size-dependent SSR + HDR RT (PP resources were destroyed at the top)
    if (!CreatePPResources()) { LUNA_LOG_WARN("VK: PP resources recreation failed ??SSR disabled"); }
}

// ===========================================================================
// Render pass (color + depth)
// ===========================================================================
bool VulkanBackend::CreateRenderPass() {
    VkDevice dev = _device->GetDevice();

    // ?�?� Helper: create a single-subpass render pass ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
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

    // Swapchain render pass ??ImGui overlay (LOAD from deferred-lighting result, PRESENT final)
    // initialLayout = PRESENT_SRC_KHR because the deferred lighting pass wrote PRESENT_SRC_KHR
    // before this pass starts (CompositeFrame ends the lighting pass, then opens this one).
    _renderPass = [&]() -> VkRenderPass {
        VkAttachmentDescription att{};
        att.format         = _swapchainFormat;
        att.samples        = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
        att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout  = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        att.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference cr{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription  sp{};
        sp.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sp.colorAttachmentCount = 1;
        sp.pColorAttachments    = &cr;

        VkSubpassDependency dep{};
        dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass    = 0;
        dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo rpi{};
        rpi.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpi.attachmentCount = 1;
        rpi.pAttachments    = &att;
        rpi.subpassCount    = 1;
        rpi.pSubpasses      = &sp;
        rpi.dependencyCount = 1;
        rpi.pDependencies   = &dep;
        VkRenderPass rp = VK_NULL_HANDLE;
        vkCreateRenderPass(dev, &rpi, nullptr, &rp);
        return rp;
    }();
    if (!_renderPass) return false;

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
    _tonemapRenderPass = mkRP(_swapchainFormat, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                              false, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
    if (!_tonemapRenderPass) return false;

    // G-buffer render pass ??3 colour (albedo/normal/metalRough) + depth.
    // Colour final layout: SHADER_READ_ONLY_OPTIMAL (ready for deferred lighting read).
    // Depth  final layout: DEPTH_STENCIL_READ_ONLY_OPTIMAL (sampled in deferred pass).
    _gbRenderPass = [&]() -> VkRenderPass {
        VkAttachmentDescription atts[4]{};

        // att[0]: albedo ??RGBA8_UNORM
        atts[0].format         = VK_FORMAT_R8G8B8A8_UNORM;
        atts[0].samples        = VK_SAMPLE_COUNT_1_BIT;
        atts[0].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        atts[0].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        atts[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[0].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        atts[0].finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        // att[1]: normal ??RGBA16F
        atts[1]        = atts[0];
        atts[1].format = VK_FORMAT_R16G16B16A16_SFLOAT;

        // att[2]: metalRough ??RGBA8_UNORM
        atts[2]        = atts[0];
        atts[2].format = VK_FORMAT_R8G8B8A8_UNORM;

        // att[3]: depth ??D32_SFLOAT
        atts[3].format         = VK_FORMAT_D32_SFLOAT;
        atts[3].samples        = VK_SAMPLE_COUNT_1_BIT;
        atts[3].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        atts[3].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        atts[3].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[3].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[3].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        atts[3].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        VkAttachmentReference cr[3]{
            { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
            { 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
            { 2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
        };
        VkAttachmentReference dr{ 3, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };

        VkSubpassDescription sp{};
        sp.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sp.colorAttachmentCount    = 3;
        sp.pColorAttachments       = cr;
        sp.pDepthStencilAttachment = &dr;

        // No external dependency needed ??frame fence ensures previous frame has completed
        // before recording commands for the next frame.
        VkRenderPassCreateInfo rpi{};
        rpi.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpi.attachmentCount = 4;
        rpi.pAttachments    = atts;
        rpi.subpassCount    = 1;
        rpi.pSubpasses      = &sp;
        rpi.dependencyCount = 0;
        rpi.pDependencies   = nullptr;
        VkRenderPass rp = VK_NULL_HANDLE;
        vkCreateRenderPass(dev, &rpi, nullptr, &rp);
        return rp;
    }();
    if (!_gbRenderPass) return false;

    // G-buffer LOAD render pass ??same structure but LOAD_OP_LOAD for re-open after GPU cull.
    // FlushDraws explicitly transitions images from SHADER_READ_ONLY/DEPTH_STENCIL_READ_ONLY
    // to ATTACHMENT_OPTIMAL before beginning this pass, so initialLayout matches that state.
    // finalLayout transitions back to READ_ONLY for SSAO + deferred lighting reads.
    _gbRenderPassLoad = [&]() -> VkRenderPass {
        VkAttachmentDescription atts[4]{};
        // att[0]: albedo ??RGBA8_UNORM, LOAD
        atts[0].format         = VK_FORMAT_R8G8B8A8_UNORM;
        atts[0].samples        = VK_SAMPLE_COUNT_1_BIT;
        atts[0].loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
        atts[0].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        atts[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[0].initialLayout  = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        atts[0].finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        // att[1]: normal ??RGBA16F
        atts[1]        = atts[0];
        atts[1].format = VK_FORMAT_R16G16B16A16_SFLOAT;
        // att[2]: metalRough ??RGBA8_UNORM
        atts[2]        = atts[0];
        // att[3]: depth ??D32_SFLOAT, LOAD
        atts[3].format         = VK_FORMAT_D32_SFLOAT;
        atts[3].samples        = VK_SAMPLE_COUNT_1_BIT;
        atts[3].loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
        atts[3].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        atts[3].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[3].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[3].initialLayout  = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        atts[3].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        VkAttachmentReference cr[3]{
            { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
            { 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
            { 2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
        };
        VkAttachmentReference dr{ 3, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };

        VkSubpassDescription sp{};
        sp.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sp.colorAttachmentCount    = 3;
        sp.pColorAttachments       = cr;
        sp.pDepthStencilAttachment = &dr;

        // Exit dependency: G-buffer writes ??SSAO/deferred fragment shader reads.
        VkSubpassDependency dep{};
        dep.srcSubpass    = 0;
        dep.dstSubpass    = VK_SUBPASS_EXTERNAL;
        dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                          | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                          | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dep.dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dep.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        VkRenderPassCreateInfo rpi{};
        rpi.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpi.attachmentCount = 4;
        rpi.pAttachments    = atts;
        rpi.subpassCount    = 1;
        rpi.pSubpasses      = &sp;
        rpi.dependencyCount = 1;
        rpi.pDependencies   = &dep;
        VkRenderPass rp = VK_NULL_HANDLE;
        vkCreateRenderPass(dev, &rpi, nullptr, &rp);
        return rp;
    }();
    if (!_gbRenderPassLoad) return false;

    return true;
}

// ===========================================================================
// Framebuffers
// ===========================================================================
bool VulkanBackend::CreateFramebuffers() {
    VkDevice dev = _device->GetDevice();
    UINT W = _swapchainExtent.width, H = _swapchainExtent.height;

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
        _hdrFramebuffer = mkFB(_hdrRenderPass, _hdrView, _depthView, W, H);

    // G-buffer framebuffer ??3 colour targets + depth
    if (_gbRenderPass && _gbAlbedoView && _gbNormalView && _gbMetalRoughView && _depthView) {
        VkImageView gbAtts[4] = { _gbAlbedoView, _gbNormalView, _gbMetalRoughView, _depthView };
        VkFramebufferCreateInfo fi{};
        fi.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fi.renderPass      = _gbRenderPass;
        fi.attachmentCount = 4;
        fi.pAttachments    = gbAtts;
        fi.width = W; fi.height = H; fi.layers = 1;
        vkCreateFramebuffer(dev, &fi, nullptr, &_gbFramebuffer);
        if (_gbRenderPassLoad) {
            fi.renderPass = _gbRenderPassLoad;
            vkCreateFramebuffer(dev, &fi, nullptr, &_gbFramebufferLoad);
        }
    }

    // Tone-map framebuffers ??one per swapchain image (no depth, PRESENT final)
    _tonemapFramebuffers.resize(_swapchainImageViews.size());
    for (size_t i = 0; i < _swapchainImageViews.size(); i++)
        _tonemapFramebuffers[i] = mkFB(_tonemapRenderPass,
                                       _swapchainImageViews[i], VK_NULL_HANDLE, W, H);

    // ImGui/swapchain framebuffers ??used for the ImGui render pass (_renderPass, no depth)
    _framebuffers.resize(_swapchainImageViews.size());
    for (size_t i = 0; i < _swapchainImageViews.size(); i++)
        _framebuffers[i] = mkFB(_renderPass, _swapchainImageViews[i], VK_NULL_HANDLE, W, H);

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
    gpi.renderPass          = _renderPass;
    gpi.subpass             = 0;

    VkResult res = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, nullptr, &_graphicsPipeline);
    vkDestroyShaderModule(dev, vsM, nullptr);
    vkDestroyShaderModule(dev, fsM, nullptr);

    if (res != VK_SUCCESS)
    { LUNA_LOG_ERROR("VK pipeline failed: %d", (int)res); return false; }

    LUNA_LOG_INFO("VK: Forward PBR pipeline created");

    // ?�?� G-buffer fill pipeline ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
    // Compiles gbuffer_vk.frag.hlsl; reuses same vertex shader, same _pipelineLayout.
    // Targets _gbRenderPass which has 3 colour attachments.
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
        gbGpi.renderPass      = _gbRenderPass;

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
// G-buffer resources
// ===========================================================================
bool VulkanBackend::CreateGBufferResources()
{
    VkDevice dev = _device->GetDevice();
    uint32_t W = _swapchainExtent.width, H = _swapchainExtent.height;

    auto mkGB = [&](VkFormat fmt, VkImage& img, VkDeviceMemory& mem, VkImageView& view) -> bool {
        if (!CreateImage(W, H, fmt, VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, img, mem))
            return false;
        view = CreateImageView(img, fmt, VK_IMAGE_ASPECT_COLOR_BIT);
        return view != VK_NULL_HANDLE;
    };

    if (!mkGB(VK_FORMAT_R8G8B8A8_UNORM,       _gbAlbedoImage,     _gbAlbedoMemory,     _gbAlbedoView))     return false;
    if (!mkGB(VK_FORMAT_R16G16B16A16_SFLOAT,  _gbNormalImage,     _gbNormalMemory,     _gbNormalView))     return false;
    if (!mkGB(VK_FORMAT_R8G8B8A8_UNORM,       _gbMetalRoughImage, _gbMetalRoughMemory, _gbMetalRoughView)) return false;

    // Build the G-buffer framebuffers (3 colour + existing depth view).
    // _depthView must already exist (CreateDepthResources called before this).
    if (_gbRenderPass && _depthView) {
        VkImageView atts[4] = { _gbAlbedoView, _gbNormalView, _gbMetalRoughView, _depthView };
        VkFramebufferCreateInfo fi{};
        fi.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fi.renderPass      = _gbRenderPass;
        fi.attachmentCount = 4;
        fi.pAttachments    = atts;
        fi.width = W; fi.height = H; fi.layers = 1;
        if (vkCreateFramebuffer(dev, &fi, nullptr, &_gbFramebuffer) != VK_SUCCESS) return false;
        if (_gbRenderPassLoad) {
            fi.renderPass = _gbRenderPassLoad;
            if (vkCreateFramebuffer(dev, &fi, nullptr, &_gbFramebufferLoad) != VK_SUCCESS) return false;
        }
    }

    LUNA_LOG_INFO("VK: G-buffer resources created (%ux%u)", W, H);
    return true;
}

void VulkanBackend::DestroyGBufferResources()
{
    VkDevice dev = _device->GetDevice();
    if (_gbFramebufferLoad) { vkDestroyFramebuffer(dev, _gbFramebufferLoad, nullptr); _gbFramebufferLoad = VK_NULL_HANDLE; }
    if (_gbFramebuffer)     { vkDestroyFramebuffer(dev, _gbFramebuffer,     nullptr); _gbFramebuffer     = VK_NULL_HANDLE; }
    auto dt = [&](VkImageView& v, VkImage& i, VkDeviceMemory& m) {
        if (v) { vkDestroyImageView(dev, v, nullptr); v = VK_NULL_HANDLE; }
        if (i) { vkDestroyImage(dev, i, nullptr);     i = VK_NULL_HANDLE; }
        if (m) { vkFreeMemory(dev, m, nullptr);       m = VK_NULL_HANDLE; }
    };
    dt(_gbAlbedoView,     _gbAlbedoImage,     _gbAlbedoMemory);
    dt(_gbNormalView,     _gbNormalImage,     _gbNormalMemory);
    dt(_gbMetalRoughView, _gbMetalRoughImage, _gbMetalRoughMemory);
}

void VulkanBackend::UpdateDeferredGbufDescriptors()
{
    if (_deferredGbufDescSet == VK_NULL_HANDLE) return;
    VkDevice dev = _device->GetDevice();

    VkDescriptorImageInfo aII  { _pointClampSampler, _gbAlbedoView,     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo nII  { _pointClampSampler, _gbNormalView,     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo mrII { _pointClampSampler, _gbMetalRoughView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo dII  { _pointClampSampler, _depthView,        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo sII  { _pointClampSampler, VK_NULL_HANDLE,    VK_IMAGE_LAYOUT_UNDEFINED };
    VkDescriptorImageInfo csmII{ VK_NULL_HANDLE,     _csmArrayView ? _csmArrayView : _gbAlbedoView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo csmSII{ _csmSampler ? _csmSampler : _pointClampSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
    VkDescriptorImageInfo ssaoII{ VK_NULL_HANDLE, _ssaoBlurView ? _ssaoBlurView : _gbAlbedoView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo bilinII{ _ssaoBilinearClamp ? _ssaoBilinearClamp : _pointClampSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };

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
    if (_iblReady) {
        envII    = { VK_NULL_HANDLE,  _vkPrefilterCubemapView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        irrII    = { VK_NULL_HANDLE,  _vkIrrCubemapView,       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        lutII    = { VK_NULL_HANDLE,  _vkBrdfLUTView,          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        envSmpII = { _vkIBLSampler,   VK_NULL_HANDLE,          VK_IMAGE_LAYOUT_UNDEFINED };
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

    VkDescriptorSetLayout dLayouts[] = { _deferredSceneLayout, _deferredGbufLayout };
    VkPipelineLayoutCreateInfo pli{};
    pli.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 2;
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
// CSM Resources
// ===========================================================================
bool VulkanBackend::CreateCSMResources()
{
    VkDevice dev = _device->GetDevice();

    // Create 2D array image (D32_SFLOAT, 2048×2048, 4 layers)
    {
        VkImageCreateInfo ii{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ii.imageType   = VK_IMAGE_TYPE_2D;
        ii.format      = VK_FORMAT_D32_SFLOAT;
        ii.extent      = { CSM_SHADOW_SIZE, CSM_SHADOW_SIZE, 1 };
        ii.mipLevels   = 1;
        ii.arrayLayers = CSM_CASCADE_COUNT;
        ii.samples     = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling      = VK_IMAGE_TILING_OPTIMAL;
        ii.usage       = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateImage(dev, &ii, nullptr, &_csmImage) != VK_SUCCESS) return false;

        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(dev, _csmImage, &req);
        VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        ai.allocationSize  = req.size;
        ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(dev, &ai, nullptr, &_csmMemory) != VK_SUCCESS) return false;
        vkBindImageMemory(dev, _csmImage, _csmMemory, 0);
    }

    // Per-layer image views (for framebuffer depth attachment)
    for (uint32_t i = 0; i < CSM_CASCADE_COUNT; i++) {
        VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image    = _csmImage;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format   = VK_FORMAT_D32_SFLOAT;
        vi.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, i, 1 };
        vkCreateImageView(dev, &vi, nullptr, &_csmLayerView[i]);
    }

    // Array image view (for sampling in deferred lighting)
    {
        VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image    = _csmImage;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        vi.format   = VK_FORMAT_D32_SFLOAT;
        vi.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, CSM_CASCADE_COUNT };
        vkCreateImageView(dev, &vi, nullptr, &_csmArrayView);
    }

    // CSM depth-only render pass
    // initialLayout = UNDEFINED: first use or we don't care about previous content (cleared anyway)
    // finalLayout = DEPTH_STENCIL_READ_ONLY_OPTIMAL: ready for sampling in deferred lighting
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
        vkCreateRenderPass(dev, &rpi, nullptr, &_csmRenderPass);
    }

    // Per-layer framebuffers
    for (uint32_t i = 0; i < CSM_CASCADE_COUNT; i++) {
        VkFramebufferCreateInfo fi{};
        fi.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fi.renderPass      = _csmRenderPass;
        fi.attachmentCount = 1;
        fi.pAttachments    = &_csmLayerView[i];
        fi.width  = CSM_SHADOW_SIZE;
        fi.height = CSM_SHADOW_SIZE;
        fi.layers = 1;
        vkCreateFramebuffer(dev, &fi, nullptr, &_csmFramebuffers[i]);
    }

    // Point-clamp sampler for shadow reads
    {
        VkSamplerCreateInfo si{};
        si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter    = VK_FILTER_NEAREST;
        si.minFilter    = VK_FILTER_NEAREST;
        si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxAnisotropy = 1.0f;
        vkCreateSampler(dev, &si, nullptr, &_csmSampler);
    }

    // CSM pipeline layout: push constant (64B lightMVP, vertex stage)
    {
        VkPushConstantRange pcr{};
        pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pcr.offset     = 0;
        pcr.size       = 64;  // float4x4
        VkPipelineLayoutCreateInfo pli{};
        pli.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges    = &pcr;
        vkCreatePipelineLayout(dev, &pli, nullptr, &_csmPipelineLayout);
    }

    // CSM pipeline: depth-only, no FS, front-face culling, depth bias
    {
        std::vector<uint32_t> vsS;
        if (!CompileGLSLtoSPIRV(GetShaderFullPath(L"csm_depth_vk.vert.glsl").wstring(), vsS))
        { LUNA_LOG_ERROR("VK: csm_depth_vk shader compile failed"); return false; }

        VkShaderModuleCreateInfo smi{};
        smi.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smi.codeSize = vsS.size() * 4;
        smi.pCode    = vsS.data();
        VkShaderModule vsM = VK_NULL_HANDLE;
        vkCreateShaderModule(dev, &smi, nullptr, &vsM);

        VkPipelineShaderStageCreateInfo stage{};
        stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage  = VK_SHADER_STAGE_VERTEX_BIT;
        stage.module = vsM;
        stage.pName  = "main";

        // Same vertex layout as PBR: stride 48, but only position is consumed
        // Declare all 4 attrs to match the buffer stride; unused ones are silently ignored
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

        VkViewport vp{ 0, 0, (float)CSM_SHADOW_SIZE, (float)CSM_SHADOW_SIZE, 0, 1 };
        VkRect2D   sc{ {0,0}, {CSM_SHADOW_SIZE, CSM_SHADOW_SIZE} };
        VkPipelineViewportStateCreateInfo vps{};
        vps.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vps.viewportCount = 1;
        vps.pViewports    = &vp;
        vps.scissorCount  = 1;
        vps.pScissors     = &sc;

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
        gpi.layout              = _csmPipelineLayout;
        gpi.renderPass          = _csmRenderPass;
        gpi.subpass             = 0;

        VkResult r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, nullptr, &_csmPipeline);
        vkDestroyShaderModule(dev, vsM, nullptr);
        if (r != VK_SUCCESS) { LUNA_LOG_ERROR("VK: CSM pipeline failed: %d", (int)r); return false; }
    }

    // Transition CSM image to DEPTH_STENCIL_READ_ONLY initially (so first frame deferred read is valid)
    {
        VkCommandBuffer cmd = BeginSingleTimeCommands();
        VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout           = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image               = _csmImage;
        b.subresourceRange    = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, CSM_CASCADE_COUNT };
        b.srcAccessMask       = 0;
        b.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);
        EndSingleTimeCommands(cmd);
    }

    LUNA_LOG_INFO("VK: CSM resources created (%u×%u × %u cascades)", CSM_SHADOW_SIZE, CSM_SHADOW_SIZE, CSM_CASCADE_COUNT);
    return true;
}

void VulkanBackend::DestroyCSMResources()
{
    VkDevice dev = _device->GetDevice();
    if (_csmPipeline)       { vkDestroyPipeline(dev, _csmPipeline, nullptr);       _csmPipeline       = VK_NULL_HANDLE; }
    if (_csmPipelineLayout) { vkDestroyPipelineLayout(dev, _csmPipelineLayout, nullptr); _csmPipelineLayout = VK_NULL_HANDLE; }
    for (uint32_t i = 0; i < CSM_CASCADE_COUNT; i++) {
        if (_csmFramebuffers[i]) { vkDestroyFramebuffer(dev, _csmFramebuffers[i], nullptr); _csmFramebuffers[i] = VK_NULL_HANDLE; }
        if (_csmLayerView[i])    { vkDestroyImageView(dev, _csmLayerView[i], nullptr);     _csmLayerView[i]    = VK_NULL_HANDLE; }
    }
    if (_csmArrayView)  { vkDestroyImageView(dev, _csmArrayView, nullptr);  _csmArrayView  = VK_NULL_HANDLE; }
    if (_csmRenderPass) { vkDestroyRenderPass(dev, _csmRenderPass, nullptr); _csmRenderPass = VK_NULL_HANDLE; }
    if (_csmImage)      { vkDestroyImage(dev, _csmImage, nullptr);          _csmImage      = VK_NULL_HANDLE; }
    if (_csmMemory)     { vkFreeMemory(dev, _csmMemory, nullptr);           _csmMemory     = VK_NULL_HANDLE; }
    if (_csmSampler)    { vkDestroySampler(dev, _csmSampler, nullptr);      _csmSampler    = VK_NULL_HANDLE; }
}

// ---------------------------------------------------------------------------
// UpdateCSMMatrices ??practical split scheme + orthographic light VP
// ---------------------------------------------------------------------------
void VulkanBackend::UpdateCSMMatrices(const XMFLOAT4X4& view, const XMFLOAT4X4& proj)
{
    const float nearZ  = 0.1f;
    const float farZ   = 100.0f;
    const float lambda = 0.5f;
    const float ratio  = farZ / nearZ;

    float splits[CSM_CASCADE_COUNT];
    for (uint32_t i = 0; i < CSM_CASCADE_COUNT; ++i)
    {
        float p       = (i + 1) / (float)CSM_CASCADE_COUNT;
        float logSpl  = nearZ * std::pow(ratio, p);
        float unifSpl = nearZ + (farZ - nearZ) * p;
        splits[i]     = lambda * (logSpl - unifSpl) + unifSpl;
    }
    for (uint32_t i = 0; i < CSM_CASCADE_COUNT; i++) _csmSplits[i] = splits[i];

    XMVECTOR lightDirV = XMVector3Normalize(XMVectorSet(1.0f, 2.0f, 1.0f, 0.0f));
    XMMATRIX viewMat = XMLoadFloat4x4(&view);
    XMMATRIX projMat = XMLoadFloat4x4(&proj);
    XMMATRIX invView = XMMatrixInverse(nullptr, viewMat);

    float tanHalfFovX = 1.0f / XMVectorGetX(projMat.r[0]);
    float tanHalfFovY = 1.0f / XMVectorGetY(projMat.r[1]);

    float lastSplit = nearZ;
    for (uint32_t cascade = 0; cascade < CSM_CASCADE_COUNT; ++cascade)
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
        XMStoreFloat4x4(&_csmLightVP[cascade], lightVP);

        lastSplit = zF;
    }
}

// ---------------------------------------------------------------------------
// DrawCSMPass ??render all meshes into the 4-cascade shadow map
// ---------------------------------------------------------------------------
void VulkanBackend::DrawCSMPass(VkCommandBuffer cmd)
{
    if (!_csmPipeline || _lastMeshModels.empty()) return;

    for (uint32_t cascade = 0; cascade < CSM_CASCADE_COUNT; ++cascade)
    {
        VkClearValue clear{};
        clear.depthStencil = { 1.0f, 0 };

        VkRenderPassBeginInfo rpi{};
        rpi.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpi.renderPass        = _csmRenderPass;
        rpi.framebuffer       = _csmFramebuffers[cascade];
        rpi.renderArea.extent = { CSM_SHADOW_SIZE, CSM_SHADOW_SIZE };
        rpi.clearValueCount   = 1;
        rpi.pClearValues      = &clear;
        vkCmdBeginRenderPass(cmd, &rpi, VK_SUBPASS_CONTENTS_INLINE);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _csmPipeline);

        XMMATRIX lvp = XMLoadFloat4x4(&_csmLightVP[cascade]);

        for (size_t mi = 0; mi < _vkSceneMeshes.size() && mi < _lastMeshModels.size(); ++mi)
        {
            auto& m = _vkSceneMeshes[mi];
            if (!m) continue;

            XMMATRIX model = XMLoadFloat4x4(&_lastMeshModels[mi]);
            XMMATRIX lmvp  = XMMatrixMultiply(model, lvp);
            XMFLOAT4X4 lmvpf;
            XMStoreFloat4x4(&lmvpf, lmvp);

            vkCmdPushConstants(cmd, _csmPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, &lmvpf);

            VkDeviceSize off = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &m->vertexBuffer, &off);
            vkCmdBindIndexBuffer(cmd, m->indexBuffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, m->indexCount, 1, 0, 0, 0);
        }

        vkCmdEndRenderPass(cmd);
    }
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
    uint32_t halfW = std::max(1u, _swapchainExtent.width  / 2);
    uint32_t halfH = std::max(1u, _swapchainExtent.height / 2);

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

        VkDescriptorImageInfo di { _pointClampSampler, _depthView,      VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo ni { _pointClampSampler, _gbNormalView,   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
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
    uint32_t W = _swapchainExtent.width, H = _swapchainExtent.height;

    // ?�?� 1. HDR image (COLOR_ATTACHMENT | SAMPLED, R16G16B16A16_SFLOAT) ?�?�
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

    // ?�?� 2. SSR image (STORAGE | SAMPLED, R16G16B16A16_SFLOAT, kept GENERAL) ?�?�
    if (!CreateImage(W, H, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, _ssrImage, _ssrMemory))
        return false;
    _ssrView = CreateImageView(_ssrImage, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!_ssrView) return false;
    TransitionImageLayout(_ssrImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

    // ?�?� 3. Tonemap DSL: binding0=SAMPLED_IMAGE(hdr), binding1=SAMPLED_IMAGE(ssr), binding2=SAMPLER ?�?�
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

    // ?�?� 4. SSR compute pipeline ?�?�
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
        VkDescriptorImageInfo  di { VK_NULL_HANDLE, _depthView,          VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo  ni { VK_NULL_HANDLE, _gbNormalView,       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo  mri{ VK_NULL_HANDLE, _gbMetalRoughView,   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
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

    // ?�?� Phase 18B: Motion Blur target (full-res RGBA16F, COLOR_ATTACHMENT | SAMPLED) ?�?�
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
            VkDescriptorImageInfo depI { VK_NULL_HANDLE, _depthView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
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

    // ?�?� Phase 17: TAA + Bloom + Full Tonemap ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
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
        VkDescriptorImageInfo depthImg{ VK_NULL_HANDLE, _depthView,        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
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
                    _vkMBPipeline = mkGfxPipeline(vsM, mkMod(mbFS), _vkMBPipeLayout, _ppRenderPass);
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
    uint32_t W = _swapchainExtent.width, H = _swapchainExtent.height;

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
    rpi.renderArea.extent = _swapchainExtent;
    rpi.clearValueCount   = 1;
    rpi.pClearValues      = &clear;
    vkCmdBeginRenderPass(cmd, &rpi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{ 0, 0, (float)W, (float)H, 0, 1 };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc{ {0,0}, _swapchainExtent };
    vkCmdSetScissor(cmd, 0, 1, &sc);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _vkTAAPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                             _vkTAAPipelineLayout, 0, 1, &_vkTAADescSet[_frameIndex], 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);

    // Barrier: TAA write complete ??bloom bright read
    VkImageMemoryBarrier b{};
    b.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask    = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask    = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.newLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.image            = _taaHistoryImage[_vkTaaHistoryIndex];
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &b);
}

void VulkanBackend::DrawVKBloomBrightPass()
{
    if (!_vkBloomBrightPipeline) return;
    auto& frame = _frames[_frameIndex];
    VkCommandBuffer cmd = frame.cmdBuffer;
    uint32_t halfW = std::max(1u, _swapchainExtent.width  / 2);
    uint32_t halfH = std::max(1u, _swapchainExtent.height / 2);

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

    VkImageMemoryBarrier b{};
    b.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask    = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask    = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.newLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.image            = _bloomBrightImage;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &b);
}

void VulkanBackend::DrawVKBloomBlurPass(bool horizontal)
{
    if (!_vkBloomBlurPipeline) return;
    auto& frame = _frames[_frameIndex];
    VkCommandBuffer cmd = frame.cmdBuffer;
    uint32_t halfW = std::max(1u, _swapchainExtent.width  / 2);
    uint32_t halfH = std::max(1u, _swapchainExtent.height / 2);

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

    VkImageMemoryBarrier b{};
    b.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask    = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask    = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.newLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.image            = dstImg;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &b);
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
    rpi.renderArea.extent = _swapchainExtent;
    rpi.clearValueCount   = 1;
    rpi.pClearValues      = &clear;
    vkCmdBeginRenderPass(cmd, &rpi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{ 0, 0, (float)_swapchainExtent.width, (float)_swapchainExtent.height, 0, 1 };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc{ {0,0}, _swapchainExtent };
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
    uint32_t halfW = std::max(1u, _swapchainExtent.width  / 2);
    uint32_t halfH = std::max(1u, _swapchainExtent.height / 2);

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
    uint32_t halfW = std::max(1u, _swapchainExtent.width  / 2);
    uint32_t halfH = std::max(1u, _swapchainExtent.height / 2);

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
    DestroyIBLResources();        // Phase 15C: destroy IBL textures + compute pipelines
    DestroyIndirectResources();   // Phase 15B: destroy GPU-driven buffers + pipelines
    DestroyDeferredPipeline();
    DestroySSAOResources();
    DestroyCSMResources();
    DestroyGBufferResources();
    if (_gbPipeline)        { vkDestroyPipeline(dev, _gbPipeline, nullptr);                   _gbPipeline         = VK_NULL_HANDLE; }
    if (_gbRenderPassLoad)  { vkDestroyRenderPass(dev, _gbRenderPassLoad, nullptr);           _gbRenderPassLoad   = VK_NULL_HANDLE; }
    if (_gbRenderPass)      { vkDestroyRenderPass(dev, _gbRenderPass, nullptr);               _gbRenderPass       = VK_NULL_HANDLE; }
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

        VkBufferUsageFlags vbUsage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
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

        VkBufferUsageFlags ibUsage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
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
}

bool VulkanBackend::CreateIndirectResources()
{
    if (!_mergedVB || !_mergedIB || !_meshInfoBuf) return false;
    if (_vkSceneMeshes.empty()) return false;

    VkDevice dev = _device->GetDevice();
    uint32_t meshCount = (uint32_t)_vkSceneMeshes.size();

    // ?�?� objectData SSBO (HOST_VISIBLE|COHERENT, persistently mapped) ?�?�?�?�?�?�?�?�?�
    {
        VkDeviceSize sz = MAX_GPU_OBJECTS * sizeof(GPUObjectDataVK);
        if (!CreateBuffer(sz,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                _objectDataBuffer, _objectDataMem))
            return false;
        vkMapMemory(dev, _objectDataMem, 0, sz, 0, &_objectDataMapped);
    }

    // ?�?� Per-frame indirect arg + draw count buffers (DEVICE_LOCAL) ?�?�?�?�?�?�?�?�?�?�
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

    // ?�?� Collect unique materials for factor SSBO and bindless textures ?�?�?�?�?�?�?�?�
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

    // ?�?� Material factor SSBO ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
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

    // ?�?� Bindless material descriptor set layout (set=1) ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
    // binding 0: material factor SSBO
    // binding 1: albedo   texture array (runtime, PARTIALLY_BOUND)
    // binding 2: normal   texture array (runtime, PARTIALLY_BOUND)
    // binding 3: metalRough texture array (runtime, PARTIALLY_BOUND)
    // binding 4: sampler
    {
        VkDescriptorSetLayoutBinding bs[6]{};
        bs[0] = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         1,         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        bs[1] = { 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,          matCount,  VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        bs[2] = { 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,          matCount,  VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        bs[3] = { 3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,          matCount,  VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        bs[4] = { 4, VK_DESCRIPTOR_TYPE_SAMPLER,                1,         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        bs[5] = { 5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,          matCount,  VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };

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

    // ?�?� VS descriptor set layout (set=0): ViewProj UBO + ObjectData SSBO ?�?�?�?�
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

    // ?�?� Cull descriptor set layout (set=0): CullConstants (push) + 4 SSBOs ?�?�
    {
        VkDescriptorSetLayoutBinding bs[4]{};
        bs[0] = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }; // gObjects
        bs[1] = { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }; // gMeshInfo
        bs[2] = { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }; // gDrawArgs
        bs[3] = { 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }; // gDrawCount

        VkDescriptorSetLayoutCreateInfo li{};
        li.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 4;
        li.pBindings    = bs;
        vkCreateDescriptorSetLayout(dev, &li, nullptr, &_vkCullDescLayout);
    }

    // ?�?� Descriptor pools ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
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

    // Cull pool: FRAMES_IN_FLIGHT sets × 4 SSBOs
    {
        VkDescriptorPoolSize psz[] = {
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 * FRAMES_IN_FLIGHT },
        };
        VkDescriptorPoolCreateInfo pi{};
        pi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pi.maxSets       = FRAMES_IN_FLIGHT;
        pi.poolSizeCount = 1;
        pi.pPoolSizes    = psz;
        vkCreateDescriptorPool(dev, &pi, nullptr, &_vkCullDescPool);
    }

    // ?�?� Allocate descriptor sets ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
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

    // ?�?� ViewProj UBO for VS set ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
    {
        if (!CreateBuffer(128,
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                _indirectViewProjBuf, _indirectViewProjMem))
            return false;
        vkMapMemory(dev, _indirectViewProjMem, 0, 128, 0, &_indirectViewProjMapped);
    }

    // ?�?� Write VS descriptor set ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
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

    // ?�?� Write bindless material descriptor set ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
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

    // ?�?� Write cull descriptor sets (per-frame, bind indirectArg + drawCount per frame) ?�
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

    // ?�?� Pipeline layouts ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
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
        pcr.size       = 112;  // 6×float4 frustumPlanes + uint objectCount + 3×uint pad
        VkPipelineLayoutCreateInfo pli{};
        pli.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount         = 1;
        pli.pSetLayouts            = &_vkCullDescLayout;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges    = &pcr;
        vkCreatePipelineLayout(dev, &pli, nullptr, &_vkCullPipeLayout);
    }

    // ?�?� GPU cull compute pipeline ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
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

    // ?�?� Indirect G-buffer graphics pipeline ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
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
        rs.cullMode    = VK_CULL_MODE_BACK_BIT;
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
        gpi.renderPass          = _gbRenderPassLoad;   // re-opened pass (LOAD_OP_LOAD)
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
}

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

    // 1b. Transition G-buffer images: SHADER_READ_ONLY ??COLOR_ATTACHMENT (color),
    //     DEPTH_STENCIL_READ_ONLY ??DEPTH_STENCIL_ATTACHMENT (depth).
    // Required because _gbRenderPass finalLayout leaves them in READ_ONLY state,
    // but _gbRenderPassLoad initialLayout expects ATTACHMENT_OPTIMAL.
    {
        VkImageMemoryBarrier bars[4]{};
        // Color barriers (albedo, normal, metalRough)
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
        bars[0].image = _gbAlbedoImage;
        bars[1].image = _gbNormalImage;
        bars[2].image = _gbMetalRoughImage;
        // Depth barrier
        bars[3].sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        bars[3].srcAccessMask    = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        bars[3].dstAccessMask    = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                                 | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        bars[3].oldLayout        = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        bars[3].newLayout        = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        bars[3].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bars[3].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bars[3].subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
        bars[3].image            = _depthImage;

        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
            0, 0, nullptr, 0, nullptr, 4, bars);
    }

    // 2. Upload objectData (HOST_COHERENT ??no barrier needed)
    memcpy(_objectDataMapped, _cpuInstances.data(), count * sizeof(GPUObjectDataVK));

    // 3. Clear draw count (must be outside render pass)
    vkCmdFillBuffer(cmd, _drawCountBuffer[_frameIndex], 0, sizeof(uint32_t), 0);

    // 4. Barrier: TRANSFER_WRITE ??COMPUTE_SHADER_READ (drawCount)
    //             host-written objectData ??COMPUTE_SHADER_READ (no explicit barrier; HOST_COHERENT)
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

    // 5. GPU frustum cull dispatch
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _vkCullPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _vkCullPipeLayout,
            0, 1, &_vkCullDescSet[_frameIndex], 0, nullptr);

        // Build frustum planes from ViewProj (push constants: 6 planes + objectCount)
        XMMATRIX V  = XMLoadFloat4x4(&_deferredView);
        XMMATRIX P  = XMLoadFloat4x4(&_deferredProj);
        XMMATRIX VP = XMMatrixMultiply(V, P);
        // Column-extract for row-major VP (stored as VP = V*P in row-major)
        // Frustum plane extraction (Gribb-Hartmann)
        struct CullConstants
        {
            float frustumPlanes[6][4];  // 6×float4 = 96 B
            uint32_t objectCount;
            uint32_t pad[3];            // total 112 B
        } cc{};

        // Transpose so we can use column ops on the row-major matrix
        XMMATRIX T = XMMatrixTranspose(VP);
        XMFLOAT4X4 tm; XMStoreFloat4x4(&tm, T);

        // left:   row3 + row0
        cc.frustumPlanes[0][0] = tm.m[3][0] + tm.m[0][0];
        cc.frustumPlanes[0][1] = tm.m[3][1] + tm.m[0][1];
        cc.frustumPlanes[0][2] = tm.m[3][2] + tm.m[0][2];
        cc.frustumPlanes[0][3] = tm.m[3][3] + tm.m[0][3];
        // right:  row3 - row0
        cc.frustumPlanes[1][0] = tm.m[3][0] - tm.m[0][0];
        cc.frustumPlanes[1][1] = tm.m[3][1] - tm.m[0][1];
        cc.frustumPlanes[1][2] = tm.m[3][2] - tm.m[0][2];
        cc.frustumPlanes[1][3] = tm.m[3][3] - tm.m[0][3];
        // bottom: row3 + row1
        cc.frustumPlanes[2][0] = tm.m[3][0] + tm.m[1][0];
        cc.frustumPlanes[2][1] = tm.m[3][1] + tm.m[1][1];
        cc.frustumPlanes[2][2] = tm.m[3][2] + tm.m[1][2];
        cc.frustumPlanes[2][3] = tm.m[3][3] + tm.m[1][3];
        // top:    row3 - row1
        cc.frustumPlanes[3][0] = tm.m[3][0] - tm.m[1][0];
        cc.frustumPlanes[3][1] = tm.m[3][1] - tm.m[1][1];
        cc.frustumPlanes[3][2] = tm.m[3][2] - tm.m[1][2];
        cc.frustumPlanes[3][3] = tm.m[3][3] - tm.m[1][3];
        // near:   row2
        cc.frustumPlanes[4][0] = tm.m[2][0];
        cc.frustumPlanes[4][1] = tm.m[2][1];
        cc.frustumPlanes[4][2] = tm.m[2][2];
        cc.frustumPlanes[4][3] = tm.m[2][3];
        // far:    row3 - row2
        cc.frustumPlanes[5][0] = tm.m[3][0] - tm.m[2][0];
        cc.frustumPlanes[5][1] = tm.m[3][1] - tm.m[2][1];
        cc.frustumPlanes[5][2] = tm.m[3][2] - tm.m[2][2];
        cc.frustumPlanes[5][3] = tm.m[3][3] - tm.m[2][3];
        cc.objectCount = count;

        vkCmdPushConstants(cmd, _vkCullPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 112, &cc);
        vkCmdDispatch(cmd, (count + 63) / 64, 1, 1);
    }

    // 6. Barrier: COMPUTE SHADER_WRITE ??DRAW_INDIRECT + VERTEX_INPUT read
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

    // 7. Re-open G-buffer render pass with LOAD_OP_LOAD
    {
        VkRenderPassBeginInfo rpi{};
        rpi.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpi.renderPass        = _gbRenderPassLoad;
        rpi.framebuffer       = _gbFramebufferLoad;
        rpi.renderArea.extent = _swapchainExtent;
        rpi.clearValueCount   = 0;  // LOAD_OP_LOAD ??no clears needed
        vkCmdBeginRenderPass(cmd, &rpi, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport vp{ 0, 0,
            (float)_swapchainExtent.width, (float)_swapchainExtent.height, 0, 1 };
        vkCmdSetViewport(cmd, 0, 1, &vp);
        VkRect2D sc{ {0,0}, _swapchainExtent };
        vkCmdSetScissor(cmd, 0, 1, &sc);
    }

    // 8. Indirect G-buffer draw
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
    _cpuInstances.clear();
}

// ===========================================================================
// Phase 15C: IBL environment lighting
// ===========================================================================

bool VulkanBackend::LoadHDREnvironment(const std::string& hdrPath)
{
    VkDevice dev = _device->GetDevice();

    // Load equirectangular HDR image via stb_image
    int w, h, c;
    float* pixels = stbi_loadf(hdrPath.c_str(), &w, &h, &c, 4);
    if (!pixels) {
        LUNA_LOG_ERROR("VK IBL: failed to load HDR '%s': %s", hdrPath.c_str(), stbi_failure_reason());
        return false;
    }
    VkDeviceSize imgSz = (VkDeviceSize)w * h * 4 * sizeof(float);

    // Upload to device-local 2D image (R32G32B32A32_SFLOAT)
    VkImage        equirectImg = VK_NULL_HANDLE;
    VkDeviceMemory equirectMem = VK_NULL_HANDLE;
    VkImageView    equirectView= VK_NULL_HANDLE;
    {
        VkBuffer stg; VkDeviceMemory stgMem;
        CreateBuffer(imgSz, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            stg, stgMem);
        void* p; vkMapMemory(dev, stgMem, 0, imgSz, 0, &p);
        memcpy(p, pixels, (size_t)imgSz);
        vkUnmapMemory(dev, stgMem);
        stbi_image_free(pixels);

        VkImageCreateInfo ii{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ii.imageType   = VK_IMAGE_TYPE_2D;
        ii.format      = VK_FORMAT_R32G32B32A32_SFLOAT;
        ii.extent      = { (uint32_t)w, (uint32_t)h, 1 };
        ii.mipLevels   = 1; ii.arrayLayers = 1;
        ii.samples     = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling      = VK_IMAGE_TILING_OPTIMAL;
        ii.usage       = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        vkCreateImage(dev, &ii, nullptr, &equirectImg);
        VkMemoryRequirements req; vkGetImageMemoryRequirements(dev, equirectImg, &req);
        VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        ai.allocationSize  = req.size;
        ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkAllocateMemory(dev, &ai, nullptr, &equirectMem);
        vkBindImageMemory(dev, equirectImg, equirectMem, 0);

        TransitionImageLayout(equirectImg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        CopyBufferToImage(stg, equirectImg, (uint32_t)w, (uint32_t)h);
        TransitionImageLayout(equirectImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        vkDestroyBuffer(dev, stg, nullptr); vkFreeMemory(dev, stgMem, nullptr);

        VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image    = equirectImg;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format   = VK_FORMAT_R32G32B32A32_SFLOAT;
        vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCreateImageView(dev, &vi, nullptr, &equirectView);
    }

    // Create IBL resources and dispatch precompute
    if (!CreateIBLResources() || !DispatchIBLPrecompute(equirectImg, equirectView)) {
        vkDestroyImageView(dev, equirectView, nullptr);
        vkDestroyImage(dev, equirectImg, nullptr);
        vkFreeMemory(dev, equirectMem, nullptr);
        return false;
    }

    vkDestroyImageView(dev, equirectView, nullptr);
    vkDestroyImage(dev, equirectImg, nullptr);
    vkFreeMemory(dev, equirectMem, nullptr);

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
    _iblReady = true;
    UpdateDeferredGbufDescriptors();
    LUNA_LOG_INFO("VK IBL: environment loaded ??IBL active");
    return true;
}

bool VulkanBackend::CreateIBLResources()
{
    VkDevice dev = _device->GetDevice();

    auto createCubemap = [&](uint32_t size, uint32_t mips, VkFormat fmt,
                              VkImage& img, VkDeviceMemory& mem) -> bool {
        VkImageCreateInfo ii{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ii.flags       = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        ii.imageType   = VK_IMAGE_TYPE_2D;
        ii.format      = fmt;
        ii.extent      = { size, size, 1 };
        ii.mipLevels   = mips;
        ii.arrayLayers = 6;
        ii.samples     = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling      = VK_IMAGE_TILING_OPTIMAL;
        ii.usage       = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateImage(dev, &ii, nullptr, &img) != VK_SUCCESS) return false;
        VkMemoryRequirements req; vkGetImageMemoryRequirements(dev, img, &req);
        VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        ai.allocationSize  = req.size;
        ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        return vkAllocateMemory(dev, &ai, nullptr, &mem) == VK_SUCCESS
            && vkBindImageMemory(dev, img, mem, 0) == VK_SUCCESS;
    };

    auto makeCubeView = [&](VkImage img, VkFormat fmt, uint32_t mips) -> VkImageView {
        VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image    = img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
        vi.format   = fmt;
        vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 6 };
        VkImageView v = VK_NULL_HANDLE;
        vkCreateImageView(dev, &vi, nullptr, &v);
        return v;
    };

    auto makeArrayView = [&](VkImage img, VkFormat fmt, uint32_t baseMip, uint32_t mipCount) -> VkImageView {
        VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image    = img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        vi.format   = fmt;
        vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, baseMip, mipCount, 0, 6 };
        VkImageView v = VK_NULL_HANDLE;
        vkCreateImageView(dev, &vi, nullptr, &v);
        return v;
    };

    // Env cubemap (512², 1 mip, R16G16B16A16_SFLOAT)
    if (!createCubemap(VK_ENV_CUBE_SIZE, 1, VK_FORMAT_R16G16B16A16_SFLOAT, _vkEnvCubemap, _vkEnvCubemapMem)) return false;
    _vkEnvCubemapView  = makeCubeView (_vkEnvCubemap, VK_FORMAT_R16G16B16A16_SFLOAT, 1);
    _vkEnvCubemapArray = makeArrayView(_vkEnvCubemap, VK_FORMAT_R16G16B16A16_SFLOAT, 0, 1);

    // Irradiance cubemap (32², 1 mip, R16G16B16A16_SFLOAT)
    if (!createCubemap(VK_IRR_CUBE_SIZE, 1, VK_FORMAT_R16G16B16A16_SFLOAT, _vkIrrCubemap, _vkIrrCubemapMem)) return false;
    _vkIrrCubemapView  = makeCubeView (_vkIrrCubemap, VK_FORMAT_R16G16B16A16_SFLOAT, 1);
    _vkIrrCubemapArray = makeArrayView(_vkIrrCubemap, VK_FORMAT_R16G16B16A16_SFLOAT, 0, 1);

    // Prefilter cubemap (128², 5 mips, R16G16B16A16_SFLOAT)
    if (!createCubemap(VK_PREFILTER_CUBE_SIZE, VK_PREFILTER_MIP_COUNT, VK_FORMAT_R16G16B16A16_SFLOAT,
                        _vkPrefilterCubemap, _vkPrefilterCubemapMem)) return false;
    _vkPrefilterCubemapView = makeCubeView(_vkPrefilterCubemap, VK_FORMAT_R16G16B16A16_SFLOAT, VK_PREFILTER_MIP_COUNT);
    for (uint32_t m = 0; m < VK_PREFILTER_MIP_COUNT; m++)
        _vkPrefilterMipView[m] = makeArrayView(_vkPrefilterCubemap, VK_FORMAT_R16G16B16A16_SFLOAT, m, 1);

    // BRDF LUT (512×512, R16G16_SFLOAT, 2D)
    {
        VkImageCreateInfo ii{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ii.imageType   = VK_IMAGE_TYPE_2D;
        ii.format      = VK_FORMAT_R16G16_SFLOAT;
        ii.extent      = { VK_BRDF_LUT_SIZE, VK_BRDF_LUT_SIZE, 1 };
        ii.mipLevels   = 1; ii.arrayLayers = 1;
        ii.samples     = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling      = VK_IMAGE_TILING_OPTIMAL;
        ii.usage       = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        if (vkCreateImage(dev, &ii, nullptr, &_vkBrdfLUT) != VK_SUCCESS) return false;
        VkMemoryRequirements req; vkGetImageMemoryRequirements(dev, _vkBrdfLUT, &req);
        VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        ai.allocationSize  = req.size;
        ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkAllocateMemory(dev, &ai, nullptr, &_vkBrdfLUTMem);
        vkBindImageMemory(dev, _vkBrdfLUT, _vkBrdfLUTMem, 0);
        _vkBrdfLUTView = CreateImageView(_vkBrdfLUT, VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT);
    }

    // IBL sampler (trilinear, clamp-to-edge, maxLOD = prefilter mip count)
    {
        VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        si.magFilter    = VK_FILTER_LINEAR;
        si.minFilter    = VK_FILTER_LINEAR;
        si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxAnisotropy = 1.0f;
        si.minLod       = 0.0f;
        si.maxLod       = (float)VK_PREFILTER_MIP_COUNT;
        vkCreateSampler(dev, &si, nullptr, &_vkIBLSampler);
    }

    // Compute pipelines: equirect, irradiance, prefilter, brdfLut
    // All use the same descriptor layout pattern:
    //   binding 0: optional CB or nothing; we store CB as push constants
    // Actually each shader has its own layout. Create all 4 here.

    auto makeComputePipeline = [&](const wchar_t* shaderFile,
                                    VkDescriptorSetLayout dsl,
                                    uint32_t pushSize,
                                    VkPipelineLayout& outLayout,
                                    VkPipeline& outPipeline) -> bool {
        VkPushConstantRange pcr{};
        pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pcr.size       = pushSize;
        VkPipelineLayoutCreateInfo pli{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pli.setLayoutCount         = 1;
        pli.pSetLayouts            = &dsl;
        pli.pushConstantRangeCount = (pushSize > 0) ? 1 : 0;
        pli.pPushConstantRanges    = (pushSize > 0) ? &pcr : nullptr;
        vkCreatePipelineLayout(dev, &pli, nullptr, &outLayout);

        std::vector<uint32_t> csS;
        if (!CompileGLSLtoSPIRV(GetShaderFullPath(shaderFile).wstring(), csS)) return false;
        VkShaderModuleCreateInfo smi{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        smi.codeSize = csS.size() * 4; smi.pCode = csS.data();
        VkShaderModule csM = VK_NULL_HANDLE;
        vkCreateShaderModule(dev, &smi, nullptr, &csM);
        VkComputePipelineCreateInfo cpi{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        cpi.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpi.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        cpi.stage.module = csM;
        cpi.stage.pName  = "main";
        cpi.layout       = outLayout;
        VkResult r = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &outPipeline);
        vkDestroyShaderModule(dev, csM, nullptr);
        return r == VK_SUCCESS;
    };

    // Each IBL shader uses bindings: 0=CB(as UBO in set), 1=input texture, 2=RW output, 3=sampler
    // We create one shared descriptor set layout for equirect/irradiance/prefilter stages:
    //   binding 0: UNIFORM_BUFFER  (CB)
    //   binding 1: SAMPLED_IMAGE   (input: equirect or env cube)
    //   binding 2: STORAGE_IMAGE   (output: RW cube layer)
    //   binding 3: SAMPLER
    // For brdfLut: binding 0: STORAGE_IMAGE (gLutOut only ??no input texture)

    // Layout with UBO+SampledImage+StorageImage+Sampler (equirect/irradiance/prefilter)
    auto makeIBLLayoutFull = [&]() -> VkDescriptorSetLayout {
        VkDescriptorSetLayoutBinding bs[4]{};
        bs[0] = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        bs[1] = { 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        bs[2] = { 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        bs[3] = { 3, VK_DESCRIPTOR_TYPE_SAMPLER,        1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        VkDescriptorSetLayoutCreateInfo li{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        li.bindingCount = 4; li.pBindings = bs;
        VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
        vkCreateDescriptorSetLayout(dev, &li, nullptr, &dsl);
        return dsl;
    };

    // Layout with only StorageImage at binding 0 (brdfLUT shader: no cbuffer, no sampler)
    auto makeIBLLayoutBrdf = [&]() -> VkDescriptorSetLayout {
        VkDescriptorSetLayoutBinding bs{ 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        VkDescriptorSetLayoutCreateInfo li{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        li.bindingCount = 1; li.pBindings = &bs;
        VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
        vkCreateDescriptorSetLayout(dev, &li, nullptr, &dsl);
        return dsl;
    };

    // Store DSLs as members ??kept alive until DestroyIBLResources (after pipeline layouts destroyed)
    _vkEquirectDSL  = makeIBLLayoutFull();
    _vkIrrConvDSL   = makeIBLLayoutFull();
    _vkPrefilterDSL = makeIBLLayoutFull();
    _vkBrdfLutDSL   = makeIBLLayoutBrdf();

    if (!makeComputePipeline(L"equirect_to_cube_vk.comp.glsl",  _vkEquirectDSL,  0, _vkEquirectPipeLayout,  _vkEquirectPipeline)  ||
        !makeComputePipeline(L"irradiance_conv_vk.comp.glsl",   _vkIrrConvDSL,   0, _vkIrrConvPipeLayout,   _vkIrrConvPipeline)   ||
        !makeComputePipeline(L"prefilter_env_vk.comp.glsl",     _vkPrefilterDSL, 0, _vkPrefilterPipeLayout, _vkPrefilterPipeline) ||
        !makeComputePipeline(L"brdf_lut_vk.comp.glsl",          _vkBrdfLutDSL,   0, _vkBrdfLutPipeLayout,   _vkBrdfLutPipeline))
    {
        LUNA_LOG_ERROR("VK IBL: failed to compile IBL compute shaders");
        return false;
    }

    return true;
}

bool VulkanBackend::DispatchIBLPrecompute(VkImage equirectSrc, VkImageView equirectView)
{
    VkDevice dev = _device->GetDevice();

    // Create a linear sampler for IBL equirect sampling
    VkSampler linearSampler = VK_NULL_HANDLE;
    {
        VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        si.magFilter    = VK_FILTER_LINEAR;
        si.minFilter    = VK_FILTER_LINEAR;
        si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxAnisotropy = 1.0f;
        si.maxLod        = 1.0f;
        vkCreateSampler(dev, &si, nullptr, &linearSampler);
    }

    // Helper: create a one-time descriptor pool + set with given layout
    // Layout must have already been embedded in the pipeline layout (destroyed after pipeline creation)
    // So we re-create small descriptor pools on the fly here.
    auto allocSet = [&](VkDescriptorPool& pool,
                         const std::vector<VkDescriptorPoolSize>& sizes,
                         VkDescriptorSetLayout dsl) -> VkDescriptorSet {
        VkDescriptorPoolCreateInfo pi{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pi.maxSets       = 1;
        pi.poolSizeCount = (uint32_t)sizes.size();
        pi.pPoolSizes    = sizes.data();
        vkCreateDescriptorPool(dev, &pi, nullptr, &pool);
        VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool     = pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts        = &dsl;
        VkDescriptorSet ds = VK_NULL_HANDLE;
        vkAllocateDescriptorSets(dev, &ai, &ds);
        return ds;
    };

    // All 4 IBL stages in a single command buffer (submit once at end)
    VkCommandBuffer cmd = BeginSingleTimeCommands();

    // Helper: transition a single image (all layers, given mip range) inline
    auto transitionInline = [&](VkImage img, VkImageLayout oldL, VkImageLayout newL,
                                  VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage,
                                  VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                                  uint32_t baseMip, uint32_t mipCount, uint32_t layers) {
        VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.oldLayout           = oldL; b.newLayout = newL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image               = img;
        b.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, baseMip, mipCount, 0, layers };
        b.srcAccessMask       = srcAccess;
        b.dstAccessMask       = dstAccess;
        vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
    };

    // Helper: create a CB UBO with given data (for IBL cbuffer at binding 0)
    // Returned buffer + memory must be destroyed after EndSingleTimeCommands
    auto makeCB = [&](const void* data, VkDeviceSize sz,
                       VkBuffer& buf, VkDeviceMemory& mem) {
        CreateBuffer(sz, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, buf, mem);
        void* p; vkMapMemory(dev, mem, 0, sz, 0, &p);
        memcpy(p, data, (size_t)sz);
        vkUnmapMemory(dev, mem);
    };

    // Use the member DSLs kept alive in CreateIBLResources (not destroyed early)
    VkDescriptorSetLayout equirectDSL  = _vkEquirectDSL;
    VkDescriptorSetLayout irrConvDSL   = _vkIrrConvDSL;
    VkDescriptorSetLayout prefilterDSL = _vkPrefilterDSL;
    VkDescriptorSetLayout brdfLutDSL   = _vkBrdfLutDSL;

    // Cleanup list (pools/buffers/memories destroyed after submit; DSLs owned by members)
    std::vector<VkDescriptorPool> tempPools;
    std::vector<VkBuffer>         tempBufs;
    std::vector<VkDeviceMemory>   tempMems;

    // ?�?� Stage 1: Equirect ??EnvCube ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
    {
        uint32_t cbData[4] = { VK_ENV_CUBE_SIZE, 0, 0, 0 };
        VkBuffer cbBuf = VK_NULL_HANDLE; VkDeviceMemory cbMem = VK_NULL_HANDLE;
        makeCB(cbData, 16, cbBuf, cbMem);
        tempBufs.push_back(cbBuf); tempMems.push_back(cbMem);

        transitionInline(_vkEnvCubemap,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, VK_ACCESS_SHADER_WRITE_BIT, 0, 1, 6);

        VkDescriptorPool pool = VK_NULL_HANDLE;
        std::vector<VkDescriptorPoolSize> psz = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 },
            { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  1 },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  1 },
            { VK_DESCRIPTOR_TYPE_SAMPLER,        1 },
        };
        VkDescriptorSet ds = allocSet(pool, psz, equirectDSL);
        tempPools.push_back(pool);

        VkDescriptorBufferInfo cbBI{ cbBuf, 0, 16 };
        VkDescriptorImageInfo  srcII{ VK_NULL_HANDLE, equirectView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo  dstII{ VK_NULL_HANDLE, _vkEnvCubemapArray, VK_IMAGE_LAYOUT_GENERAL };
        VkDescriptorImageInfo  smpII{ linearSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
        VkWriteDescriptorSet ws[4]{};
        ws[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ds, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,  nullptr, &cbBI,  nullptr };
        ws[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ds, 1, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  &srcII,  nullptr, nullptr };
        ws[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ds, 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  &dstII,  nullptr, nullptr };
        ws[3] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ds, 3, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER,        &smpII,  nullptr, nullptr };
        vkUpdateDescriptorSets(dev, 4, ws, 0, nullptr);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _vkEquirectPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _vkEquirectPipeLayout, 0, 1, &ds, 0, nullptr);
        uint32_t g = (VK_ENV_CUBE_SIZE + 7) / 8;
        vkCmdDispatch(cmd, g, g, 6);

        transitionInline(_vkEnvCubemap,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, 0, 1, 6);
    }

    // ?�?� Stage 2: Irradiance convolution ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
    {
        uint32_t cbData[4] = { VK_IRR_CUBE_SIZE, 0, 0, 0 };
        VkBuffer cbBuf = VK_NULL_HANDLE; VkDeviceMemory cbMem = VK_NULL_HANDLE;
        makeCB(cbData, 16, cbBuf, cbMem);
        tempBufs.push_back(cbBuf); tempMems.push_back(cbMem);

        transitionInline(_vkIrrCubemap,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, VK_ACCESS_SHADER_WRITE_BIT, 0, 1, 6);

        VkDescriptorPool pool = VK_NULL_HANDLE;
        std::vector<VkDescriptorPoolSize> psz = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 },
            { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  1 },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  1 },
            { VK_DESCRIPTOR_TYPE_SAMPLER,        1 },
        };
        VkDescriptorSet ds = allocSet(pool, psz, irrConvDSL);
        tempPools.push_back(pool);

        VkDescriptorBufferInfo cbBI{ cbBuf, 0, 16 };
        VkDescriptorImageInfo  srcII{ VK_NULL_HANDLE, _vkEnvCubemapView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo  dstII{ VK_NULL_HANDLE, _vkIrrCubemapArray, VK_IMAGE_LAYOUT_GENERAL };
        VkDescriptorImageInfo  smpII{ linearSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
        VkWriteDescriptorSet ws[4]{};
        ws[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ds, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &cbBI,  nullptr };
        ws[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ds, 1, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &srcII,  nullptr, nullptr };
        ws[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ds, 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &dstII,  nullptr, nullptr };
        ws[3] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ds, 3, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER,       &smpII,  nullptr, nullptr };
        vkUpdateDescriptorSets(dev, 4, ws, 0, nullptr);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _vkIrrConvPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _vkIrrConvPipeLayout, 0, 1, &ds, 0, nullptr);
        uint32_t g = (VK_IRR_CUBE_SIZE + 7) / 8;
        vkCmdDispatch(cmd, g, g, 6);

        transitionInline(_vkIrrCubemap,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, 0, 1, 6);
    }

    // ?�?� Stage 3: Prefilter environment (one dispatch per mip) ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
    // Transition all prefilter mips to GENERAL first
    transitionInline(_vkPrefilterCubemap,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, VK_ACCESS_SHADER_WRITE_BIT, 0, VK_PREFILTER_MIP_COUNT, 6);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _vkPrefilterPipeline);
    for (uint32_t mip = 0; mip < VK_PREFILTER_MIP_COUNT; mip++) {
        float roughness = (float)mip / float(VK_PREFILTER_MIP_COUNT - 1u);
        uint32_t mipSize = std::max(1u, VK_PREFILTER_CUBE_SIZE >> mip);

        struct PrefilterCB { uint32_t faceSize, mipLevel, numMips; float roughness; };
        PrefilterCB cbData{ mipSize, mip, VK_PREFILTER_MIP_COUNT, roughness };
        VkBuffer cbBuf = VK_NULL_HANDLE; VkDeviceMemory cbMem = VK_NULL_HANDLE;
        makeCB(&cbData, 16, cbBuf, cbMem);
        tempBufs.push_back(cbBuf); tempMems.push_back(cbMem);

        VkDescriptorPool pool = VK_NULL_HANDLE;
        std::vector<VkDescriptorPoolSize> psz = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 },
            { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  1 },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  1 },
            { VK_DESCRIPTOR_TYPE_SAMPLER,        1 },
        };
        VkDescriptorSet ds = allocSet(pool, psz, prefilterDSL);
        tempPools.push_back(pool);

        VkDescriptorBufferInfo cbBI{ cbBuf, 0, 16 };
        VkDescriptorImageInfo  srcII{ VK_NULL_HANDLE, _vkEnvCubemapView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo  dstII{ VK_NULL_HANDLE, _vkPrefilterMipView[mip], VK_IMAGE_LAYOUT_GENERAL };
        VkDescriptorImageInfo  smpII{ linearSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
        VkWriteDescriptorSet ws[4]{};
        ws[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ds, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &cbBI,  nullptr };
        ws[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ds, 1, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &srcII,  nullptr, nullptr };
        ws[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ds, 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &dstII,  nullptr, nullptr };
        ws[3] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ds, 3, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER,       &smpII,  nullptr, nullptr };
        vkUpdateDescriptorSets(dev, 4, ws, 0, nullptr);

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _vkPrefilterPipeLayout, 0, 1, &ds, 0, nullptr);
        uint32_t g = std::max(1u, (mipSize + 7) / 8);
        vkCmdDispatch(cmd, g, g, 6);
    }
    transitionInline(_vkPrefilterCubemap,
        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, 0, VK_PREFILTER_MIP_COUNT, 6);

    // ?�?� Stage 4: BRDF LUT ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
    // brdf_lut_vk.comp.hlsl: binding=0 is RWTexture2D (STORAGE_IMAGE) ??no cbuffer, no sampler
    {
        transitionInline(_vkBrdfLUT,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, VK_ACCESS_SHADER_WRITE_BIT, 0, 1, 1);

        VkDescriptorPool pool = VK_NULL_HANDLE;
        std::vector<VkDescriptorPoolSize> psz = {
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 },
        };
        VkDescriptorSet ds = allocSet(pool, psz, brdfLutDSL);
        tempPools.push_back(pool);

        VkDescriptorImageInfo dstII{ VK_NULL_HANDLE, _vkBrdfLUTView, VK_IMAGE_LAYOUT_GENERAL };
        VkWriteDescriptorSet ws{};
        ws = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ds, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &dstII, nullptr, nullptr };
        vkUpdateDescriptorSets(dev, 1, &ws, 0, nullptr);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _vkBrdfLutPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _vkBrdfLutPipeLayout, 0, 1, &ds, 0, nullptr);
        uint32_t g = (VK_BRDF_LUT_SIZE + 15) / 16;
        vkCmdDispatch(cmd, g, g, 1);

        transitionInline(_vkBrdfLUT,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, 0, 1, 1);
    }

    EndSingleTimeCommands(cmd);

    // Cleanup temp resources (DSLs are owned by members, destroyed in DestroyIBLResources)
    vkDestroySampler(dev, linearSampler, nullptr);
    for (auto p : tempPools) vkDestroyDescriptorPool(dev, p, nullptr);
    for (auto b : tempBufs)  vkDestroyBuffer(dev, b, nullptr);
    for (auto m : tempMems)  vkFreeMemory(dev, m, nullptr);

    LUNA_LOG_INFO("VK IBL: precompute done (envCube + irrCube + prefilterCube + brdfLUT)");
    return true;
}

// ===========================================================================
// Phase 18B: Vulkan Motion Blur pass
// Entry:  _hdrImage = SHADER_READ_ONLY_OPTIMAL (after deferred lighting)
//         _depthImage = DEPTH_STENCIL_READ_ONLY_OPTIMAL (G-buffer writes depth, left readable)
// Exit:   _mbImage = SHADER_READ_ONLY_OPTIMAL (ppRenderPass finalLayout)
// ===========================================================================
void VulkanBackend::DrawVKMotionBlurPass()
{
    if (!_vkMBPipeline || !_mbFB || !_mbView) return;

    VkDevice        dev  = _device->GetDevice();
    VkCommandBuffer cmd  = _frames[_frameIndex].cmdBuffer;
    uint32_t        W    = _swapchainExtent.width;
    uint32_t        H    = _swapchainExtent.height;

    // Build and upload MB constants
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

    // Store current VP for next frame
    XMFLOAT4X4 vpF; XMStoreFloat4x4(&vpF, VP);
    _vkMBLastVP = vpF;

    // Begin render pass targeting _mbFB (ppRenderPass: UNDEFINED?�SHADER_READ_ONLY)
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
    // _mbImage is now SHADER_READ_ONLY_OPTIMAL (ppRenderPass finalLayout)
}

// ===========================================================================
// Phase 18D: Vulkan Ray Tracing ??stubs (full implementation guarded by _rtSupported)
// ===========================================================================
bool VulkanBackend::BuildAccelerationStructures()
{
    if (!_rtSupported || _meshASInfoCache.empty() || !_mergedVB || !_mergedIB) return false;
    VkDevice dev = _device->GetDevice();

    // Helper: buffer device address
    auto getAddr = [&](VkBuffer buf) -> VkDeviceAddress {
        VkBufferDeviceAddressInfo i{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
        i.buffer = buf;
        return vkGetBufferDeviceAddress(dev, &i);
    };

    // Helper: allocate AS-backing or scratch buffer (DEVICE_LOCAL + SHADER_DEVICE_ADDRESS)
    auto makeASBuf = [&](VkDeviceSize sz, VkBufferUsageFlags extra, VkBuffer& b, VkDeviceMemory& m) {
        return CreateBuffer(sz,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | extra,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, b, m);
    };

    VkDeviceAddress vbAddr = getAddr(_mergedVB);
    VkDeviceAddress ibAddr = getAddr(_mergedIB);

    // ?�?� Build one BLAS per mesh ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
    _vkBLASes.clear();
    _vkBLASes.reserve(_meshASInfoCache.size());

    for (size_t mi = 0; mi < _meshASInfoCache.size(); mi++)
    {
        const MeshASInfo& info = _meshASInfoCache[mi];

        VkAccelerationStructureGeometryTrianglesDataKHR tris{};
        tris.sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        tris.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;               // position at byte offset 0
        tris.vertexData.deviceAddress = vbAddr + (VkDeviceSize)info.vertexOffset * 48u; // sizeof(PBRVertex)=48
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
        // Memory barrier: BLAS write must complete before TLAS build reads it
        VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        mb.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        mb.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        vkCmdPipelineBarrier(blasCmd,
            VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            0, 1, &mb, 0, nullptr, 0, nullptr);
        EndSingleTimeCommands(blasCmd);

        vkDestroyBuffer(dev, scratchBuf, nullptr);
        vkFreeMemory(dev, scratchMem, nullptr);

        _vkBLASes.push_back(blas);
    }

    // ?�?� Build TLAS (one instance per BLAS, identity transform) ?�?�?�?�?�?�?�?�?�?�?�?�?�?�
    std::vector<VkAccelerationStructureInstanceKHR> instances(_vkBLASes.size());
    for (size_t i = 0; i < _vkBLASes.size(); i++)
    {
        VkAccelerationStructureDeviceAddressInfoKHR addrInfo{};
        addrInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
        addrInfo.accelerationStructure = _vkBLASes[i].as;

        VkAccelerationStructureInstanceKHR inst{};
        // Identity transform (3×4 row-major: diagonal = 1, rest = 0)
        inst.transform.matrix[0][0] = inst.transform.matrix[1][1] = inst.transform.matrix[2][2] = 1.0f;
        inst.instanceCustomIndex    = (uint32_t)i;
        inst.mask                   = 0xFF;
        inst.instanceShaderBindingTableRecordOffset = 0;
        inst.flags                  = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        inst.accelerationStructureReference = pfn_vkGetAccelerationStructureDeviceAddressKHR(dev, &addrInfo);
        instances[i] = inst;
    }

    // Instance buffer (HOST_VISIBLE for simplicity ??static scene)
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

    vkDestroyBuffer(dev, tlasScratch, nullptr);
    vkFreeMemory(dev, tlasScratchMem, nullptr);

    LUNA_LOG_INFO("VK RT: Built %zu BLAS(es) + TLAS (%u instances)",
                  _vkBLASes.size(), instCount);
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
    uint32_t W = _swapchainExtent.width, H = _swapchainExtent.height;

    // 1. Shadow mask image (R8_UNORM, STORAGE | SAMPLED, full-res)
    if (!CreateImage(W, H, VK_FORMAT_R8_UNORM, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, _vkShadowMaskImage, _vkShadowMaskMem))
        return false;
    _vkShadowMaskView = CreateImageView(_vkShadowMaskImage, VK_FORMAT_R8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);

    // 2. Query RT pipeline properties for SBT layout
    auto getPhysProps2 = (PFN_vkGetPhysicalDeviceProperties2)
        vkGetInstanceProcAddr(_instance, "vkGetPhysicalDeviceProperties2");
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProps{};
    rtProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
    VkPhysicalDeviceProperties2 devProps2{};
    devProps2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    devProps2.pNext = &rtProps;
    getPhysProps2(_device->GetPhysicalDevice(), &devProps2);

    const uint32_t handleSize  = rtProps.shaderGroupHandleSize;       // typically 32
    const uint32_t baseAlign   = rtProps.shaderGroupBaseAlignment;    // typically 64
    // Each SBT entry occupies one baseAlign-aligned slot
    const uint32_t entryStride = baseAlign;

    // 3. DSL: binding 0=TLAS, 1=STORAGE_IMAGE, 2=COMBINED_IMAGE_SAMPLER(depth), 3=COMBINED_IMAGE_SAMPLER(normal), 4=UBO
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

    // 4. Pipeline layout
    VkPipelineLayoutCreateInfo pli{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 1; pli.pSetLayouts = &_vkRTLayout;
    vkCreatePipelineLayout(dev, &pli, nullptr, &_vkRTPipeLayout);

    // 5. Descriptor pool + per-frame sets
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

    // 6. Per-frame RT scene UBOs (256B) + descriptor sets
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

        // Write all bindings for this frame's set
        VkWriteDescriptorSetAccelerationStructureKHR asWrite{};
        asWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
        asWrite.accelerationStructureCount = 1;
        asWrite.pAccelerationStructures    = &_vkTLAS.as;

        VkDescriptorImageInfo shadowII{ VK_NULL_HANDLE,        _vkShadowMaskView, VK_IMAGE_LAYOUT_GENERAL };
        VkDescriptorImageInfo depthII { _pointClampSampler,   _depthView,        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo normalII{ _pointClampSampler,   _gbNormalView,     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorBufferInfo uboBI  { _vkRTSceneCB[i], 0, 256 };

        VkWriteDescriptorSet ws[5]{};
        ws[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, &asWrite, _vkRTDescSet[i], 0, 0, 1, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, nullptr,   nullptr, nullptr };
        ws[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,  _vkRTDescSet[i], 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,              &shadowII, nullptr, nullptr };
        ws[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,  _vkRTDescSet[i], 2, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,     &depthII,  nullptr, nullptr };
        ws[3] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,  _vkRTDescSet[i], 3, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,     &normalII, nullptr, nullptr };
        ws[4] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,  _vkRTDescSet[i], 4, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,             nullptr,   &uboBI,  nullptr };
        vkUpdateDescriptorSets(dev, 5, ws, 0, nullptr);
    }

    // 7. Compile RT shaders (lib_6_5 ??SPIR-V)
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

    // 8. RT pipeline: 3 stages + 3 groups (raygen, miss, chit)
    VkPipelineShaderStageCreateInfo stages[3]{};
    stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                  VK_SHADER_STAGE_RAYGEN_BIT_KHR,       rgenMod,  "main", nullptr };
    stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                  VK_SHADER_STAGE_MISS_BIT_KHR,         rmissMod, "main", nullptr };
    stages[2] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                  VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,  rchitMod, "main", nullptr };

    VkRayTracingShaderGroupCreateInfoKHR groups[3]{};
    // Group 0: raygen (GENERAL)
    groups[0].sType              = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[0].type               = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[0].generalShader      = 0;
    groups[0].closestHitShader   = VK_SHADER_UNUSED_KHR;
    groups[0].anyHitShader       = VK_SHADER_UNUSED_KHR;
    groups[0].intersectionShader = VK_SHADER_UNUSED_KHR;
    // Group 1: miss (GENERAL)
    groups[1] = groups[0];
    groups[1].generalShader = 1;
    // Group 2: closest hit (TRIANGLES_HIT_GROUP)
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

    // 9. Shader Binding Table (SBT)
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

    // Update the deferred G-buffer descriptor set (binding 13 = shadow mask)
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

void VulkanBackend::DestroyIBLResources()
{
    VkDevice dev = _device->GetDevice();
    _iblReady = false;

    if (_vkBrdfLutPipeline)      { vkDestroyPipeline(dev, _vkBrdfLutPipeline, nullptr);          _vkBrdfLutPipeline     = VK_NULL_HANDLE; }
    if (_vkBrdfLutPipeLayout)    { vkDestroyPipelineLayout(dev, _vkBrdfLutPipeLayout, nullptr);   _vkBrdfLutPipeLayout   = VK_NULL_HANDLE; }
    if (_vkBrdfLutDSL)           { vkDestroyDescriptorSetLayout(dev, _vkBrdfLutDSL, nullptr);     _vkBrdfLutDSL          = VK_NULL_HANDLE; }
    if (_vkPrefilterPipeline)    { vkDestroyPipeline(dev, _vkPrefilterPipeline, nullptr);         _vkPrefilterPipeline   = VK_NULL_HANDLE; }
    if (_vkPrefilterPipeLayout)  { vkDestroyPipelineLayout(dev, _vkPrefilterPipeLayout, nullptr); _vkPrefilterPipeLayout = VK_NULL_HANDLE; }
    if (_vkPrefilterDSL)         { vkDestroyDescriptorSetLayout(dev, _vkPrefilterDSL, nullptr);   _vkPrefilterDSL        = VK_NULL_HANDLE; }
    if (_vkIrrConvPipeline)      { vkDestroyPipeline(dev, _vkIrrConvPipeline, nullptr);           _vkIrrConvPipeline     = VK_NULL_HANDLE; }
    if (_vkIrrConvPipeLayout)    { vkDestroyPipelineLayout(dev, _vkIrrConvPipeLayout, nullptr);   _vkIrrConvPipeLayout   = VK_NULL_HANDLE; }
    if (_vkIrrConvDSL)           { vkDestroyDescriptorSetLayout(dev, _vkIrrConvDSL, nullptr);     _vkIrrConvDSL          = VK_NULL_HANDLE; }
    if (_vkEquirectPipeline)     { vkDestroyPipeline(dev, _vkEquirectPipeline, nullptr);          _vkEquirectPipeline    = VK_NULL_HANDLE; }
    if (_vkEquirectPipeLayout)   { vkDestroyPipelineLayout(dev, _vkEquirectPipeLayout, nullptr);  _vkEquirectPipeLayout  = VK_NULL_HANDLE; }
    if (_vkEquirectDSL)          { vkDestroyDescriptorSetLayout(dev, _vkEquirectDSL, nullptr);    _vkEquirectDSL         = VK_NULL_HANDLE; }

    if (_vkIBLSampler)           { vkDestroySampler(dev, _vkIBLSampler, nullptr);                 _vkIBLSampler  = VK_NULL_HANDLE; }
    if (_vkBrdfSampler)          { vkDestroySampler(dev, _vkBrdfSampler, nullptr);                _vkBrdfSampler = VK_NULL_HANDLE; }

    if (_vkBrdfLUTView)          { vkDestroyImageView(dev, _vkBrdfLUTView, nullptr);              _vkBrdfLUTView = VK_NULL_HANDLE; }
    if (_vkBrdfLUT)              { vkDestroyImage(dev, _vkBrdfLUT, nullptr);                      _vkBrdfLUT     = VK_NULL_HANDLE; }
    if (_vkBrdfLUTMem)           { vkFreeMemory(dev, _vkBrdfLUTMem, nullptr);                     _vkBrdfLUTMem  = VK_NULL_HANDLE; }

    for (uint32_t m = 0; m < VK_PREFILTER_MIP_COUNT; m++)
        if (_vkPrefilterMipView[m]) { vkDestroyImageView(dev, _vkPrefilterMipView[m], nullptr); _vkPrefilterMipView[m] = VK_NULL_HANDLE; }
    if (_vkPrefilterCubemapView) { vkDestroyImageView(dev, _vkPrefilterCubemapView, nullptr); _vkPrefilterCubemapView = VK_NULL_HANDLE; }
    if (_vkPrefilterCubemap)     { vkDestroyImage(dev, _vkPrefilterCubemap, nullptr);         _vkPrefilterCubemap     = VK_NULL_HANDLE; }
    if (_vkPrefilterCubemapMem)  { vkFreeMemory(dev, _vkPrefilterCubemapMem, nullptr);        _vkPrefilterCubemapMem  = VK_NULL_HANDLE; }

    if (_vkIrrCubemapArray)      { vkDestroyImageView(dev, _vkIrrCubemapArray, nullptr);   _vkIrrCubemapArray = VK_NULL_HANDLE; }
    if (_vkIrrCubemapView)       { vkDestroyImageView(dev, _vkIrrCubemapView, nullptr);    _vkIrrCubemapView  = VK_NULL_HANDLE; }
    if (_vkIrrCubemap)           { vkDestroyImage(dev, _vkIrrCubemap, nullptr);            _vkIrrCubemap      = VK_NULL_HANDLE; }
    if (_vkIrrCubemapMem)        { vkFreeMemory(dev, _vkIrrCubemapMem, nullptr);           _vkIrrCubemapMem   = VK_NULL_HANDLE; }

    if (_vkEnvCubemapArray)      { vkDestroyImageView(dev, _vkEnvCubemapArray, nullptr);   _vkEnvCubemapArray = VK_NULL_HANDLE; }
    if (_vkEnvCubemapView)       { vkDestroyImageView(dev, _vkEnvCubemapView, nullptr);    _vkEnvCubemapView  = VK_NULL_HANDLE; }
    if (_vkEnvCubemap)           { vkDestroyImage(dev, _vkEnvCubemap, nullptr);            _vkEnvCubemap      = VK_NULL_HANDLE; }
    if (_vkEnvCubemapMem)        { vkFreeMemory(dev, _vkEnvCubemapMem, nullptr);           _vkEnvCubemapMem   = VK_NULL_HANDLE; }
}

} // namespace Luna
