#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

namespace Luna
{

struct GPUTimingResult
{
    std::string name;
    float       gpuTimeMs    = 0.0f;
    float       avgGpuTimeMs = 0.0f;
};

class IGPUProfiler
{
public:
    virtual ~IGPUProfiler() = default;

    virtual void BeginFrame() = 0;
    virtual void EndFrame() = 0;
    virtual void BeginPass(const char* passName) = 0;
    virtual void EndPass() = 0;
    virtual const std::vector<GPUTimingResult>& GetResults() const = 0;
    virtual float GetTotalGpuTimeMs() const = 0;
    virtual bool IsEnabled() const = 0;
    virtual void SetEnabled(bool enabled) = 0;
};

class GPUProfilerOverlay
{
public:
    void Render(IGPUProfiler* profiler);

    bool IsVisible() const { return _visible; }
    void SetVisible(bool visible) { _visible = visible; }
    void Toggle() { _visible = !_visible; }

private:
    bool  _visible        = true;
    float _barScale       = 1.0f;   // scale factor for bar widths (auto-adjusted)
    int   _sortMode       = 0;      // 0 = declaration order, 1 = by time descending
    bool  _showPercentage = true;
};

} // namespace Luna

