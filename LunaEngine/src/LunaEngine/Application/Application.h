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

struct GLFWwindow;
namespace Luna
{

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

    template<typename T>
    void PushLayer();
    void PushLayer(const std::shared_ptr<Layer> &layer);

    GLFWwindow *GetWindowHandle() const { return _windowHandle; }
    void* GetNativeWindow() const;

    Camera& GetCamera() { return _camera; }

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

    // Mouse drag state
    bool   _mouseDown    = false;
    double _lastMouseX   = 0.0;
    double _lastMouseY   = 0.0;
};

// Implemented by CLIENT
Application *CreateApplication(int argc, char **argv);
} // namespace Luna
