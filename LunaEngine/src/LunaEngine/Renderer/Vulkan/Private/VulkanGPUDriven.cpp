#include "LunaPCH.h"
#include "LunaEngine/Renderer/Vulkan/Public/VulkanGPUDriven.h"
#include "LunaEngine/Renderer/Vulkan/Public/VulkanCore.h"
#include "LunaEngine/Renderer/Vulkan/Public/VulkanDevice.h"
#include "LunaEngine/Renderer/Mesh.h"
#include "LunaEngine/Utils/FileSystemUtil.h"
#include "Logger/Logger.h"

#include <set>
#include <algorithm>
#include <fstream>

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
    if (glslPath.find(L".vert.") != std::wstring::npos) stage = L"vertex";
    else if (glslPath.find(L".frag.") != std::wstring::npos) stage = L"fragment";
    else if (glslPath.find(L".comp.") != std::wstring::npos) stage = L"compute";
    else stage = L"vertex";

    std::wstring cmdLine = L"\"" + glslcPath + L"\" -fshader-stage=" + stage + L" -o \"" + spvPath + L"\" \"" + glslPath + L"\"";

    PROCESS_INFORMATION pi{};
    STARTUPINFOW si{ sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (!CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    {
        LUNA_LOG_ERROR("VulkanGPUDriven: glslc launch failed");
        return false;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (exitCode != 0)
    {
        LUNA_LOG_ERROR("VulkanGPUDriven: glslc shader compile failed (exit %u)", exitCode);
        DeleteFileW(spvPath.c_str());
        return false;
    }

    std::ifstream ifs(spvPath, std::ios::binary | std::ios::ate);
    if (!ifs) { DeleteFileW(spvPath.c_str()); return false; }
    size_t sz = (size_t)ifs.tellg();
    ifs.seekg(0);
    outSpirv.resize(sz / 4);
    ifs.read(reinterpret_cast<char*>(outSpirv.data()), sz);
    ifs.close();
    DeleteFileW(spvPath.c_str());
    return true;
}

// ---------------------------------------------------------------------------
// Local Structs
// ---------------------------------------------------------------------------

// MeshDrawInfo for cull shader's gMeshInfo SSBO
struct VkMeshDrawInfo
{
    uint32_t indexCount;
    uint32_t firstIndex;
    int32_t  vertexOffset;
    uint32_t _pad;
};
static_assert(sizeof(VkMeshDrawInfo) == 16, "VkMeshDrawInfo must be 16 bytes");

// VkDrawIndexedIndirectCommand equivalent
struct VkDrawIndexedIndirectCmd
{
    uint32_t indexCount;
    uint32_t instanceCount;
    uint32_t firstIndex;
    int32_t  vertexOffset;
    uint32_t firstInstance;  // carries objectIndex
};
static_assert(sizeof(VkDrawIndexedIndirectCmd) == 20, "VkDrawIndexedIndirectCmd must be 20 bytes");

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------

VulkanGPUDriven::~VulkanGPUDriven()
{
    Destroy();
}

// ---------------------------------------------------------------------------
// Create / Destroy
// ---------------------------------------------------------------------------

bool VulkanGPUDriven::Create(const CreateInfo& info)
{
    if (!info.core || !info.gbRenderPassLoad)
    {
        LUNA_LOG_ERROR("VulkanGPUDriven::Create: invalid CreateInfo");
        return false;
    }

    _core = info.core;
    _gbRenderPassLoad = info.gbRenderPassLoad;
    _linearSampler = info.linearSampler;

    // Geometry must be built first
    if (!_geometryBuilt)
    {
        LUNA_LOG_ERROR("VulkanGPUDriven::Create: call BuildMergedGeometry first");
        return false;
    }

    // Create indirect resources with materials
    if (info.materials && !info.materials->empty())
    {
        if (!CreateIndirectResources(*info.materials))
        {
            LUNA_LOG_ERROR("VulkanGPUDriven::Create: CreateIndirectResources failed");
            return false;
        }
    }

    // Create async compute resources (optional — may fail if no dedicated queue)
    CreateAsyncComputeResources();

    _ready = true;
    LUNA_LOG_INFO("VulkanGPUDriven: created");
    return true;
}

void VulkanGPUDriven::Destroy()
{
    if (!_core) return;
    VkDevice dev = _core->GetDevice();
    if (dev == VK_NULL_HANDLE) return;

    _ready = false;

    DestroyAsyncComputeResources();

    // Cull pipeline
    if (_cullPipeline)    { vkDestroyPipeline(dev, _cullPipeline, nullptr);           _cullPipeline   = VK_NULL_HANDLE; }
    if (_cullPipeLayout)  { vkDestroyPipelineLayout(dev, _cullPipeLayout, nullptr);   _cullPipeLayout = VK_NULL_HANDLE; }
    if (_cullDescPool)    { vkDestroyDescriptorPool(dev, _cullDescPool, nullptr);     _cullDescPool   = VK_NULL_HANDLE; }
    if (_cullDescLayout)  { vkDestroyDescriptorSetLayout(dev, _cullDescLayout, nullptr); _cullDescLayout = VK_NULL_HANDLE; }

    // Indirect G-buffer pipeline
    if (_indirectGBufPipeline) { vkDestroyPipeline(dev, _indirectGBufPipeline, nullptr);      _indirectGBufPipeline = VK_NULL_HANDLE; }
    if (_indirectPipeLayout)   { vkDestroyPipelineLayout(dev, _indirectPipeLayout, nullptr);  _indirectPipeLayout   = VK_NULL_HANDLE; }

    // Material descriptors
    if (_materialPool)   { vkDestroyDescriptorPool(dev, _materialPool, nullptr);           _materialPool   = VK_NULL_HANDLE; }
    if (_materialLayout) { vkDestroyDescriptorSetLayout(dev, _materialLayout, nullptr);    _materialLayout = VK_NULL_HANDLE; }

    // VS descriptors
    if (_vsPool)   { vkDestroyDescriptorPool(dev, _vsPool, nullptr);           _vsPool   = VK_NULL_HANDLE; }
    if (_vsLayout) { vkDestroyDescriptorSetLayout(dev, _vsLayout, nullptr);    _vsLayout = VK_NULL_HANDLE; }

    // ViewProj UBO
    if (_viewProjMapped) { vkUnmapMemory(dev, _viewProjMem);                   _viewProjMapped = nullptr; }
    if (_viewProjBuf)    { vkDestroyBuffer(dev, _viewProjBuf, nullptr);        _viewProjBuf    = VK_NULL_HANDLE; }
    if (_viewProjMem)    { vkFreeMemory(dev, _viewProjMem, nullptr);           _viewProjMem    = VK_NULL_HANDLE; }

    // Object data SSBO
    if (_objectDataMapped) { vkUnmapMemory(dev, _objectDataMem);               _objectDataMapped = nullptr; }
    if (_objectDataBuffer) { vkDestroyBuffer(dev, _objectDataBuffer, nullptr); _objectDataBuffer = VK_NULL_HANDLE; }
    if (_objectDataMem)    { vkFreeMemory(dev, _objectDataMem, nullptr);       _objectDataMem    = VK_NULL_HANDLE; }

    // Per-frame buffers
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++)
    {
        if (_indirectArgBuffer[i]) { vkDestroyBuffer(dev, _indirectArgBuffer[i], nullptr); _indirectArgBuffer[i] = VK_NULL_HANDLE; }
        if (_indirectArgMem[i])    { vkFreeMemory(dev, _indirectArgMem[i], nullptr);       _indirectArgMem[i]    = VK_NULL_HANDLE; }
        if (_drawCountBuffer[i])   { vkDestroyBuffer(dev, _drawCountBuffer[i], nullptr);   _drawCountBuffer[i]   = VK_NULL_HANDLE; }
        if (_drawCountMem[i])      { vkFreeMemory(dev, _drawCountMem[i], nullptr);         _drawCountMem[i]      = VK_NULL_HANDLE; }
    }

    // Material factor SSBO
    if (_matFactorBuffer) { vkDestroyBuffer(dev, _matFactorBuffer, nullptr); _matFactorBuffer = VK_NULL_HANDLE; }
    if (_matFactorMem)    { vkFreeMemory(dev, _matFactorMem, nullptr);       _matFactorMem    = VK_NULL_HANDLE; }

    DestroyMergedGeometry();

    _cpuInstances.clear();
    _core = nullptr;
}

void VulkanGPUDriven::DestroyMergedGeometry()
{
    if (!_core) return;
    VkDevice dev = _core->GetDevice();
    if (dev == VK_NULL_HANDLE) return;

    if (_mergedVB)    { vkDestroyBuffer(dev, _mergedVB, nullptr);    _mergedVB    = VK_NULL_HANDLE; }
    if (_mergedVBMem) { vkFreeMemory(dev, _mergedVBMem, nullptr);    _mergedVBMem = VK_NULL_HANDLE; }
    if (_mergedIB)    { vkDestroyBuffer(dev, _mergedIB, nullptr);    _mergedIB    = VK_NULL_HANDLE; }
    if (_mergedIBMem) { vkFreeMemory(dev, _mergedIBMem, nullptr);    _mergedIBMem = VK_NULL_HANDLE; }
    if (_meshInfoBuf) { vkDestroyBuffer(dev, _meshInfoBuf, nullptr); _meshInfoBuf = VK_NULL_HANDLE; }
    if (_meshInfoMem) { vkFreeMemory(dev, _meshInfoMem, nullptr);    _meshInfoMem = VK_NULL_HANDLE; }

    _geometryBuilt = false;
}

// ---------------------------------------------------------------------------
// BuildMergedGeometry
// ---------------------------------------------------------------------------

void VulkanGPUDriven::BuildMergedGeometry(
    const std::vector<std::vector<PBRVertex>>& allVerts,
    const std::vector<std::vector<uint32_t>>& allIdxs,
    bool rtSupported)
{
    if (allVerts.empty()) return;
    if (!_core) return;

    VkDevice dev = _core->GetDevice();

    // Flatten vertex/index data and build meshInfo SSBO
    std::vector<PBRVertex>      mergedV;
    std::vector<uint32_t>       mergedI;
    std::vector<VkMeshDrawInfo> meshInfos;

    for (size_t i = 0; i < allVerts.size(); i++)
    {
        VkMeshDrawInfo mi{};
        mi.indexCount   = (uint32_t)allIdxs[i].size();
        mi.firstIndex   = (uint32_t)mergedI.size();
        mi.vertexOffset = (int32_t)mergedV.size();
        meshInfos.push_back(mi);

        mergedV.insert(mergedV.end(), allVerts[i].begin(), allVerts[i].end());
        mergedI.insert(mergedI.end(), allIdxs[i].begin(), allIdxs[i].end());
    }

    // Merged vertex buffer (device-local via staging)
    {
        VkDeviceSize sz = mergedV.size() * sizeof(PBRVertex);
        VkBuffer stg; VkDeviceMemory stgMem;
        _core->CreateBuffer(sz, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            stg, stgMem);
        void* p; vkMapMemory(dev, stgMem, 0, sz, 0, &p);
        memcpy(p, mergedV.data(), (size_t)sz);
        vkUnmapMemory(dev, stgMem);

        VkBufferUsageFlags vbUsage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        if (rtSupported)
            vbUsage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR
                     | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        _core->CreateBuffer(sz, vbUsage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, _mergedVB, _mergedVBMem);
        _core->CopyBuffer(stg, _mergedVB, sz);
        vkDestroyBuffer(dev, stg, nullptr); vkFreeMemory(dev, stgMem, nullptr);
    }

    // Merged index buffer
    {
        VkDeviceSize sz = mergedI.size() * sizeof(uint32_t);
        VkBuffer stg; VkDeviceMemory stgMem;
        _core->CreateBuffer(sz, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            stg, stgMem);
        void* p; vkMapMemory(dev, stgMem, 0, sz, 0, &p);
        memcpy(p, mergedI.data(), (size_t)sz);
        vkUnmapMemory(dev, stgMem);

        VkBufferUsageFlags ibUsage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        if (rtSupported)
            ibUsage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR
                     | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        _core->CreateBuffer(sz, ibUsage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, _mergedIB, _mergedIBMem);
        _core->CopyBuffer(stg, _mergedIB, sz);
        vkDestroyBuffer(dev, stg, nullptr); vkFreeMemory(dev, stgMem, nullptr);
    }

    // Mesh info SSBO (device-local via staging)
    {
        VkDeviceSize sz = meshInfos.size() * sizeof(VkMeshDrawInfo);
        VkBuffer stg; VkDeviceMemory stgMem;
        _core->CreateBuffer(sz, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            stg, stgMem);
        void* p; vkMapMemory(dev, stgMem, 0, sz, 0, &p);
        memcpy(p, meshInfos.data(), (size_t)sz);
        vkUnmapMemory(dev, stgMem);

        _core->CreateBuffer(sz,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, _meshInfoBuf, _meshInfoMem);
        _core->CopyBuffer(stg, _meshInfoBuf, sz);
        vkDestroyBuffer(dev, stg, nullptr); vkFreeMemory(dev, stgMem, nullptr);
    }

    _geometryBuilt = true;

    LUNA_LOG_INFO("VulkanGPUDriven: merged geometry — %zu verts, %zu indices, %zu meshes",
        mergedV.size(), mergedI.size(), meshInfos.size());
}

// ---------------------------------------------------------------------------
// CreateIndirectResources
// ---------------------------------------------------------------------------

bool VulkanGPUDriven::CreateIndirectResources(const std::vector<MaterialData>& uniqueMats)
{
    if (!_mergedVB || !_mergedIB || !_meshInfoBuf) return false;

    VkDevice dev = _core->GetDevice();
    uint32_t matCount = (uint32_t)uniqueMats.size();

    // Object data SSBO (HOST_VISIBLE|COHERENT, persistently mapped)
    {
        VkDeviceSize sz = MAX_GPU_OBJECTS * sizeof(GPUObjectData);
        if (!_core->CreateBuffer(sz,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                _objectDataBuffer, _objectDataMem))
            return false;
        vkMapMemory(dev, _objectDataMem, 0, sz, 0, &_objectDataMapped);
    }

    // Per-frame indirect arg + draw count buffers (DEVICE_LOCAL)
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++)
    {
        VkDeviceSize argSz   = MAX_GPU_OBJECTS * sizeof(VkDrawIndexedIndirectCmd);
        VkDeviceSize countSz = sizeof(uint32_t);

        if (!_core->CreateBuffer(argSz,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                _indirectArgBuffer[i], _indirectArgMem[i]))
            return false;

        if (!_core->CreateBuffer(countSz,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                _drawCountBuffer[i], _drawCountMem[i]))
            return false;
    }

    // Material factor SSBO
    if (matCount > 0)
    {
        std::vector<MaterialFactors> factors(matCount);
        for (uint32_t i = 0; i < matCount; i++)
        {
            const auto& m = uniqueMats[i];
            factors[i].albedoR = m.albedoFactor[0];
            factors[i].albedoG = m.albedoFactor[1];
            factors[i].albedoB = m.albedoFactor[2];
            factors[i].albedoA = m.albedoFactor[3];
            factors[i].metallicFactor  = m.metallicFactor;
            factors[i].roughnessFactor = m.roughnessFactor;
        }
        VkDeviceSize sz = matCount * sizeof(MaterialFactors);
        VkBuffer stg; VkDeviceMemory stgMem;
        _core->CreateBuffer(sz, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            stg, stgMem);
        void* p; vkMapMemory(dev, stgMem, 0, sz, 0, &p);
        memcpy(p, factors.data(), (size_t)sz);
        vkUnmapMemory(dev, stgMem);

        _core->CreateBuffer(sz,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, _matFactorBuffer, _matFactorMem);
        _core->CopyBuffer(stg, _matFactorBuffer, sz);
        vkDestroyBuffer(dev, stg, nullptr); vkFreeMemory(dev, stgMem, nullptr);
    }

    // --- Descriptor Set Layouts ---

    // Bindless material layout (set=1)
    {
        VkDescriptorSetLayoutBinding bs[6]{};
        bs[0] = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        bs[1] = { 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, std::max(matCount, 1u), VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        bs[2] = { 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, std::max(matCount, 1u), VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        bs[3] = { 3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, std::max(matCount, 1u), VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        bs[4] = { 4, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        bs[5] = { 5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, std::max(matCount, 1u), VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };

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
        vkCreateDescriptorSetLayout(dev, &li, nullptr, &_materialLayout);
    }

    // VS layout (set=0): ViewProj UBO + ObjectData SSBO
    {
        VkDescriptorSetLayoutBinding bs[2]{};
        bs[0] = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr };
        bs[1] = { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr };

        VkDescriptorSetLayoutCreateInfo li{};
        li.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 2;
        li.pBindings    = bs;
        vkCreateDescriptorSetLayout(dev, &li, nullptr, &_vsLayout);
    }

    // Cull layout (set=0): 6 bindings
    {
        VkDescriptorSetLayoutBinding bs[6]{};
        bs[0] = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }; // gObjects
        bs[1] = { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }; // gMeshInfo
        bs[2] = { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }; // gDrawArgs
        bs[3] = { 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }; // gDrawCount
        bs[4] = { 4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }; // hizParams
        bs[5] = { 5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }; // gHiZ

        VkDescriptorSetLayoutCreateInfo li{};
        li.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 6;
        li.pBindings    = bs;
        vkCreateDescriptorSetLayout(dev, &li, nullptr, &_cullDescLayout);
    }

    // --- Descriptor Pools ---

    // Material pool
    {
        VkDescriptorPoolSize psz[] = {
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 },
            { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4 * std::max(matCount, 1u) },
            { VK_DESCRIPTOR_TYPE_SAMPLER, 1 },
        };
        VkDescriptorPoolCreateInfo pi{};
        pi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pi.maxSets       = 1;
        pi.poolSizeCount = 3;
        pi.pPoolSizes    = psz;
        vkCreateDescriptorPool(dev, &pi, nullptr, &_materialPool);
    }

    // VS pool
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
        vkCreateDescriptorPool(dev, &pi, nullptr, &_vsPool);
    }

    // Cull pool
    {
        VkDescriptorPoolSize psz[] = {
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 * FRAMES_IN_FLIGHT },
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 * FRAMES_IN_FLIGHT },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 * FRAMES_IN_FLIGHT },
        };
        VkDescriptorPoolCreateInfo pi{};
        pi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pi.maxSets       = FRAMES_IN_FLIGHT;
        pi.poolSizeCount = 3;
        pi.pPoolSizes    = psz;
        vkCreateDescriptorPool(dev, &pi, nullptr, &_cullDescPool);
    }

    // --- Allocate Descriptor Sets ---

    // Material set
    {
        VkDescriptorSetAllocateInfo ai{};
        ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool     = _materialPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts        = &_materialLayout;
        vkAllocateDescriptorSets(dev, &ai, &_materialDescSet);
    }

    // VS set
    {
        VkDescriptorSetAllocateInfo ai{};
        ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool     = _vsPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts        = &_vsLayout;
        vkAllocateDescriptorSets(dev, &ai, &_vsDescSet);
    }

    // Cull sets (per-frame)
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++)
    {
        VkDescriptorSetAllocateInfo ai{};
        ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool     = _cullDescPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts        = &_cullDescLayout;
        vkAllocateDescriptorSets(dev, &ai, &_cullDescSet[i]);
    }

    // --- ViewProj UBO ---
    {
        if (!_core->CreateBuffer(128,
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                _viewProjBuf, _viewProjMem))
            return false;
        vkMapMemory(dev, _viewProjMem, 0, 128, 0, &_viewProjMapped);
    }

    // --- Write VS Descriptor Set ---
    {
        VkDescriptorBufferInfo vpBI{ _viewProjBuf, 0, 128 };
        VkDescriptorBufferInfo objBI{ _objectDataBuffer, 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet ws[2]{};
        ws[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _vsDescSet, 0, 0, 1,
                  VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &vpBI, nullptr };
        ws[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _vsDescSet, 1, 0, 1,
                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &objBI, nullptr };
        vkUpdateDescriptorSets(dev, 2, ws, 0, nullptr);
    }

    // --- Write Bindless Material Descriptor Set ---
    if (matCount > 0 && _linearSampler != VK_NULL_HANDLE)
    {
        VkDescriptorBufferInfo matBI{ _matFactorBuffer, 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet matW{};
        matW.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        matW.dstSet          = _materialDescSet;
        matW.dstBinding      = 0;
        matW.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        matW.descriptorCount = 1;
        matW.pBufferInfo     = &matBI;
        vkUpdateDescriptorSets(dev, 1, &matW, 0, nullptr);

        // Per-material texture arrays
        std::vector<VkDescriptorImageInfo> albedoII(matCount), normalII(matCount), mrII(matCount), emissiveII(matCount);
        for (uint32_t i = 0; i < matCount; i++)
        {
            const auto& m = uniqueMats[i];
            VkImageView av = m.albedoView;
            VkImageView nv = m.normalView     ? m.normalView     : av;
            VkImageView mv = m.metalRoughView ? m.metalRoughView : av;
            VkImageView ev = m.emissiveView   ? m.emissiveView   : av;
            albedoII[i]   = { VK_NULL_HANDLE, av, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            normalII[i]   = { VK_NULL_HANDLE, nv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            mrII[i]       = { VK_NULL_HANDLE, mv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            emissiveII[i] = { VK_NULL_HANDLE, ev, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        }
        VkDescriptorImageInfo samplerII{ _linearSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };

        VkWriteDescriptorSet ws[5]{};
        ws[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _materialDescSet, 1, 0,
                  matCount, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, albedoII.data(), nullptr, nullptr };
        ws[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _materialDescSet, 2, 0,
                  matCount, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, normalII.data(), nullptr, nullptr };
        ws[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _materialDescSet, 3, 0,
                  matCount, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, mrII.data(), nullptr, nullptr };
        ws[3] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _materialDescSet, 4, 0,
                  1, VK_DESCRIPTOR_TYPE_SAMPLER, &samplerII, nullptr, nullptr };
        ws[4] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _materialDescSet, 5, 0,
                  matCount, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, emissiveII.data(), nullptr, nullptr };
        vkUpdateDescriptorSets(dev, 5, ws, 0, nullptr);
    }

    // --- Write Cull Descriptor Sets (base bindings 0-3, Hi-Z handled externally) ---
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++)
    {
        VkDescriptorBufferInfo objBI { _objectDataBuffer, 0, VK_WHOLE_SIZE };
        VkDescriptorBufferInfo miBI  { _meshInfoBuf, 0, VK_WHOLE_SIZE };
        VkDescriptorBufferInfo argBI { _indirectArgBuffer[i], 0, VK_WHOLE_SIZE };
        VkDescriptorBufferInfo cntBI { _drawCountBuffer[i], 0, sizeof(uint32_t) };

        VkWriteDescriptorSet ws[4]{};
        ws[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _cullDescSet[i], 0, 0, 1,
                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &objBI, nullptr };
        ws[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _cullDescSet[i], 1, 0, 1,
                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &miBI, nullptr };
        ws[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _cullDescSet[i], 2, 0, 1,
                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &argBI, nullptr };
        ws[3] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _cullDescSet[i], 3, 0, 1,
                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &cntBI, nullptr };
        vkUpdateDescriptorSets(dev, 4, ws, 0, nullptr);
    }

    // --- Pipeline Layouts ---

    // Indirect G-buffer pipeline layout
    {
        VkDescriptorSetLayout dsl[] = { _vsLayout, _materialLayout };
        VkPipelineLayoutCreateInfo pli{};
        pli.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = 2;
        pli.pSetLayouts    = dsl;
        vkCreatePipelineLayout(dev, &pli, nullptr, &_indirectPipeLayout);
    }

    // Cull pipeline layout
    {
        VkPushConstantRange pcr{};
        pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pcr.offset     = 0;
        pcr.size       = 128;
        VkPipelineLayoutCreateInfo pli{};
        pli.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount         = 1;
        pli.pSetLayouts            = &_cullDescLayout;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges    = &pcr;
        vkCreatePipelineLayout(dev, &pli, nullptr, &_cullPipeLayout);
    }

    // --- Cull Compute Pipeline ---
    {
        std::vector<uint32_t> csS;
        if (!CompileGLSLtoSPIRV(GetShaderFullPath(L"gpu_cull_vk.comp.glsl").wstring(), csS))
        {
            LUNA_LOG_ERROR("VulkanGPUDriven: gpu_cull_vk compile failed");
            return false;
        }

        VkShaderModuleCreateInfo smi{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        smi.codeSize = csS.size() * 4;
        smi.pCode    = csS.data();
        VkShaderModule csM = VK_NULL_HANDLE;
        vkCreateShaderModule(dev, &smi, nullptr, &csM);

        VkComputePipelineCreateInfo cpi{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        cpi.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpi.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        cpi.stage.module = csM;
        cpi.stage.pName  = "main";
        cpi.layout       = _cullPipeLayout;
        vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &_cullPipeline);
        vkDestroyShaderModule(dev, csM, nullptr);
    }

    // --- Indirect G-Buffer Pipeline ---
    {
        std::vector<uint32_t> vsS, fsS;
        if (!CompileGLSLtoSPIRV(GetShaderFullPath(L"pbr_indirect_vk.vert.glsl").wstring(), vsS) ||
            !CompileGLSLtoSPIRV(GetShaderFullPath(L"gbuffer_indirect_vk.frag.glsl").wstring(), fsS))
        {
            LUNA_LOG_ERROR("VulkanGPUDriven: indirect G-buffer shaders compile failed");
            return false;
        }

        auto mkM = [&](const std::vector<uint32_t>& sp) {
            VkShaderModuleCreateInfo si{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
            si.codeSize = sp.size() * 4;
            si.pCode    = sp.data();
            VkShaderModule m = VK_NULL_HANDLE;
            vkCreateShaderModule(dev, &si, nullptr, &m);
            return m;
        };
        VkShaderModule vsM = mkM(vsS);
        VkShaderModule fsM = mkM(fsS);

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vsM, "main" };
        stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fsM, "main" };

        // PBRVertex: pos(12)+normal(12)+uv(8)+tangent(16) = stride 48
        VkVertexInputBindingDescription vbd{ 0, 48, VK_VERTEX_INPUT_RATE_VERTEX };
        VkVertexInputAttributeDescription vad[4]{};
        vad[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 };
        vad[1] = { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12 };
        vad[2] = { 2, 0, VK_FORMAT_R32G32_SFLOAT, 24 };
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
        vps.viewportCount = 1;
        vps.scissorCount  = 1;

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
        cbs.attachmentCount = 3;
        cbs.pAttachments    = cbas;

        VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dsi{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dsi.dynamicStateCount = 2;
        dsi.pDynamicStates    = dyn;

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
        gpi.renderPass          = _gbRenderPassLoad;
        gpi.subpass             = 0;

        VkResult r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, nullptr, &_indirectGBufPipeline);
        vkDestroyShaderModule(dev, vsM, nullptr);
        vkDestroyShaderModule(dev, fsM, nullptr);
        if (r != VK_SUCCESS)
        {
            LUNA_LOG_ERROR("VulkanGPUDriven: indirect G-buffer pipeline failed: %d", (int)r);
            return false;
        }
    }

    LUNA_LOG_INFO("VulkanGPUDriven: indirect resources created");
    return true;
}

// ---------------------------------------------------------------------------
// Async Compute Resources
// ---------------------------------------------------------------------------

bool VulkanGPUDriven::CreateAsyncComputeResources()
{
    VulkanDevice* vkDev = _core->GetVulkanDevice();
    if (!vkDev || !vkDev->IsAsyncComputeSupported()) return false;

    VkDevice dev = _core->GetDevice();

    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++)
    {
        auto& cf = _computeFrames[i];

        // Command pool (compute queue family)
        VkCommandPoolCreateInfo poolCI{};
        poolCI.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolCI.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolCI.queueFamilyIndex = vkDev->GetComputeQueueFamily();
        if (vkCreateCommandPool(dev, &poolCI, nullptr, &cf.cmdPool) != VK_SUCCESS)
        {
            LUNA_LOG_WARN("VulkanGPUDriven: failed to create compute command pool [%u]", i);
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
            LUNA_LOG_WARN("VulkanGPUDriven: failed to allocate compute command buffer [%u]", i);
            return false;
        }

        // Semaphore
        VkSemaphoreCreateInfo semCI{};
        semCI.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (vkCreateSemaphore(dev, &semCI, nullptr, &cf.doneSemaphore) != VK_SUCCESS)
        {
            LUNA_LOG_WARN("VulkanGPUDriven: failed to create compute semaphore [%u]", i);
            return false;
        }

        // Fence (initially signaled)
        VkFenceCreateInfo fenceCI{};
        fenceCI.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceCI.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (vkCreateFence(dev, &fenceCI, nullptr, &cf.fence) != VK_SUCCESS)
        {
            LUNA_LOG_WARN("VulkanGPUDriven: failed to create compute fence [%u]", i);
            return false;
        }
    }

    _asyncComputeReady = true;
    LUNA_LOG_INFO("VulkanGPUDriven: async compute resources created");
    return true;
}

