#pragma once
#include "LunaEngine/Application/ApplicationSpecification.h"
#include "LunaEngine/Layer.h"
#include <functional>
#include <memory>

struct GLFWwindow;
namespace Luna
{
class Application
{
  public:
    Application(const ApplicationSpecification &applicationSpecification);
    ~Application();

    static Application &Get();

    void Run();
    void Close();

    void SetMenubarCallback(const std::function<void()> &menubarCallback);
    void PushLayer(const std::shared_ptr<Layer> &layer);
    float GetTime() const;
    GLFWwindow *GetWindowHandle() const
    {
        return _windowHandle;
    }

  private:
    void Init();
    void Shutdown();
    bool ShouldContinueRunning() const;

  private:
    ApplicationSpecification _specification;
    GLFWwindow *_windowHandle = nullptr;
    bool _running = false;

    float _timeStep = 0.0f;
    float _frameTime = 0.0f;
    float _lastFrameTime = 0.0f;

    std::vector<std::shared_ptr<Layer>> _layerStack;
    std::function<void()> _menubarCallBack;
};

// Implemented by CLIENT
Application *CreateApplication(int argc, char **argv);
} // namespace Luna