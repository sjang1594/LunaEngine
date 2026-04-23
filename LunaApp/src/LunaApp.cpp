#include <LunaEngine/Application/Application.h>
#include <imgui.h>

namespace Luna
{

class ExampleLayer : public Layer
{
  public:
    virtual void OnAttach() override
    {
    }

    virtual void OnUpdate(float /*dt*/) override
    {
        // Camera is updated automatically in Application::Run() each frame.
        // Place per-frame game logic here.
    }

    virtual void OnUIRender() override
    {
        // Camera debug overlay
        ImGui::Begin("Camera");
        Luna::Application& app = Luna::Application::Get();
        Luna::Camera& cam = app.GetCamera();
        ImGui::Text("Left-drag: orbit | Scroll: zoom");
        XMFLOAT3 eye = cam.GetEyePosition();
        ImGui::Text("Eye: (%.2f, %.2f, %.2f)", eye.x, eye.y, eye.z);
        ImGui::End();
    }
};

Application *Luna::CreateApplication(int argc, char **argv)
{
    Luna::ApplicationSpecification spec;
    spec.name    = "LunaApp";
    spec.width   = 1600;
    spec.height  = 900;
    spec.backend = RenderBackendType::DirectX12;  // Default backend
    spec.iconPath = "Resources/icon.png";

    // Parse command line arguments for backend selection
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if ((arg == "--backend" || arg == "-b") && i + 1 < argc)
        {
            std::string backendStr = argv[++i];
            if (backendStr == "vulkan" || backendStr == "vk")
                spec.backend = RenderBackendType::Vulkan;
            else if (backendStr == "dx12" || backendStr == "directx12")
                spec.backend = RenderBackendType::DirectX12;
        }
        else if (arg == "--vulkan" || arg == "-vk")
        {
            spec.backend = RenderBackendType::Vulkan;
        }
        else if (arg == "--dx12")
        {
            spec.backend = RenderBackendType::DirectX12;
        }
    }

    Application *app = new Application(spec);
    app->PushLayer(std::make_shared<ExampleLayer>());
    app->SetMenubarCallback([app]() {
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("Exit"))
                app->Close();
            ImGui::EndMenu();
        }
    });
    return app;
}
} // namespace Luna