void VulkanGPUDriven::DestroyAsyncComputeResources()
{
    if (!_core) return;
    VkDevice dev = _core->GetDevice();
    if (dev == VK_NULL_HANDLE) return;

    _asyncComputeReady = false;

    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++)
    {
        auto& cf = _computeFrames[i];
        if (cf.fence)         { vkDestroyFence(dev, cf.fence, nullptr);             cf.fence         = VK_NULL_HANDLE; }
        if (cf.doneSemaphore) { vkDestroySemaphore(dev, cf.doneSemaphore, nullptr); cf.doneSemaphore = VK_NULL_HANDLE; }
        if (cf.cmdPool)       { vkDestroyCommandPool(dev, cf.cmdPool, nullptr);     cf.cmdPool       = VK_NULL_HANDLE; }
        cf.cmdBuffer = VK_NULL_HANDLE;
    }
}

// ---------------------------------------------------------------------------
// Instance Recording
// ---------------------------------------------------------------------------

void VulkanGPUDriven::RecordInstance(const GPUObjectData& data)
{
    if (_cpuInstances.size() < MAX_GPU_OBJECTS)
        _cpuInstances.push_back(data);
}

void VulkanGPUDriven::ClearInstances()
{
    _cpuInstances.clear();
}

// ---------------------------------------------------------------------------
// ViewProj UBO
// ---------------------------------------------------------------------------

