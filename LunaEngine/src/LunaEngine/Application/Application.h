#pragma once
#include "LunaEngine/Layer.h"
#include "Renderer/HAL/Public/IRenderContext.h"

#include <functional>
#include <memory>

struct GLFWwindow;
namespace Luna
{

struct ApplicationSpecification
{
    // Default Parameters
    std::string name = "LunaEngine";
    uint32_t width = 1600;
    uint32_t height = 900;

    std::filesystem::path iconPath;
    bool windowResizeable = true;
    bool customTitleBar = false;
    bool useDockSpace = true;
    bool centerWindow = true;
    RenderBackendType backend = RenderBackendType::Vulkan;
};

class Application
{
public:
    Application(ApplicationSpecification  applicationSpecification=ApplicationSpecification());
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
  private:
    void Init();
    void Shutdown();
    bool ShouldContinueRunning() const;

    float _timeStep = 0.0f;
    float _frameTime = 0.0f;
    float _lastFrameTime = 0.0f;

    bool _running = false;
    bool _titleBarHovered = false;
    GLFWwindow *_windowHandle = nullptr;
    ApplicationSpecification _specification;
    std::vector<std::shared_ptr<Layer>> _layerStack;
    std::function<void()> _menubarCallBack;
};

// Implemented by CLIENT
Application *CreateApplication(int argc, char **argv);
} // namespace Luna