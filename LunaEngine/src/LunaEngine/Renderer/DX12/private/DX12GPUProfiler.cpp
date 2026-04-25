#include "LunaPCH.h"
#include "LunaEngine/Renderer/DX12/Public/DX12GPUProfiler.h"
#include "Logger/Logger.h"
#include <algorithm>
#include <numeric>

namespace Luna
{

DX12GPUProfiler::DX12GPUProfiler()
{
    _passNames.reserve(MAX_PASSES);
    _results.reserve(MAX_PASSES);
}

DX12GPUProfiler::~DX12GPUProfiler()
{
    Shutdown();
}

bool DX12GPUProfiler::Init(ID3D12Device* device, ID3D12CommandQueue* queue)
{
    if (!device || !queue) return false;
    _device = device;
    _queue = queue;

    // Get timestamp frequency
    if (FAILED(_queue->GetTimestampFrequency(&_timestampFrequency)))
    {
        LUNA_LOG_ERROR("[DX12GPUProfiler] Failed to get timestamp frequency");
        return false;
    }

    // Create timestamp query heap
    D3D12_QUERY_HEAP_DESC heapDesc = {};
    heapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    heapDesc.Count = QUERY_COUNT;
    heapDesc.NodeMask = 0;

    if (FAILED(_device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&_queryHeap))))
    {
        LUNA_LOG_ERROR("[DX12GPUProfiler] Failed to create query heap");
        return false;
    }

    // Create readback buffers (one per frame in flight)
    UINT64 bufferSize = QUERY_COUNT * sizeof(UINT64);
    D3D12_HEAP_PROPERTIES readbackHeap = {};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = bufferSize;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    for (uint32_t i = 0; i < FRAME_LATENCY; ++i)
    {
        if (FAILED(_device->CreateCommittedResource(
                &readbackHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&_readbackBuffer[i]))))
        {
            LUNA_LOG_ERROR("[DX12GPUProfiler] Failed to create readback buffer %u", i);
            return false;
        }
    }

    // Create fence for tracking readback availability
    if (FAILED(_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_fence))))
    {
        LUNA_LOG_ERROR("[DX12GPUProfiler] Failed to create fence");
        return false;
    }

    _fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!_fenceEvent)
    {
        LUNA_LOG_ERROR("[DX12GPUProfiler] Failed to create fence event");
        return false;
    }

    _initialized = true;
    LUNA_LOG_INFO("[DX12GPUProfiler] Initialized (freq=%llu Hz)", _timestampFrequency);
    return true;
}

void DX12GPUProfiler::Shutdown()
{
    if (_fenceEvent)
    {
        CloseHandle(_fenceEvent);
        _fenceEvent = nullptr;
    }
    _queryHeap.Reset();
    for (auto& rb : _readbackBuffer) rb.Reset();
    _fence.Reset();
    _initialized = false;
}

void DX12GPUProfiler::BeginFrame()
{
    if (!_initialized || !_enabled) return;

    // Read back results from frame N-2 (double-buffered)
    ReadbackResults();

    _passNames.clear();
    _currentPassIndex = 0;
    _inPass = false;
    _cmd = nullptr;  // will be set when InsertTimestamp is called
}

void DX12GPUProfiler::EndFrame()
{
    // Actual resolve happens in ResolveQueries(), which is called by backend
}

void DX12GPUProfiler::BeginPass(const char* passName)
{
    if (!_initialized || !_enabled) return;
    if (_currentPassIndex >= MAX_PASSES) return;
    if (_inPass) return;  // nested BeginPass not allowed

    _passNames.push_back(passName);
    _inPass = true;
}

void DX12GPUProfiler::EndPass()
{
    if (!_initialized || !_enabled) return;
    if (!_inPass) return;

    _currentPassIndex++;
    _inPass = false;
}

