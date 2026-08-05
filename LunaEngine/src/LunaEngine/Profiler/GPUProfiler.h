#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

namespace Luna
{

// Per-pass timing result (in milliseconds)
struct GPUTimingResult
{
    std::string name;
    float       gpuTimeMs    = 0.0f;
    float       avgGpuTimeMs = 0.0f;  // rolling average over N frames
};

// Abstract interface for GPU profiling — backends implement platform-specific queries
class IGPUProfiler
{
public:
    virtual ~IGPUProfiler() = default;

    // Called once at the start of each frame before any passes
    virtual void BeginFrame() = 0;

    // Called at the end of each frame after all passes complete (to resolve queries)
    virtual void EndFrame() = 0;

    // Mark the beginning of a named pass
    virtual void BeginPass(const char* passName) = 0;

    // Mark the end of the current pass
    virtual void EndPass() = 0;

    // Get timing results from the previous frame
    virtual const std::vector<GPUTimingResult>& GetResults() const = 0;

    // Get total GPU frame time (sum of all passes)
    virtual float GetTotalGpuTimeMs() const = 0;

    // Is profiling enabled?
    virtual bool IsEnabled() const = 0;
    virtual void SetEnabled(bool enabled) = 0;
};

// ---------------------------------------------------------------------------
// GPUProfilerOverlay — ImGui window displaying GPU timing results
// ---------------------------------------------------------------------------
class GPUProfilerOverlay
{
public:
    void Render(IGPUProfiler* profiler);        // standalone window (Begin/End included)
    void RenderContent(IGPUProfiler* profiler); // content only — for embedding in a tab

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

