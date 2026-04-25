#include "LunaPCH.h"
#include "Renderer/Vulkan/Public/VulkanGPUProfiler.h"
#include "Logger/Logger.h"
#include <algorithm>
#include <numeric>

namespace Luna
{

VulkanGPUProfiler::VulkanGPUProfiler()
{
    _passNames.reserve(MAX_PASSES);
    _results.reserve(MAX_PASSES);
}

VulkanGPUProfiler::~VulkanGPUProfiler()
{
    Shutdown();
}

bool VulkanGPUProfiler::Init(VkDevice device, VkPhysicalDevice physicalDevice, VkQueue queue)
{
    if (device == VK_NULL_HANDLE || physicalDevice == VK_NULL_HANDLE)
        return false;

    _device = device;
    _physicalDevice = physicalDevice;
    _queue = queue;

    // Get timestamp period (nanoseconds per tick)
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(_physicalDevice, &props);
    _timestampPeriod = props.limits.timestampPeriod;

    if (_timestampPeriod == 0.0f)
    {
        LUNA_LOG_WARN("[VulkanGPUProfiler] timestampPeriod is 0, profiling disabled");
        return false;
    }

    // Create query pools (one per frame in flight)
    VkQueryPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    poolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
    poolInfo.queryCount = QUERY_COUNT;

    for (uint32_t i = 0; i < FRAME_LATENCY; ++i)
    {
        if (vkCreateQueryPool(_device, &poolInfo, nullptr, &_queryPools[i]) != VK_SUCCESS)
        {
            LUNA_LOG_ERROR("[VulkanGPUProfiler] Failed to create query pool %u", i);
            Shutdown();
            return false;
        }
    }

    _initialized = true;
    LUNA_LOG_INFO("[VulkanGPUProfiler] Initialized (timestampPeriod=%.3f ns)", _timestampPeriod);
    return true;
}

void VulkanGPUProfiler::Shutdown()
{
    if (_device == VK_NULL_HANDLE) return;

    for (auto& pool : _queryPools)
    {
        if (pool != VK_NULL_HANDLE)
        {
            vkDestroyQueryPool(_device, pool, nullptr);
            pool = VK_NULL_HANDLE;
        }
    }
    _initialized = false;
}

void VulkanGPUProfiler::BeginFrame()
{
    if (!_initialized || !_enabled) return;

    // Read back results from an older frame
    ReadbackResults();

    _passNames.clear();
    _currentPassIndex = 0;
    _inPass = false;
}

void VulkanGPUProfiler::ResetQueryPool(VkCommandBuffer cmd)
{
    if (!_initialized || !_enabled) return;

    uint32_t slot = _frameIndex % FRAME_LATENCY;
    vkCmdResetQueryPool(cmd, _queryPools[slot], 0, QUERY_COUNT);
}

void VulkanGPUProfiler::EndFrame()
{
    if (!_initialized || !_enabled) return;

    // Store pass count and names for this frame slot
    uint32_t slot = _frameIndex % FRAME_LATENCY;
    _passCountPerFrame[slot] = _currentPassIndex;
    _passNamesPerFrame[slot] = _passNames;

    _frameIndex++;
}

void VulkanGPUProfiler::BeginPass(const char* passName)
{
    if (!_initialized || !_enabled) return;
    if (_currentPassIndex >= MAX_PASSES) return;
    if (_inPass) return;

    _passNames.push_back(passName);
    _inPass = true;
}

void VulkanGPUProfiler::EndPass()
{
    if (!_initialized || !_enabled) return;
    if (!_inPass) return;

    _currentPassIndex++;
    _inPass = false;
}

void VulkanGPUProfiler::WriteBeginTimestamp(VkCommandBuffer cmd, const char* passName)
{
    if (!_initialized || !_enabled) return;

    // Auto-close previous pass if it wasn't explicitly ended (e.g. FlushDraws skipped)
    if (_inPass)
    {
        uint32_t slot = _frameIndex % FRAME_LATENCY;
        uint32_t endIdx = _currentPassIndex * 2 + 1;
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, _queryPools[slot], endIdx);
        _currentPassIndex++;
        _inPass = false;
    }

    if (_currentPassIndex >= MAX_PASSES) return;

    uint32_t slot = _frameIndex % FRAME_LATENCY;
    uint32_t queryIdx = _currentPassIndex * 2;

    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, _queryPools[slot], queryIdx);
    _passNames.push_back(passName);
    _inPass = true;
}

void VulkanGPUProfiler::WriteEndTimestamp(VkCommandBuffer cmd)
{
    if (!_initialized || !_enabled) return;
    if (!_inPass) return;

    uint32_t slot = _frameIndex % FRAME_LATENCY;
    uint32_t queryIdx = _currentPassIndex * 2 + 1;

    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, _queryPools[slot], queryIdx);

    _currentPassIndex++;
    _inPass = false;
}

void VulkanGPUProfiler::ReadbackResults()
{
    if (!_initialized) return;

    // Need at least FRAME_LATENCY frames before readback data is valid
    if (_frameIndex < FRAME_LATENCY) return;

    uint32_t slot = _frameIndex % FRAME_LATENCY;
    uint32_t passCount = _passCountPerFrame[slot];
    if (passCount == 0) return;

    uint32_t queryCount = passCount * 2;

    std::vector<uint64_t> timestamps(queryCount);
    VkResult result = vkGetQueryPoolResults(
        _device,
        _queryPools[slot],
        0,
        queryCount,
        queryCount * sizeof(uint64_t),
        timestamps.data(),
        sizeof(uint64_t),
        VK_QUERY_RESULT_64_BIT
    );

    if (result != VK_SUCCESS && result != VK_NOT_READY)
    {
        LUNA_LOG_WARN("[VulkanGPUProfiler] vkGetQueryPoolResults failed: %d", result);
        return;
    }

    if (result == VK_NOT_READY) return;

    // Use stored pass names from the frame that produced this data
    const auto& storedNames = _passNamesPerFrame[slot];

    _results.clear();
    _totalMs = 0.0f;

    for (uint32_t i = 0; i < passCount; ++i)
    {
        uint64_t begin = timestamps[i * 2 + 0];
        uint64_t end   = timestamps[i * 2 + 1];

        float ms = 0.0f;
        if (end >= begin)
        {
            double nanoseconds = static_cast<double>(end - begin) * _timestampPeriod;
            ms = static_cast<float>(nanoseconds / 1e6);
        }

        std::string passName = (i < storedNames.size()) ? storedNames[i] : "Unknown";

        // Update rolling average
        auto& history = _avgHistory[passName];
        history.push_back(ms);
        if (history.size() > HISTORY_SIZE)
            history.pop_front();

        float avg = 0.0f;
        if (!history.empty())
        {
            avg = std::accumulate(history.begin(), history.end(), 0.0f) / static_cast<float>(history.size());
        }

        _results.push_back({ passName, ms, avg });
        _totalMs += avg;
    }
}

} // namespace Luna