void DX12GPUProfiler::InsertBeginTimestamp(ID3D12GraphicsCommandList* cmd, const char* passName)
{
    if (!_initialized || !_enabled) return;
    if (_currentPassIndex >= MAX_PASSES) return;

    _cmd = cmd;
    
    // Insert begin timestamp
    uint32_t queryIdx = _currentPassIndex * 2;
    cmd->EndQuery(_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryIdx);
    
    // Track pass name
    _passNames.push_back(passName);
    _inPass = true;
}

void DX12GPUProfiler::InsertEndTimestamp(ID3D12GraphicsCommandList* cmd)
{
    if (!_initialized || !_enabled) return;
    if (!_inPass) return;
    
    // Insert end timestamp
    uint32_t queryIdx = _currentPassIndex * 2 + 1;
    cmd->EndQuery(_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryIdx);
    
    _currentPassIndex++;
    _inPass = false;
}

void DX12GPUProfiler::ResolveQueries(ID3D12GraphicsCommandList* cmd)
{
    if (!_initialized || !_enabled) return;
    if (_currentPassIndex == 0) return;

    uint32_t queryCount = _currentPassIndex * 2;
    uint32_t readbackIdx = _frameIndex % FRAME_LATENCY;

    // Resolve all used queries to readback buffer
    cmd->ResolveQueryData(
        _queryHeap.Get(),
        D3D12_QUERY_TYPE_TIMESTAMP,
        0,
        queryCount,
        _readbackBuffer[readbackIdx].Get(),
        0
    );
}

void DX12GPUProfiler::SignalFence(ID3D12CommandQueue* queue)
{
    if (!_initialized || !_enabled) return;
    if (_currentPassIndex == 0) return;

    uint32_t readbackIdx = _frameIndex % FRAME_LATENCY;
    
    // Increment and signal fence value for this frame's queries
    UINT64 fenceVal = _fence->GetCompletedValue() + 1;
    _fenceValues[readbackIdx] = fenceVal;
    queue->Signal(_fence.Get(), fenceVal);

    _frameIndex++;
}

void DX12GPUProfiler::ReadbackResults()
{
    if (!_initialized) return;

    // Read from the oldest frame slot (N-FRAME_LATENCY)
    uint32_t readbackIdx = _frameIndex % FRAME_LATENCY;
    UINT64 completedValue = _fence->GetCompletedValue();

    // Skip if data not ready yet (first few frames)
    if (_fenceValues[readbackIdx] == 0 || completedValue < _fenceValues[readbackIdx])
    {
        // No data ready yet
        return;
    }

    // Map readback buffer
    void* data = nullptr;
    D3D12_RANGE readRange = { 0, QUERY_COUNT * sizeof(UINT64) };
    if (FAILED(_readbackBuffer[readbackIdx]->Map(0, &readRange, &data)))
        return;

    UINT64* timestamps = static_cast<UINT64*>(data);
    uint32_t passCount = static_cast<uint32_t>(_passNames.size());

    _results.clear();
    _totalMs = 0.0f;

    for (uint32_t i = 0; i < passCount; ++i)
    {
        UINT64 begin = timestamps[i * 2 + 0];
        UINT64 end   = timestamps[i * 2 + 1];

        float ms = 0.0f;
        if (end >= begin && _timestampFrequency > 0)
        {
            double seconds = static_cast<double>(end - begin) / static_cast<double>(_timestampFrequency);
            ms = static_cast<float>(seconds * 1000.0);
        }

        // Update rolling average
        auto& history = _avgHistory[_passNames[i]];
        history.push_back(ms);
        if (history.size() > HISTORY_SIZE)
            history.pop_front();

        float avg = 0.0f;
        if (!history.empty())
        {
            avg = std::accumulate(history.begin(), history.end(), 0.0f) / static_cast<float>(history.size());
        }

        _results.push_back({ _passNames[i], ms, avg });
        _totalMs += avg;
    }

    D3D12_RANGE writeRange = { 0, 0 };  // we didn't write
    _readbackBuffer[readbackIdx]->Unmap(0, &writeRange);
}

} // namespace Luna



