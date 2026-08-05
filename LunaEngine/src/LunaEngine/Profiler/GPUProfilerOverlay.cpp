#include "LunaPCH.h"
#include "GPUProfiler.h"
#include "imgui.h"
#include <algorithm>

namespace Luna
{

void GPUProfilerOverlay::RenderContent(IGPUProfiler* profiler)
{
    if (!profiler) return;

    bool enabled = profiler->IsEnabled();
    if (ImGui::Checkbox("Enable Profiling", &enabled))
        profiler->SetEnabled(enabled);

    ImGui::SameLine();
    ImGui::Checkbox("Show %", &_showPercentage);
    ImGui::SameLine();
    if (ImGui::Button(_sortMode == 0 ? "Sort: Order" : "Sort: Time"))
        _sortMode = (_sortMode + 1) % 2;

    ImGui::Separator();

    if (!enabled) { ImGui::TextDisabled("Profiling disabled"); return; }

    const auto& results = profiler->GetResults();
    float totalMs = profiler->GetTotalGpuTimeMs();

    ImGui::Text("Total GPU: %.3f ms (%.1f FPS)", totalMs, totalMs > 0.0001f ? 1000.0f / totalMs : 0.0f);
    ImGui::Separator();

    if (results.empty()) { ImGui::TextDisabled("No timing data"); return; }

    std::vector<size_t> order(results.size());
    for (size_t i = 0; i < results.size(); ++i) order[i] = i;
    if (_sortMode == 1)
    {
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            return results[a].avgGpuTimeMs > results[b].avgGpuTimeMs;
        });
    }

    static const ImVec4 kColors[] = {
        ImVec4(0.4f, 0.7f, 1.0f, 1.0f), ImVec4(0.4f, 0.9f, 0.5f, 1.0f),
        ImVec4(1.0f, 0.7f, 0.3f, 1.0f), ImVec4(0.9f, 0.4f, 0.4f, 1.0f),
        ImVec4(0.8f, 0.5f, 0.9f, 1.0f), ImVec4(0.9f, 0.9f, 0.4f, 1.0f),
        ImVec4(0.5f, 0.9f, 0.9f, 1.0f), ImVec4(1.0f, 0.6f, 0.8f, 1.0f),
    };
    constexpr size_t kColorCount = sizeof(kColors) / sizeof(kColors[0]);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 cursorStart = ImGui::GetCursorScreenPos();
    float barHeight = 24.0f;
    float barWidth  = ImGui::GetContentRegionAvail().x;
    float x = cursorStart.x;

    for (size_t p = 0; p < order.size(); ++p)
    {
        size_t i = order[p];
        const auto& r = results[i];
        float frac = (totalMs > 0.0001f) ? (r.avgGpuTimeMs / totalMs) : 0.0f;
        float segW = std::max(frac * barWidth, 1.0f);
        drawList->AddRectFilled(ImVec2(x, cursorStart.y),
                                ImVec2(x + segW, cursorStart.y + barHeight),
                                ImGui::ColorConvertFloat4ToU32(kColors[i % kColorCount]));
        x += segW;
    }
    drawList->AddRect(cursorStart, ImVec2(cursorStart.x + barWidth, cursorStart.y + barHeight),
                      ImGui::ColorConvertFloat4ToU32(ImVec4(0.3f, 0.3f, 0.3f, 1.0f)));
    ImGui::Dummy(ImVec2(barWidth, barHeight + 4.0f));

    if (ImGui::BeginTable("##GPUTimings", _showPercentage ? 4 : 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV))
    {
        ImGui::TableSetupColumn("Pass",     ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Cur (ms)", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Avg (ms)", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        if (_showPercentage)
            ImGui::TableSetupColumn("%", ImGuiTableColumnFlags_WidthFixed, 50.0f);
        ImGui::TableHeadersRow();

        for (size_t idx : order)
        {
            const auto& r = results[idx];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushStyleColor(ImGuiCol_Text, kColors[idx % kColorCount]);
            ImGui::TextUnformatted(r.name.c_str());
            ImGui::PopStyleColor();
            ImGui::TableSetColumnIndex(1); ImGui::Text("%.3f", r.gpuTimeMs);
            ImGui::TableSetColumnIndex(2); ImGui::Text("%.3f", r.avgGpuTimeMs);
            if (_showPercentage)
            {
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%.1f", totalMs > 0.0001f ? r.avgGpuTimeMs / totalMs * 100.0f : 0.0f);
            }
        }
        ImGui::EndTable();
    }
}

void GPUProfilerOverlay::Render(IGPUProfiler* profiler)
{
    if (!_visible || !profiler) return;

    ImGui::SetNextWindowSize(ImVec2(400, 300), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("GPU Profiler", &_visible))
    {
        ImGui::End();
        return;
    }
    RenderContent(profiler);
    ImGui::End();
}

} // namespace Luna