void VulkanGPUDriven::UpdateViewProj(const XMFLOAT4X4& view, const XMFLOAT4X4& proj)
{
    if (!_viewProjMapped) return;
    struct VP { float view[16]; float proj[16]; } vp{};
    memcpy(vp.view, &view, 64);
    memcpy(vp.proj, &proj, 64);
    memcpy(_viewProjMapped, &vp, 128);
}

// ---------------------------------------------------------------------------
// Frustum Plane Extraction (Gribb-Hartmann)
// ---------------------------------------------------------------------------

void VulkanGPUDriven::BuildFrustumPlanes(const XMMATRIX& VP, CullConstants& out)
{
    XMMATRIX T = XMMatrixTranspose(VP);
    XMFLOAT4X4 tm;
    XMStoreFloat4x4(&tm, T);

    // Left
    out.frustumPlanes[0][0] = tm.m[3][0] + tm.m[0][0];
    out.frustumPlanes[0][1] = tm.m[3][1] + tm.m[0][1];
    out.frustumPlanes[0][2] = tm.m[3][2] + tm.m[0][2];
    out.frustumPlanes[0][3] = tm.m[3][3] + tm.m[0][3];
    // Right
    out.frustumPlanes[1][0] = tm.m[3][0] - tm.m[0][0];
    out.frustumPlanes[1][1] = tm.m[3][1] - tm.m[0][1];
    out.frustumPlanes[1][2] = tm.m[3][2] - tm.m[0][2];
    out.frustumPlanes[1][3] = tm.m[3][3] - tm.m[0][3];
    // Bottom
    out.frustumPlanes[2][0] = tm.m[3][0] + tm.m[1][0];
    out.frustumPlanes[2][1] = tm.m[3][1] + tm.m[1][1];
    out.frustumPlanes[2][2] = tm.m[3][2] + tm.m[1][2];
    out.frustumPlanes[2][3] = tm.m[3][3] + tm.m[1][3];
    // Top
    out.frustumPlanes[3][0] = tm.m[3][0] - tm.m[1][0];
    out.frustumPlanes[3][1] = tm.m[3][1] - tm.m[1][1];
    out.frustumPlanes[3][2] = tm.m[3][2] - tm.m[1][2];
    out.frustumPlanes[3][3] = tm.m[3][3] - tm.m[1][3];
    // Near
    out.frustumPlanes[4][0] = tm.m[2][0];
    out.frustumPlanes[4][1] = tm.m[2][1];
    out.frustumPlanes[4][2] = tm.m[2][2];
    out.frustumPlanes[4][3] = tm.m[2][3];
    // Far
    out.frustumPlanes[5][0] = tm.m[3][0] - tm.m[2][0];
    out.frustumPlanes[5][1] = tm.m[3][1] - tm.m[2][1];
    out.frustumPlanes[5][2] = tm.m[3][2] - tm.m[2][2];
    out.frustumPlanes[5][3] = tm.m[3][3] - tm.m[2][3];
}

