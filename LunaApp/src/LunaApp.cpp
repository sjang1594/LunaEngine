
#include <imgui.h>
#include <LunaEngine/Layer.h>
#include <LunaEngine/Application.h>

class ExampleLayer : public Luna::Layer
{
public:
    virtual void OnUIRender() override
    {
        ImGui::Begin("Hello");
        ImGui::Button("Button");
        ImGui::End();

        ImGui::ShowDemoWindow();
    }
};

Luna::Application* Luna::CreateApplication(int argc, char** argv)
{
    Luna::ApplicationSpecification spec;
    spec.Name = "LunaApp";
    
    Luna::Application* app = new Luna::Application(spec);
    app->PushLayer<ExampleLayer>();
    app->SetMenubarCallback([app]()
        {
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