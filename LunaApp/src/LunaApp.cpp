#include <LunaEngine/LunaPCH.h>
#include <LunaEngine/Application/Application.h>
#include <LunaEngine/Manager/SceneManager.h>
#include <LunaEngine/Utils/FileDialog.h>
#include <LunaEngine/UI/ViewGizmo.h>
#include <LunaEngine/UI/TransformGizmo.h>
#include <LunaEngine/UI/Selection.h>
#include <LunaEngine/Renderer/HAL/Public/IRenderContext.h>
#include <LunaEngine/Utils/MathUtils.h>
#include <Scene/Scene.h>
#include <Components/GameObject.h>
#include <Components/Transform.h>
#include <Components/MeshRenderer.h>
#include <LunaEngine/Renderer/Mesh.h>
#include <Components/CameraComponent.h>
#include <Components/LightComponent.h>
#include <LunaEngine/Sensor/SensorComponent.h>
#include <imgui.h>
#include <LunaEngine/Profiler/GPUProfiler.h>
#include "SensorLayer.h"

namespace Luna
{

static bool s_showScene    = true;
static bool s_showGraphics = true;
static bool s_showProfiler = false;
static Luna::GPUProfilerOverlay s_profilerOverlay;

class ExampleLayer : public Layer
{
  public:
    virtual void OnAttach() override
    {
    }

    virtual void OnUpdate(float /*dt*/) override
    {
        // Poll async file dialog result
        std::string dialogPath;
        if (Luna::PollFileDialogResult(dialogPath) && !dialogPath.empty())
            Luna::SceneManager::GetInstance()->LoadSceneFromFile(dialogPath);
    }

