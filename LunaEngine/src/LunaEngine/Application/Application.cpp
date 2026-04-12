#include "LunaPCH.h"
#include "LunaEngine/Application/Application.h"
#include "Manager/SceneManager.h"
#include "stb_image.h"

#include "Logger/Logger.h"

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

static Luna::Application *g_instance = nullptr;

static void glfwErrorCallback(int error, const char *description)
{
    fprintf(stderr, "Glfw Error %d: %s\n", error, description);
}

namespace Luna
{
Application::Application(ApplicationSpecification applicationSpecification)
    : _specification(std::move(applicationSpecification))
    , _camera(45.0f,
              static_cast<float>(_specification.width) / static_cast<float>(_specification.height),
              0.1f, 500.0f)
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
    return *g_instance;
}

void *Application::GetNativeWindow() const
{
#ifdef _WIN32
    return glfwGetWin32Window(_windowHandle);
#else
    return _windowHandle;
#endif
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

    _windowHandle = glfwCreateWindow(_specification.width, _specification.height,
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
            monitorX + (videoMode->width  - _specification.width)  / 2,
            monitorY + (videoMode->height - _specification.height) / 2);
        glfwSetWindowAttrib(_windowHandle, GLFW_RESIZABLE,
                            _specification.windowResizeable ? GLFW_TRUE : GLFW_FALSE);
    }

    glfwShowWindow(_windowHandle);

    // Register GLFW callbacks
    glfwSetWindowUserPointer(_windowHandle, this);
    glfwSetFramebufferSizeCallback(_windowHandle, OnFramebufferResize);
    glfwSetCursorPosCallback(_windowHandle,  OnMouseMove);
    glfwSetMouseButtonCallback(_windowHandle, OnMouseButton);
    glfwSetScrollCallback(_windowHandle,     OnScroll);

    // Load icon
    GLFWimage icon = {};
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
            LUNA_LOG_ERROR("Failed to load icon: %s", _specification.iconPath.string().c_str());
        }
    }

    // Initialize render backend
    if (_specification.backend == RenderBackendType::DirectX12)
    {
#ifdef _WIN32
        HWND hwnd = glfwGetWin32Window(_windowHandle);
        IRenderContext::Initialize(RenderBackendType::DirectX12, hwnd,
                                   _specification.width, _specification.height);
#else
        LUNA_LOG_ERROR("DirectX 12 is only supported on Windows");
#endif
    }
    else if (_specification.backend == RenderBackendType::Vulkan ||
             _specification.backend == RenderBackendType::VulkanMolt)
    {
#ifdef LUNA_VULKAN_ENABLED
        if (!glfwVulkanSupported())
        {
            LUNA_LOG_ERROR("Vulkan is not supported on this platform");
            glfwTerminate();
            return;
        }
        IRenderContext::Initialize(_specification.backend, _windowHandle,
                                   _specification.width, _specification.height);
#else
        LUNA_LOG_ERROR("Vulkan backend requested but this build was compiled without "
                       "LUNA_VULKAN_ENABLED. Falling back to no renderer.");
#endif
    }

    // P2-04: propagate vsync preference to the backend before any frames are rendered
    IRenderContext::SetVSync(_specification.vsync);

    IRenderContext::InitImGui(_windowHandle);

    // Load the initial test scene (glTF mesh upload happens here)
    SceneManager::GetInstance()->LoadScene(L"Test");

    _lastFrameTime = GetTime();
    LUNA_LOG_INFO("Application initialized!");
}

void Application::Run()
{
    _running = true;
    while (ShouldContinueRunning())
    {
        glfwPollEvents();

        float time   = GetTime();
        _frameTime   = time - _lastFrameTime;
        _lastFrameTime = time;

        // ----------------------------------------------------------------
        // Update MVP from orbital camera — must happen before BeginFrame
        // so the correct CB data is memcpy'd before GPU submission
        // ----------------------------------------------------------------
        {
            XMMATRIX model = XMMatrixIdentity();
            XMMATRIX view  = _camera.GetViewMatrix();
            XMMATRIX proj  = _camera.GetProjectionMatrix();

            XMFLOAT4X4 modelF, viewF, projF;
            XMStoreFloat4x4(&modelF, model);
            XMStoreFloat4x4(&viewF,  view);
            XMStoreFloat4x4(&projF,  proj);

            IRenderContext::UpdateMVP(modelF, viewF, projF);
        }

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

        // Scene update: calls MeshRenderer::Render() per object, which records
        // DrawIndexedInstanced into the open command list via IRenderContext::DrawMesh().
        SceneManager::GetInstance()->Update();

        IRenderContext::RenderImGui();
        IRenderContext::EndFrame();
    }
}

void Application::Shutdown()
{
    if (!_windowHandle) return;

    // Reset scene before backend shutdown: destroys GameObjects → MeshRenderers →
    // releases shared_ptr<Mesh> refs so ~Mesh() fires while D3D12MA allocator is alive.
    SceneManager::GetInstance()->ResetActiveScene();

    for (auto &layer : _layerStack)
        layer->OnDetach();
    _layerStack.clear();

    IRenderContext::ShutdownImGui();
    IRenderContext::Shutdown();

    glfwDestroyWindow(_windowHandle);
    _windowHandle = nullptr;
    glfwTerminate();
}

bool Application::ShouldContinueRunning() const
{
    return _running && !glfwWindowShouldClose(_windowHandle);
}

void Application::Close()
{
    LUNA_LOG_INFO("Application::Close");
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

// ---------------------------------------------------------------------------
// GLFW static callbacks — retrieve the Application* from the user pointer
// ---------------------------------------------------------------------------
void Application::OnFramebufferResize(GLFWwindow* w, int width, int height)
{
    if (width == 0 || height == 0) return;
    auto* app = static_cast<Application*>(glfwGetWindowUserPointer(w));
    IRenderContext::Resize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    app->_camera.SetAspect(static_cast<float>(width) / static_cast<float>(height));
}

void Application::OnMouseButton(GLFWwindow* w, int button, int action, int /*mods*/)
{
    auto* app = static_cast<Application*>(glfwGetWindowUserPointer(w));
    if (button == GLFW_MOUSE_BUTTON_LEFT)
    {
        if (action == GLFW_PRESS)
        {
            app->_mouseDown = true;
            glfwGetCursorPos(w, &app->_lastMouseX, &app->_lastMouseY);
        }
        else if (action == GLFW_RELEASE)
        {
            app->_mouseDown = false;
        }
    }
}

void Application::OnMouseMove(GLFWwindow* w, double x, double y)
{
    auto* app = static_cast<Application*>(glfwGetWindowUserPointer(w));
    if (!app->_mouseDown) return;

    // Sensitivity: 0.3 degrees per pixel
    constexpr float kSensitivity = 0.3f;
    float dYaw   = static_cast<float>(x - app->_lastMouseX) * kSensitivity;
    float dPitch = static_cast<float>(y - app->_lastMouseY) * kSensitivity;

    app->_camera.Orbit(dYaw, dPitch);
    app->_lastMouseX = x;
    app->_lastMouseY = y;
}

void Application::OnScroll(GLFWwindow* w, double /*xOffset*/, double yOffset)
{
    auto* app = static_cast<Application*>(glfwGetWindowUserPointer(w));
    // Positive yOffset = scroll up = zoom in (decrease radius)
    app->_camera.Zoom(static_cast<float>(yOffset) * 0.5f);
}

} // namespace Luna
