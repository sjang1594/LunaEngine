#include <LunaEngine/Application/Application.h>
#include <imgui.h>

namespace Luna
{

class ExampleLayer : public Layer
{
  public:
    virtual void OnUIRender() override
    {
    }
};

Application *Luna::CreateApplication(int argc, char **argv)
{
    ApplicationSpecification spec;
    spec.Name = "LunaApp";
    spec.width = 1600;
    spec.height = 900;
    spec.backend = RenderBackendType::DirectX12;
    spec.iconPath = "Resources/icon.png";

    Application *app = new Application(spec);
    app->PushLayer(std::make_shared<ExampleLayer>());
    app->SetMenubarCallback([app]() {
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("Exit"))
            {
                app->Close();
            }
            ImGui::EndMenu();
        }
    });
    return app;
}
} // namespace Luna