// ---------------------------------------------------------------------------
// Dispatch Cull (Sync)
// ---------------------------------------------------------------------------

void VulkanGPUDriven::DispatchCullSync(
    VkCommandBuffer cmd,
    uint32_t frameIndex,
    const CullConstants& cullConst)
{
    uint32_t count = (uint32_t)_cpuInstances.size();
    if (count == 0) return;

    // Upload object data (HOST_COHERENT — no barrier needed)
    memcpy(_objectDataMapped, _cpuInstances.data(), count * sizeof(GPUObjectData));

    // Clear draw count
    vkCmdFillBuffer(cmd, _drawCountBuffer[frameIndex], 0, sizeof(uint32_t), 0);

    // Barrier: TRANSFER_WRITE → COMPUTE_SHADER_READ
    {
        VkBufferMemoryBarrier bmb{};
        bmb.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        bmb.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        bmb.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        bmb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bmb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bmb.buffer              = _drawCountBuffer[frameIndex];
        bmb.offset              = 0;
        bmb.size                = sizeof(uint32_t);
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 1, &bmb, 0, nullptr);
    }

    // Bind + dispatch
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _cullPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _cullPipeLayout,
        0, 1, &_cullDescSet[frameIndex], 0, nullptr);
    vkCmdPushConstants(cmd, _cullPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 128, &cullConst);
    vkCmdDispatch(cmd, (count + 63) / 64, 1, 1);

    // Barrier: COMPUTE SHADER_WRITE → DRAW_INDIRECT
    {
        VkBufferMemoryBarrier bmbs[2]{};
        bmbs[0].sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        bmbs[0].srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
        bmbs[0].dstAccessMask       = VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
        bmbs[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bmbs[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bmbs[0].buffer              = _indirectArgBuffer[frameIndex];
        bmbs[0].offset              = 0;
        bmbs[0].size                = VK_WHOLE_SIZE;
        bmbs[1] = bmbs[0];
        bmbs[1].buffer = _drawCountBuffer[frameIndex];
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
            0, 0, nullptr, 2, bmbs, 0, nullptr);
    }
}

