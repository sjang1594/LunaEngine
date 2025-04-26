#include <imgui.h>
#include <LunaEngine/Application/Application.h>
namespace Luna {

class ExampleLayer : public Luna::Layer {
 public:
  virtual void OnUIRender() override {
  }
};

Luna::Application* Luna::CreateApplication(int argc, char** argv) {
  Luna::ApplicationSpecification spec;
  spec.Name = "LunaApp";
  spec.width = 1600;
  spec.height = 900;
  spec.backend = RenderBackendType::DirectX12;

  Luna::Application* app = new Luna::Application(spec);
  app->PushLayer(std::make_shared<ExampleLayer>());
  app->SetMenubarCallback([app]() {
    if (ImGui::BeginMenu("File")) {
      if (ImGui::MenuItem("Exit")) {
        app->Close();
      }
      ImGui::EndMenu();
    }
  });
  return app;
}
}  // namespace Luna