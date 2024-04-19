
#include <imgui.h>
#include <Layer.h>
#include <Application.cpp>

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
    Luna::ApplicationSpec spec;
    spec.Name = "LunaApp";

    

}