// ---------------------------------------------------------------------------
// Dispatch Cull (Async)
// ---------------------------------------------------------------------------

void VulkanGPUDriven::DispatchCullAsync(
    uint32_t frameIndex,
    const CullConstants& cullConst)
{
    if (!_asyncComputeReady) return;

    uint32_t count = (uint32_t)_cpuInstances.size();
    if (count == 0) return;

    VulkanDevice* vkDev = _core->GetVulkanDevice();
    VkDevice dev = _core->GetDevice();
    auto& cf = _computeFrames[frameIndex];

    uint32_t computeFamily  = vkDev->GetComputeQueueFamily();
    uint32_t graphicsFamily = vkDev->GetGraphicsQueueFamily();

    // Wait for previous compute on this frame slot
    vkWaitForFences(dev, 1, &cf.fence, VK_TRUE, UINT64_MAX);
    vkResetFences(dev, 1, &cf.fence);

    // Reset + begin command buffer
    vkResetCommandBuffer(cf.cmdBuffer, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cf.cmdBuffer, &bi);

    VkCommandBuffer cmd = cf.cmdBuffer;

    // Upload object data
    memcpy(_objectDataMapped, _cpuInstances.data(), count * sizeof(GPUObjectData));

    // Clear draw count
    vkCmdFillBuffer(cmd, _drawCountBuffer[frameIndex], 0, sizeof(uint32_t), 0);

    // Barrier
    {
        VkBufferMemoryBarrier bmb{};
        bmb.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        bmb.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        bmb.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        bmb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bmb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bmb.buffer              = _drawCountBuffer[frameIndex];
        bmb.offset              = 0;
        bmb.size                = sizeof(uint32_t);
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 1, &bmb, 0, nullptr);
    }

    // Dispatch cull
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _cullPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _cullPipeLayout,
        0, 1, &_cullDescSet[frameIndex], 0, nullptr);
    vkCmdPushConstants(cmd, _cullPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 128, &cullConst);
    vkCmdDispatch(cmd, (count + 63) / 64, 1, 1);

    // Queue ownership release barriers
    {
        uint32_t srcFamily = (computeFamily != graphicsFamily) ? computeFamily  : VK_QUEUE_FAMILY_IGNORED;
        uint32_t dstFamily = (computeFamily != graphicsFamily) ? graphicsFamily : VK_QUEUE_FAMILY_IGNORED;

        VkBufferMemoryBarrier bmbs[2]{};
        bmbs[0].sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        bmbs[0].srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
        bmbs[0].dstAccessMask       = 0;
        bmbs[0].srcQueueFamilyIndex = srcFamily;
        bmbs[0].dstQueueFamilyIndex = dstFamily;
        bmbs[0].buffer              = _indirectArgBuffer[frameIndex];
        bmbs[0].offset              = 0;
        bmbs[0].size                = VK_WHOLE_SIZE;
        bmbs[1] = bmbs[0];
        bmbs[1].buffer = _drawCountBuffer[frameIndex];
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0, 0, nullptr, 2, bmbs, 0, nullptr);
    }

    vkEndCommandBuffer(cmd);

    // Submit
    VkSubmitInfo si{};
    si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount   = 1;
    si.pCommandBuffers      = &cf.cmdBuffer;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores    = &cf.doneSemaphore;
    vkQueueSubmit(vkDev->GetComputeQueue(), 1, &si, cf.fence);
}

