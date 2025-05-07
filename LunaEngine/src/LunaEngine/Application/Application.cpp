#include "LunaPCH.h"
#include "LunaEngine/Application/Application.h"
#include "LunaEngine/Renderer/IRenderContext.h"

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

namespace Luna
{
Application::Application(const ApplicationSpecification &applicationSpecification = {})
    : _specification(applicationSpecification)
{
    Init();
}

Application::~Application()
{
    Shutdown();
}

Application &Luna::Application::Get()
{
    static Application instance;
    return instance;
}

void Application::Init()
{
    if (!glfwInit())
    {
        return;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    _windowHandle = glfwCreateWindow(_specification.width, _specification.height,
                                     _specification.Name.c_str(), nullptr, nullptr);

    if (!_windowHandle)
    {
        std::cerr << "Failed to create GLFW window!" << std::endl;
        glfwTerminate();
        return;
    }

    HWND hwnd = glfwGetWin32Window(_windowHandle);
    IRenderContext::Initialize(_specification.backend, hwnd, _specification.width,
                              _specification.height);

    IRenderContext::InitImGui(_windowHandle);
    _lastFrameTime = GetTime();
}

void Application::Run()
{
    _running = true;
    while (ShouldContiueRunning())
    {
        glfwPollEvents();
        float time = GetTime();
        _frameTime = time - _lastFrameTime;
        _lastFrameTime = time;

        IRenderContext::BeginFrame();
        IRenderContext::StartImGuiFrame();

        if (ImGui::BeginMainMenuBar())
        {
            if (_menubarCallBack)
                _menubarCallBack();
            ImGui::EndMainMenuBar();
        }
        
        ImGui::PushStyleColor(ImGuiCol_DockingEmptyBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::DockSpaceOverViewport(ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);
        ImGui::PopStyleColor();

        for (auto &layer : _layerStack)
            layer->OnUpdate(_frameTime);
        for (auto &layer : _layerStack)
            layer->OnUIRender();
        ImGui::ShowDemoWindow();
        IRenderContext::DrawFrame();
        IRenderContext::RenderImGui();
        
        IRenderContext::EndFrame();
    }

    Shutdown();
}

void Application::Shutdown()
{
    for (auto &layer : _layerStack)
        layer->OnDetach();
    IRenderContext::ShutdownImGui();
    IRenderContext::Shutdown();
    glfwDestroyWindow(_windowHandle);
    glfwTerminate();
}

bool Application::ShouldContiueRunning() const
{
    return _running && !glfwWindowShouldClose(_windowHandle);
}

void Application::Close()
{
    std::cout << "[Application] Close() called" << std::endl;
    _running = false;
    glfwSetWindowShouldClose(_windowHandle, GLFW_TRUE);
}

void Application::SetMenubarCallback(const std::function<void()> &menubarCallback)
{
    _menubarCallBack = menubarCallback;
}

void Application::PushLayer(const std::shared_ptr<Layer> &layer)
{
    _layerStack.emplace_back(layer);
    layer->OnAttach();
}

float Application::GetTime() const
{
    return static_cast<float>(glfwGetTime());
}
} // namespace Luna