    virtual void OnUIRender() override
    {
        Luna::Application& app = Luna::Application::Get();
        auto scene = Luna::SceneManager::GetInstance()->GetActiveScene();
        auto sceneCam = scene ? scene->GetMainCamera() : nullptr;

        // 3D orientation gizmo (top-right corner)
        if (sceneCam)
            Luna::DrawViewGizmo(*sceneCam);
        else
            Luna::DrawViewGizmo(app.GetCamera());

        // ── Transform Gizmo (W/E/R mode switch + 3D handle) ──────────
        static bool gizmoLocalSpace = false;
        {
            auto& sel = Luna::GetSelection();

            if (!ImGui::GetIO().WantTextInput)
            {
                if (ImGui::IsKeyPressed(ImGuiKey_W)) Luna::SetGizmoMode(Luna::GizmoMode::Translate);
                if (ImGui::IsKeyPressed(ImGuiKey_E)) Luna::SetGizmoMode(Luna::GizmoMode::Rotate);
                if (ImGui::IsKeyPressed(ImGuiKey_R)) Luna::SetGizmoMode(Luna::GizmoMode::Scale);
            }

            if (sel.HasSelection())
            {
                auto selTransform = sel.selectedObject->GetTransform();
                if (selTransform)
                {
                    if (sceneCam)
                        Luna::DrawTransformGizmo(*sceneCam, *selTransform, gizmoLocalSpace);
                    else
                        Luna::DrawTransformGizmo(app.GetCamera(), *selTransform, gizmoLocalSpace);
                }
            }
        }

        // ── Scene window ─────────────────────────────────────────────
        if (s_showScene)
        {
        ImGui::Begin("Scene", &s_showScene);
        {
            // Gizmo toolbar (always visible above tabs)
            auto mode = Luna::GetGizmoMode();
            if (ImGui::RadioButton("Translate (W)", mode == Luna::GizmoMode::Translate))
                Luna::SetGizmoMode(Luna::GizmoMode::Translate);
            ImGui::SameLine();
            if (ImGui::RadioButton("Rotate (E)", mode == Luna::GizmoMode::Rotate))
                Luna::SetGizmoMode(Luna::GizmoMode::Rotate);
            ImGui::SameLine();
            if (ImGui::RadioButton("Scale (R)", mode == Luna::GizmoMode::Scale))
                Luna::SetGizmoMode(Luna::GizmoMode::Scale);
            ImGui::SameLine();
            ImGui::Checkbox("Local", &gizmoLocalSpace);
            ImGui::Separator();

            if (ImGui::BeginTabBar("SceneTabs"))
            {
                // ── Inspector tab ─────────────────────────────────────
                if (ImGui::BeginTabItem("Inspector"))
                {
                    // Camera info (formerly standalone "Camera" window)
                    if (sceneCam)
                    {
                        XMFLOAT3 eye = sceneCam->GetEyePosition();
                        ImGui::Text("Eye: (%.2f, %.2f, %.2f)", eye.x, eye.y, eye.z);
                        ImGui::TextDisabled("Left-drag: orbit | Scroll: zoom");
                        ImGui::Separator();
                    }

                    if (scene)
                    {
                        const auto& objects = scene->GetGameObjects();
                        ImGui::Text("Objects: %d", (int)objects.size());
                        ImGui::Separator();

                        auto& sel = Luna::GetSelection();
                        static int prevSelectedIndex = -1;
                        bool selectionChanged = (sel.selectedIndex != prevSelectedIndex);
                        prevSelectedIndex = sel.selectedIndex;

                        for (int idx = 0; idx < (int)objects.size(); ++idx)
                        {
                            const auto& go = objects[idx];
                            if (!go) continue;
                            auto transform = go->GetTransform();
                            if (!transform) continue;

                            auto camComp = std::dynamic_pointer_cast<Luna::CameraComponent>(
                                go->GetComponentByType(Luna::ComponentType::CAMERA));
                            auto lightComp = std::dynamic_pointer_cast<Luna::LightComponent>(
                                go->GetComponentByType(Luna::ComponentType::LIGHT));
                            auto mr = std::dynamic_pointer_cast<Luna::MeshRenderer>(
                                go->GetComponentByType(Luna::ComponentType::MESH_RENDERER));
                            auto sensorComp = std::dynamic_pointer_cast<Luna::SensorComponent>(
                                go->GetComponentByType(Luna::ComponentType::SENSOR));

                            std::string displayName;
                            if (camComp)        displayName = "Main Camera";
                            else if (lightComp) displayName = "Directional Light";
                            else if (mr)
                            {
                                std::string meshName = mr->GetMeshName();
                                displayName = meshName.empty() ? "Mesh " + std::to_string(idx) : meshName;
                            }
                            else displayName = "Object " + std::to_string(idx);

                            const char* icon = camComp ? "[Cam] " : lightComp ? "[Light] " : sensorComp ? "[Sensor] " : "[Mesh] ";
                            char label[128];
                            snprintf(label, sizeof(label), "%s%s##%d", icon, displayName.c_str(), idx);

                            bool isSelected = (sel.selectedIndex == idx);
                            // Light/Camera nodes open by default so properties are immediately visible
                            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow;
                            if (lightComp || camComp) flags |= ImGuiTreeNodeFlags_DefaultOpen;
                            if (isSelected) flags |= ImGuiTreeNodeFlags_Selected;
                            if (isSelected && selectionChanged)
                                ImGui::SetNextItemOpen(true);

                            bool nodeOpen = ImGui::TreeNodeEx(label, flags);
                            if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
                            {
                                sel.selectedIndex = idx;
                                sel.selectedObject = go;
                            }

                            if (nodeOpen)
                            {
                                XMFLOAT3 pos, scl, rot;
                                if (transform->useRawMatrix)
                                {
                                    XMMATRIX world = XMLoadFloat4x4(&transform->rawMatrix);
                                    XMVECTOR sv, rv, tv;
                                    XMMatrixDecompose(&sv, &rv, &tv, world);
                                    XMStoreFloat3(&pos, tv);
                                    XMStoreFloat3(&scl, sv);
                                    XMFLOAT4 q; XMStoreFloat4(&q, rv);
                                    rot = Luna::QuatToEulerDegrees(q);
                                }
                                else
                                {
                                    pos = transform->position;
                                    rot = transform->rotation;
                                    scl = transform->scale;
                                }

                                bool changed = false;
                                changed |= ImGui::DragFloat3("Position", &pos.x, 0.01f);
                                changed |= ImGui::DragFloat3("Rotation", &rot.x, 0.5f);
                                changed |= ImGui::DragFloat3("Scale",    &scl.x, 0.01f);
                                if (changed)
                                {
                                    transform->useRawMatrix = false;
                                    transform->position = pos;
                                    transform->rotation = rot;
                                    transform->scale    = scl;
                                }

                                if (camComp)
                                {
                                    ImGui::Separator();
                                    ImGui::Text("Camera Properties");
                                    float fov = camComp->GetFovDegrees();
                                    if (ImGui::DragFloat("FOV", &fov, 0.5f, 10.0f, 120.0f))
                                        camComp->_fovRad = XMConvertToRadians(fov);
                                    ImGui::DragFloat("Near", &camComp->_nearZ, 0.01f, 0.001f, 10.0f);
                                    ImGui::DragFloat("Far",  &camComp->_farZ, 1.0f, 10.0f, 10000.0f);
                                    ImGui::Text("Orbit Radius: %.2f", camComp->GetRadius());
                                }

                                if (lightComp)
                                {
                                    ImGui::Separator();
                                    ImGui::Text("Light Properties");
                                    XMFLOAT3 dir = lightComp->GetDirection();
                                    if (ImGui::DragFloat3("Direction", &dir.x, 0.01f))
                                        lightComp->SetDirection(dir);
                                    ImGui::ColorEdit3("Color", &lightComp->color.x);
                                    ImGui::DragFloat("Intensity", &lightComp->intensity, 0.1f, 0.0f, 50.0f);
                                }

                                if (mr)
                                {
                                    auto mesh = mr->GetMesh();
                                    if (mesh && mesh->material)
                                    {
                                        ImGui::Separator();
                                        ImGui::Text("Material");
                                        ImGui::SliderFloat("Alpha", &mesh->material->alpha, 0.0f, 1.0f, "%.2f");
                                    }
                                }

                                ImGui::TreePop();
                            }
                        }
                    }
                    else
                    {
                        ImGui::TextDisabled("No active scene");
                    }

                    ImGui::EndTabItem();
                }

                // ── Lights tab ────────────────────────────────────────
                if (ImGui::BeginTabItem("Lights"))
                {
                    static std::vector<Luna::IRenderBackend::PointLightDesc> s_lights;

                    ImGui::Text("Lights: %d / 1024", (int)s_lights.size());

                    if (ImGui::Button("Add Light") && s_lights.size() < 1024)
                    {
                        Luna::IRenderBackend::PointLightDesc l{};
                        if (sceneCam) {
                            XMFLOAT3 eye = sceneCam->GetEyePosition();
                            l.position[0] = eye.x; l.position[1] = eye.y; l.position[2] = eye.z;
                        }
                        l.radius    = 10.0f;
                        l.color[0]  = 1.0f; l.color[1] = 0.9f; l.color[2] = 0.7f;
                        l.intensity = 5.0f;
                        s_lights.push_back(l);
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Clear All"))
                        s_lights.clear();

                    ImGui::Separator();

                    int removeIdx = -1;
                    for (int i = 0; i < (int)s_lights.size(); i++)
                    {
                        ImGui::PushID(i);
                        auto& l = s_lights[i];
                        bool open = ImGui::TreeNodeEx("##light", ImGuiTreeNodeFlags_DefaultOpen, "Light %d", i);
                        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 20);
                        if (ImGui::SmallButton("X")) removeIdx = i;
                        if (open)
                        {
                            ImGui::DragFloat3("Position",  l.position, 0.1f);
                            ImGui::DragFloat("Radius",     &l.radius, 0.5f, 0.1f, 100.0f);
                            ImGui::ColorEdit3("Color",     l.color);
                            ImGui::DragFloat("Intensity",  &l.intensity, 0.1f, 0.0f, 100.0f);
                            ImGui::TreePop();
                        }
                        ImGui::PopID();
                    }
                    if (removeIdx >= 0)
                        s_lights.erase(s_lights.begin() + removeIdx);

                    auto* backend = Luna::IRenderContext::GetBackend();
                    if (backend)
                        backend->SetPointLights(s_lights);

                    ImGui::EndTabItem();
                }

                ImGui::EndTabBar();
            }
        }
        ImGui::End();
        } // if (s_showScene)

        // ── Graphics window ───────────────────────────────────────────
        if (s_showGraphics) {
        
            ImGui::Begin("Graphics", &s_showGraphics);
            {
                if (ImGui::BeginTabBar("GraphicsTabs"))
                {
                    // ── Fog tab 
                    if (ImGui::BeginTabItem("Fog"))
                    {
                        static Luna::IRenderBackend::VolumetricFogParams fogParams = {
                            false,  // enabled
                            0.15f,  // density
                            0.0f,   // heightFalloff
                            0.0f,   // baseHeight
                            1.0f,   // scattering
                            1.0f,   // extinction
                            0.3f    //
                        };

                        ImGui::Checkbox("Enable Fog", &fogParams.enabled);
                        ImGui::Separator();
                        ImGui::BeginDisabled(!fogParams.enabled);
                        ImGui::SliderFloat("Density",        &fogParams.density,       0.000f,  0.5f,  "%.4f");
                        ImGui::SliderFloat("Height Falloff", &fogParams.heightFalloff, 0.001f,  1.0f,  "%.3f");
                        ImGui::SliderFloat("Base Height",    &fogParams.baseHeight,   -100.0f, 50.0f, "%.1f");
                        ImGui::SliderFloat("Scattering",     &fogParams.scattering,    0.0f,    2.0f,  "%.3f");
                        ImGui::SliderFloat("Extinction",     &fogParams.extinction,    0.0f,    2.0f,  "%.3f");
                        ImGui::SliderFloat("Phase G",        &fogParams.phaseG,       -0.99f,   0.99f, "%.2f");
                        ImGui::EndDisabled();

                        auto* backend = Luna::IRenderContext::GetBackend();
                        if (backend)
                            backend->SetVolumetricFogParams(fogParams);

                        ImGui::EndTabItem();
                    }

                    // ── GI tab ────────────────────────────────────────────
                    if (ImGui::BeginTabItem("GI"))
                    {
                        static Luna::IRenderBackend::GIParams giParams;

                        ImGui::DragFloat3("Probe Origin",   giParams.probeGridOrigin,  0.5f, -100.0f, 100.0f, "%.1f");
                        ImGui::DragFloat3("Probe Spacing",  giParams.probeGridSpacing, 0.1f,   0.1f,  20.0f,  "%.2f");
                        ImGui::SliderInt("SSGI Rays",       &giParams.numSSGIRays,     1, 16);
                        ImGui::SliderFloat("Max Ray Dist",  &giParams.maxRayDist,      0.5f, 20.0f, "%.1f");
                        ImGui::SliderFloat("Temporal Alpha",&giParams.temporalAlpha,   0.01f, 1.0f, "%.3f");

                        auto* backend = Luna::IRenderContext::GetBackend();
                        if (backend)
                            backend->SetGIParams(giParams);

                        ImGui::EndTabItem();
                    }

                    ImGui::EndTabBar();
                }
            }
        
            ImGui::End();
        } 

        // ── GPU Profiler window ───────────────────────────────────────
        if (s_showProfiler)
        {
            ImGui::SetNextWindowSize(ImVec2(420, 440), ImGuiCond_FirstUseEver);
            ImGui::Begin("GPU Profiler", &s_showProfiler);
            auto* backend  = Luna::IRenderContext::GetBackend();
            auto* profiler = backend ? backend->GetGPUProfiler() : nullptr;

            // ── CPU frame breakdown + which side is the limiter ──────────
            // GPU timestamps say where GPU time goes but not whether the GPU is even
            // the bottleneck. Present time is the discriminator: it is the CPU
            // blocking on the GPU, so a large present with a small CPU workload means
            // GPU-bound, and the reverse means the CPU is gating the frame.
            const auto& cpu = Luna::Application::Get().GetCPUFrameStats();
            const float cpuWork = cpu.updateMs + cpu.uiMs + cpu.submitMs;
            const float gpuMs   = profiler ? profiler->GetTotalGpuTimeMs() : 0.0f;

            ImGui::SeparatorText("Frame (CPU)");
            ImGui::Text("total    %6.2f ms  (%.0f FPS)", cpu.totalMs,
                        cpu.totalMs > 0.0f ? 1000.0f / cpu.totalMs : 0.0f);
            ImGui::Text("  update %6.2f ms", cpu.updateMs);
            ImGui::Text("  ui     %6.2f ms", cpu.uiMs);
            ImGui::Text("  submit %6.2f ms", cpu.submitMs);
            ImGui::Text("  present%6.2f ms   <- CPU waiting on GPU", cpu.presentMs);
            ImGui::Text("CPU work %6.2f ms  |  GPU %6.2f ms", cpuWork, gpuMs);

            if (cpu.totalMs > 0.01f)
            {
                const char* verdict;
                ImVec4      col;
                if (cpu.presentMs > cpuWork * 1.5f)      { verdict = "GPU-bound (CPU is waiting)";        col = {1.0f, 0.6f, 0.2f, 1.0f}; }
                else if (cpuWork > cpu.presentMs * 1.5f) { verdict = "CPU-bound (GPU is starved)";        col = {0.4f, 0.8f, 1.0f, 1.0f}; }
                else                                     { verdict = "balanced / near vsync";            col = {0.6f, 0.6f, 0.6f, 1.0f}; }
                ImGui::TextColored(col, "=> %s", verdict);
            }
            ImGui::TextDisabled("Halve the window to confirm: if GPU time drops, it is pixel bound.");

            ImGui::SeparatorText("Passes (GPU)");
            s_profilerOverlay.RenderContent(profiler);
            ImGui::End();
        }
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

    std::string sceneAsset; // Phase 21: optional scene override

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
        else if ((arg == "--scene" || arg == "-s") && i + 1 < argc)
        {
            sceneAsset = argv[++i];
        }
    }