void VulkanGPUDriven::WaitForComputeFence(uint32_t frameIndex)
{
    if (!_asyncComputeReady) return;
    VkDevice dev = _core->GetDevice();
    vkWaitForFences(dev, 1, &_computeFrames[frameIndex].fence, VK_TRUE, UINT64_MAX);
}

void VulkanGPUDriven::RecordAcquireBarriers(VkCommandBuffer cmd, uint32_t frameIndex)
{
    if (!_asyncComputeReady) return;

    VulkanDevice* vkDev = _core->GetVulkanDevice();
    uint32_t computeFamily  = vkDev->GetComputeQueueFamily();
    uint32_t graphicsFamily = vkDev->GetGraphicsQueueFamily();
    uint32_t srcFamily = (computeFamily != graphicsFamily) ? computeFamily  : VK_QUEUE_FAMILY_IGNORED;
    uint32_t dstFamily = (computeFamily != graphicsFamily) ? graphicsFamily : VK_QUEUE_FAMILY_IGNORED;

    VkBufferMemoryBarrier bmbs[2]{};
    bmbs[0].sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    bmbs[0].srcAccessMask       = 0;
    bmbs[0].dstAccessMask       = VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
    bmbs[0].srcQueueFamilyIndex = srcFamily;
    bmbs[0].dstQueueFamilyIndex = dstFamily;
    bmbs[0].buffer              = _indirectArgBuffer[frameIndex];
    bmbs[0].offset              = 0;
    bmbs[0].size                = VK_WHOLE_SIZE;
    bmbs[1] = bmbs[0];
    bmbs[1].buffer = _drawCountBuffer[frameIndex];
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
        VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
        0, 0, nullptr, 2, bmbs, 0, nullptr);
}

// ---------------------------------------------------------------------------
// Indirect Draw
// ---------------------------------------------------------------------------

void VulkanGPUDriven::DrawIndirect(VkCommandBuffer cmd, uint32_t frameIndex)
{
    if (!_ready) return;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _indirectGBufPipeline);

    VkDescriptorSet dSets[] = { _vsDescSet, _materialDescSet };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _indirectPipeLayout,
        0, 2, dSets, 0, nullptr);

    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &_mergedVB, &off);
    vkCmdBindIndexBuffer(cmd, _mergedIB, 0, VK_INDEX_TYPE_UINT32);

    vkCmdDrawIndexedIndirectCount(cmd,
        _indirectArgBuffer[frameIndex], 0,
        _drawCountBuffer[frameIndex], 0,
        MAX_GPU_OBJECTS,
        sizeof(VkDrawIndexedIndirectCmd));
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

VkSemaphore VulkanGPUDriven::GetComputeDoneSemaphore(uint32_t frameIndex) const
{
    if (!_asyncComputeReady || frameIndex >= FRAMES_IN_FLIGHT) return VK_NULL_HANDLE;
    return _computeFrames[frameIndex].doneSemaphore;
}

} // namespace Luna

