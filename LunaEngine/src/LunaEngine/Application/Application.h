#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "LunaEngine/Layer.h"
#include "Renderer/HAL/Public/IRenderContext.h"
#include "Renderer/Camera.h"
#include "LunaEngine/Application/CustomTitleBar.h"
#include "LunaEngine/Profiler/GPUProfiler.h"

struct GLFWwindow;
namespace Luna
{
class CameraComponent;

struct ApplicationSpecification
{
    std::string name    = "LunaEngine";
    uint32_t    width   = 1600;
    uint32_t    height  = 900;

    std::filesystem::path iconPath;
    bool windowResizeable = true;
    bool customTitleBar   = true;   // Enable custom title bar by default
    bool useDockSpace     = true;
    bool centerWindow     = true;
    RenderBackendType backend = RenderBackendType::Vulkan;
    bool vsync = false;
};

class Application
{
public:
    Application(ApplicationSpecification applicationSpecification = ApplicationSpecification());
    ~Application();

    static Application &Get();
    void Run();
    void Close();
    float GetTime() const;
    void SetMenubarCallback(const std::function<void()> &menubarCallback);

    // ── CPU-side frame breakdown ────────────────────────────────────────────
    // GPU timestamps alone cannot tell you which side of the pipeline is the
    // limiter. The decisive signal is `present`: a large present time means the
    // CPU is blocked waiting on the GPU (GPU-bound), while present ~0 with
    // ui+submit filling the frame means the CPU is the limiter.
    struct CPUFrameStats
    {
        float totalMs   = 0.0f;  // wall clock, frame start to frame start
        float updateMs  = 0.0f;  // input poll + camera/MVP update
        float uiMs      = 0.0f;  // ImGui layer build (CPU only)
        float submitMs  = 0.0f;  // FlushDraws + CompositeFrame + sensor passes
        float presentMs = 0.0f;  // EndFrame: submit + present + swapchain wait
    };
    const CPUFrameStats& GetCPUFrameStats() const { return _cpuStatsAvg; }

    template<typename T>
    void PushLayer();
    void PushLayer(const std::shared_ptr<Layer> &layer);

    GLFWwindow *GetWindowHandle() const { return _windowHandle; }
    void* GetNativeWindow() const;

    Camera& GetCamera() { return _camera; }
    // Get the scene's CameraComponent (preferred over legacy _camera)
    std::shared_ptr<CameraComponent> GetSceneCamera() const;
    const ApplicationSpecification& GetSpec() const { return _specification; }

private:
    void Init();
    void Shutdown();
    bool ShouldContinueRunning() const;

    // GLFW callbacks
    static void OnFramebufferResize(GLFWwindow* w, int width, int height);
    static void OnMouseMove(GLFWwindow* w, double x, double y);
    static void OnMouseButton(GLFWwindow* w, int button, int action, int mods);
    static void OnScroll(GLFWwindow* w, double xOffset, double yOffset);

    float _timeStep      = 0.0f;
    float _frameTime     = 0.0f;
    float _lastFrameTime = 0.0f;

    CPUFrameStats _cpuStats;      // this frame, raw
    CPUFrameStats _cpuStatsAvg;   // exponential moving average — raw values are too noisy to read

    bool _running          = false;
    bool _titleBarHovered  = false;
    GLFWwindow *_windowHandle = nullptr;
    ApplicationSpecification _specification;
    std::vector<std::shared_ptr<Layer>> _layerStack;
    std::function<void()> _menubarCallBack;

    // Custom title bar
    CustomTitleBar _titleBar;

    // Orbital camera
    Camera _camera;

    // GPU profiler overlay
    GPUProfilerOverlay _profilerOverlay;

    // Mouse drag state
    bool   _mouseDown    = false;
    double _lastMouseX   = 0.0;
    double _lastMouseY   = 0.0;
};

// Implemented by CLIENT
Application *CreateApplication(int argc, char **argv);
} // namespace Luna