    // Phase 21: configure scene asset before Application init (which triggers LoadScene)
    if (!sceneAsset.empty())
        Luna::SceneManager::GetInstance()->SetSceneAsset(sceneAsset);

    Application *app = new Application(spec);
    app->PushLayer(std::make_shared<ExampleLayer>());
    app->PushLayer(std::make_shared<SensorLayer>());
    app->SetMenubarCallback([app]() {
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("Import Scene (glTF/GLB)...", nullptr, false,
                                !Luna::IsFileDialogOpen()))
            {
                Luna::OpenFileDialogAsync(
                    "Import glTF Scene",
                    "glTF Files\0*.gltf;*.glb\0All Files\0*.*\0");
            }
            if (ImGui::MenuItem("Load nuScenes Sample..."))
                SensorLayer::RequestNuScenesPopup();
            ImGui::Separator();
            if (ImGui::MenuItem("Exit"))
                app->Close();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View"))
        {
            ImGui::MenuItem("Scene",       nullptr, &Luna::s_showScene);
            ImGui::MenuItem("Graphics",    nullptr, &Luna::s_showGraphics);
            ImGui::Separator();
            ImGui::MenuItem("GPU Profiler", nullptr, &Luna::s_showProfiler);

            // Sky toggle 
            if (auto* backend = Luna::IRenderContext::GetBackend())
            {
                if (backend->HasSky())
                {
                    ImGui::Separator();
                    bool sky = backend->IsSkyEnabled();
                    if (ImGui::MenuItem("Sky / Atmosphere", nullptr, &sky))
                        backend->SetSkyEnabled(sky);
                }
            }
            ImGui::EndMenu();
        }

    });
    return app;
}
} // namespace Luna
