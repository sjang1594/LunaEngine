#pragma once

#include "LunaEngine/Profiler/GPUProfiler.h"
#include <wrl/client.h>
#include <d3d12.h>
#include <array>
#include <deque>

namespace Luna
{
using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
// DX12GPUProfiler — uses ID3D12QueryHeap (TIMESTAMP) + readback buffer
//
// Query layout:
//   Query [2*i + 0] = begin timestamp for pass i
//   Query [2*i + 1] = end   timestamp for pass i
//
// Double-buffered readback: results from frame N-1 are read on frame N.
// ---------------------------------------------------------------------------
class DX12GPUProfiler : public IGPUProfiler
{
public:
    static constexpr uint32_t MAX_PASSES = 64;
    static constexpr uint32_t QUERY_COUNT = MAX_PASSES * 2;  // begin + end per pass
    static constexpr uint32_t FRAME_LATENCY = 2;             // double-buffer readback

    DX12GPUProfiler();
    ~DX12GPUProfiler() override;

    // Initialize query heap and readback buffers. Must be called once after device creation.
    bool Init(ID3D12Device* device, ID3D12CommandQueue* queue);
    void Shutdown();

    // Frame lifecycle
    void BeginFrame() override;
    void EndFrame() override;

    // Pass markers (legacy - don't actually insert timestamps)
    void BeginPass(const char* passName) override;
    void EndPass() override;

    // Pass markers that actually insert timestamp queries
    void InsertBeginTimestamp(ID3D12GraphicsCommandList* cmd, const char* passName);
    void InsertEndTimestamp(ID3D12GraphicsCommandList* cmd);

    // Must be called at the end of the frame to resolve queries
    void ResolveQueries(ID3D12GraphicsCommandList* cmd);

    // Must be called after ExecuteCommandLists to signal readback fence
    void SignalFence(ID3D12CommandQueue* queue);

    // Results from the previous frame
    const std::vector<GPUTimingResult>& GetResults() const override { return _results; }
    float GetTotalGpuTimeMs() const override { return _totalMs; }

    bool IsEnabled() const override { return _enabled; }
    void SetEnabled(bool enabled) override { _enabled = enabled; }

private:
    void ReadbackResults();

    ID3D12Device*              _device = nullptr;
    ID3D12CommandQueue*        _queue  = nullptr;
    ID3D12GraphicsCommandList* _cmd    = nullptr;  // current frame's command list
    ComPtr<ID3D12QueryHeap>    _queryHeap;
    ComPtr<ID3D12Resource>     _readbackBuffer[FRAME_LATENCY];
    ComPtr<ID3D12Fence>        _fence;
    HANDLE                     _fenceEvent = nullptr;
    UINT64                     _fenceValues[FRAME_LATENCY] = {};
    UINT64                     _timestampFrequency = 1;

    uint32_t _frameIndex       = 0;
    uint32_t _currentPassIndex = 0;
    bool     _inPass           = false;
    bool     _enabled          = true;
    bool     _initialized      = false;

    // Per-frame pass names (cleared each frame)
    std::vector<std::string> _passNames;

    // Results from previous frame
    std::vector<GPUTimingResult> _results;
    float _totalMs = 0.0f;

    // Rolling average history (per pass, keyed by name)
    static constexpr int HISTORY_SIZE = 60;
    std::unordered_map<std::string, std::deque<float>> _avgHistory;
};

} // namespace Luna

