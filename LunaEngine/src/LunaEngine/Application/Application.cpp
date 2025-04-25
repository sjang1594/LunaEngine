#include "LunaPCH.h"
#include "LunaEngine/Application/Application.h"
#include "LunaEngine/Renderer/RenderContext.h"

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

namespace Luna {
Application::Application(
    const ApplicationSpecification& applicationSpecification = {})
    : _specification(applicationSpecification) {
  Init();
}

Application::~Application() { Shutdown(); }

Application& Luna::Application::Get() {
  static Application instance;
  return instance;
}

void Application::Init() {
  if (!glfwInit()) {
    return;
  }

  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  _windowHandle =
      glfwCreateWindow(_specification.width, _specification.height,
                       _specification.Name.c_str(), nullptr, nullptr);

  if (!_windowHandle) {
    std::cerr << "Failed to create GLFW window!" << std::endl;
    glfwTerminate();
    return;
  }

  HWND hwnd = glfwGetWin32Window(_windowHandle);
  RenderContext::Initialize(_specification.backend, hwnd, _specification.width,
                            _specification.height);

  RenderContext::InitImGui(_windowHandle);

  _lastFrameTime = GetTime();
}

void Application::Run() {
  _running = true;
  while (!glfwWindowShouldClose(_windowHandle) && _running) {
    float time = GetTime();
    _frameTime = time - _lastFrameTime;
    _lastFrameTime = time;

    glfwPollEvents();
    RenderContext::BeginFrame();
    RenderContext::StartImGuiFrame();
    for (auto& layer : _layerStack) layer->OnUpdate(_frameTime);
    for (auto& layer : _layerStack) layer->OnUIRender();
    if (_menubarCallBack) _menubarCallBack();
    ImGui::Render();
    RenderContext::RenderImGui();
    RenderContext::DrawFrame();
    RenderContext::EndFrame();
  }

  Shutdown();
}

void Application::Shutdown() {
  for (auto& layer : _layerStack) layer->OnDetach();
  RenderContext::ShutdownImGui();
  RenderContext::Shutdown();
  glfwDestroyWindow(_windowHandle);
  glfwTerminate();
}

void Application::Close() { _running = false; }

void Application::SetMenubarCallback(
    const std::function<void()>& menubarCallback) {
  _menubarCallBack = menubarCallback;
}

void Application::PushLayer(const std::shared_ptr<Layer>& layer) {
  _layerStack.emplace_back(layer);
  layer->OnAttach();
}

float Application::GetTime() const { return static_cast<float>(glfwGetTime()); }
}  // namespace Luna