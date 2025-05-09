#include "LunaPCH.h"
#include "LunaEngine/Application/Application.h"
#include "stb_image.h"

#define GLFW_EXPOSE_NATIVE_WIN32
#include "Logger/Logger.h"


#include <GLFW/glfw3native.h>

static Luna::Application *g_instance = nullptr;

static void glfwErrorCallback(int error, const char *description)
{
    fprintf(stderr, "Glfw Error %d: %s\n", error, description);
}

namespace Luna
{
Application::Application(ApplicationSpecification applicationSpecification)
    : _specification(std::move(applicationSpecification))
{
    g_instance = this;
    Init();
}

Application::~Application()
{
    Shutdown();
    g_instance = nullptr;
}

Application &Luna::Application::Get()
{
    static Application instance;
    return instance;
}

void Application::Init()
{
    glfwSetErrorCallback(glfwErrorCallback);
    if (!glfwInit())
    {
        LUNA_LOG_ERROR("Failed to initialize GLFW!");
        return;
    }

    GLFWmonitor* primaryMonitor = glfwGetPrimaryMonitor();
    const GLFWvidmode* videoMode = glfwGetVideoMode(primaryMonitor);
    int monitorX, monitorY;
    glfwGetMonitorPos(primaryMonitor, &monitorX, &monitorY);

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
#ifdef __APPLE__
    glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_TRUE);
#endif
    
    _windowHandle = glfwCreateWindow(_specification.width,_specification.height,
                                 _specification.name.c_str(), nullptr, nullptr);
    if (!_windowHandle)
    {
        LUNA_LOG_ERROR("Failed to create GLFW window!");
        glfwTerminate();
        return;
    }

    if (_specification.centerWindow)
    {
        glfwSetWindowPos(_windowHandle,
            monitorX + (videoMode->width - _specification.width) * 0.5,
            monitorY + (videoMode->height - _specification.height) * 0.5);
        glfwSetWindowAttrib(_windowHandle, GLFW_RESIZABLE, _specification.windowResizeable ? GLFW_TRUE : GLFW_FALSE);
    }

    glfwShowWindow(_windowHandle);

    GLFWimage icon;
    if (!_specification.iconPath.empty())
    {
        int channels;
        std::string iconPathStr = _specification.iconPath.string();
        icon.pixels = stbi_load(iconPathStr.c_str(), &icon.width, &icon.height, &channels, 4);
        if (icon.pixels)
        {
            glfwSetWindowIcon(_windowHandle, 1, &icon);
            stbi_image_free(icon.pixels);
        }
        else
        {
            LUNA_LOG_ERROR("Failed to load icon: " + _specification.iconPath.string());
        }
    }

#ifdef _WIN32
    HWND hwnd = glfwGetWin32Window(_windowHandle);
    if (_specification.backend == RenderBackendType::DX12)
    {
        IRenderContext::Initialize(_specification.backend, hwnd, _specification.width,
                              _specification.height);
        IRenderContext::InitImGui(_windowHandle);
    }
    
#elif __APPLE__ 
    void* nsWindow = glfwGetCocoaWindow(_windowHandle);
    if (_specification.backend == RenderBackendType::Metal)
    {
        LUNA_LOG_INFO("Metal backend is not supported yet!");
    }
#else
    if (_specification.backend == RenderBackendType::Vulkan)
    {
        if (!glfwVulkanSupported())
        {
            LUNA_LOG_ERROR("Vulkan is not supported on this platform!");
            glfwTerminate();
            return;
        }

        VkSurfaceKHR surface;
        VkResult error = glfwCreateWindowSurface(g_instance, _windowHandle, nullptr, &surface);
        if (error != VK_SUCCESS)
        {
            LUNA_LOG_ERROR("Failed to create Vulkan surface!");
            glfwTerminate();
            return;
        }

        // IRENDERBACKEND INitialize
        LUNA_LOG_INFO("Vulkan backend is not supported yet!");
    }
#endif
    _lastFrameTime = GetTime();
    LUNA_LOG_INFO("Application initialized!");
}

void Application::Run()
{
    _running = true;
    while (ShouldContinueRunning())
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

bool Application::ShouldContinueRunning() const
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

template<typename T>
void Application::PushLayer()
{
    static_assert(std::is_base_of<Layer, T>::value, "T must be derived from Layer");
    _layerStack.emplace_back(std::make_shared<T>())->OnAttach();
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