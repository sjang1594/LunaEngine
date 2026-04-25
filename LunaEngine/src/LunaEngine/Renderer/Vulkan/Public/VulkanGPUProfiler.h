#pragma once

#include "LunaEngine/Profiler/GPUProfiler.h"
#include <vulkan/vulkan.h>
#include <deque>
#include <unordered_map>

namespace Luna
{

// ---------------------------------------------------------------------------
// VulkanGPUProfiler — uses VkQueryPool (TIMESTAMP) + readback
//
// Query layout:
//   Query [2*i + 0] = begin timestamp for pass i
//   Query [2*i + 1] = end   timestamp for pass i
//
// Double-buffered: results from frame N-1 are read on frame N.
// ---------------------------------------------------------------------------
class VulkanGPUProfiler : public IGPUProfiler
{
public:
    static constexpr uint32_t MAX_PASSES = 64;
    static constexpr uint32_t QUERY_COUNT = MAX_PASSES * 2;
    static constexpr uint32_t FRAME_LATENCY = 3;  // match Vulkan FRAMES_IN_FLIGHT

    VulkanGPUProfiler();
    ~VulkanGPUProfiler() override;

    // Initialize query pool. Must be called once after device creation.
    bool Init(VkDevice device, VkPhysicalDevice physicalDevice, VkQueue queue);
    void Shutdown();

    // Frame lifecycle
    void BeginFrame() override;
    void EndFrame() override;

    // Pass markers — must be called with an active command buffer
    void BeginPass(const char* passName) override;
    void EndPass() override;

    // Call within an active command buffer to insert timestamp queries
    void WriteBeginTimestamp(VkCommandBuffer cmd, const char* passName);
    void WriteEndTimestamp(VkCommandBuffer cmd);

    // Resolve and readback queries (call at end of frame)
    void ReadbackResults();

    // Results from the previous frame
    const std::vector<GPUTimingResult>& GetResults() const override { return _results; }
    float GetTotalGpuTimeMs() const override { return _totalMs; }

    bool IsEnabled() const override { return _enabled; }
    void SetEnabled(bool enabled) override { _enabled = enabled; }

    // Access for external timestamp writes
    VkQueryPool GetQueryPool() const { return _queryPools[_frameIndex % FRAME_LATENCY]; }
    uint32_t GetNextQueryIndex() const { return _currentPassIndex * 2; }

    // Command-buffer query pool reset (call after vkBeginCommandBuffer, before any timestamps)
    void ResetQueryPool(VkCommandBuffer cmd);

    // Access a specific pool slot (for initial one-shot reset)
    VkQueryPool GetQueryPoolAtSlot(uint32_t slot) const { return _queryPools[slot % FRAME_LATENCY]; }

private:
    VkDevice         _device         = VK_NULL_HANDLE;
    VkPhysicalDevice _physicalDevice = VK_NULL_HANDLE;
    VkQueue          _queue          = VK_NULL_HANDLE;
    VkQueryPool      _queryPools[FRAME_LATENCY] = {};

    float            _timestampPeriod = 1.0f;  // nanoseconds per timestamp tick
    uint32_t         _frameIndex      = 0;
    uint32_t         _currentPassIndex = 0;
    bool             _inPass          = false;
    bool             _enabled         = true;
    bool             _initialized     = false;

    // Per-frame pass names (cleared each frame)
    std::vector<std::string> _passNames;

    // Per-frame pass name storage for readback (indexed by slot)
    std::vector<std::string> _passNamesPerFrame[FRAME_LATENCY];

    // Number of passes recorded per frame slot (for readback)
    uint32_t _passCountPerFrame[FRAME_LATENCY] = {};

    // Results from previous frame
    std::vector<GPUTimingResult> _results;
    float _totalMs = 0.0f;

    // Rolling average history (per pass, keyed by name)
    static constexpr int HISTORY_SIZE = 60;
    std::unordered_map<std::string, std::deque<float>> _avgHistory;
};

} // namespace Luna

