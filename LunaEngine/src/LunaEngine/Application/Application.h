#pragma once

#include "LunaPCH.h"
#include "Renderer/IRenderBackend.h"
#include "Layer.h"

enum class RenderAPI { DirectX12, Vulkan, OpenGL };

struct GLFWindow;

namespace Luna {
struct ApplicationSpecification {
  std::string Name = "LunaEngine";
  uint32_t Width = 1600;
  uint32_t Height = 900;
  RenderAPI API = RenderAPI::DirectX12;
};

class Application {
 public:
  Application(const ApplicationSpecification& applicationSpecification = {});
  ~Application();

  static Application& Get();

  void Run();
  void Close();

  void SetMenubarCallback(const std::function<void()>& menubarCallback);
  void PushLayer(const std::shared_ptr<Layer>& layer);

  float GetTime() const;
  GLFWwindow* GetWindowHandle() const { return _windowHandle; } 

 private:
  void Init();
  void Shutdown();

 private:
  ApplicationSpecification _specification;
  GLFWwindow* _windowHandle = nullptr;
  bool m_Running = false;

  float m_TimeStep = 0.0f;
  float m_FrameTime = 0.0f;
  float m_LastFrameTime = 0.0f;

  std::vector<std::shared_ptr<Layer>> _layerStack;
  std::function<void()> menubarCallBack;
  std::unique_ptr<IRenderBackend> _renderBackend;
};

// Implemented by CLIENT
Application* CreateApplication(int argc, char** argv);
}  // namespace Luna