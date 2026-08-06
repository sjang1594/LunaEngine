#include <LunaEngine/LunaPCH.h>
#include "SensorLayer.h"
#include <LunaEngine/Sensor/SensorManager.h>
#include <LunaEngine/Sensor/SensorComponent.h>
#include <LunaEngine/Sensor/CameraSensor.h>
#include <LunaEngine/Sensor/LiDARSensor.h>
#include <LunaEngine/Sensor/RadarSensor.h>
#include <LunaEngine/Manager/SceneManager.h>
#include <LunaEngine/Renderer/HAL/Public/IRenderContext.h>
#include <LunaEngine/utils/FileDialog.h>
#include <Scene/Scene.h>
#include <Components/GameObject.h>
#include <Components/Transform.h>
#include <imgui.h>
#include <algorithm>
#include <cmath>

namespace Luna
{

bool SensorLayer::s_openNuScenesPopup = false;

void SensorLayer::OnAttach() {}
void SensorLayer::OnDetach() {}

void SensorLayer::OnUpdate(float dt)
{
    // Tick all sensors via SensorManager
    SensorManager::GetInstance()->Update(dt);
}

void SensorLayer::OnUIRender()
{
    // Main sensor menu window
    ImGui::Begin("Sensor Simulation", nullptr, ImGuiWindowFlags_MenuBar);
    if (ImGui::BeginMenuBar())
    {
        if (ImGui::BeginMenu("View"))
        {
            ImGui::MenuItem("Sensor Placement", nullptr, &_showSensorPanel);
            ImGui::MenuItem("Camera Output",    nullptr, &_showCameraPanel);
            ImGui::MenuItem("LiDAR Output",     nullptr, &_showLiDARPanel);
            ImGui::MenuItem("Radar Output",     nullptr, &_showRadarPanel);
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    // Quick stats
    auto sensors = SensorManager::GetInstance()->GetAllSensors();
    int camCount = 0, lidarCount = 0, radarCount = 0;
    for (auto* s : sensors)
    {
        switch (s->GetType())
        {
        case SensorType::Camera: camCount++;  break;
        case SensorType::LiDAR:  lidarCount++; break;
        case SensorType::Radar:  radarCount++; break;
        default: break;
        }
    }
    ImGui::Text("Active Sensors: %d Camera, %d LiDAR, %d Radar", camCount, lidarCount, radarCount);
    ImGui::End();

    DrawNuScenesPopup();
    DrawAnnotationBoxes();
    if (_showSensorPanel)  DrawSensorPlacementPanel();
    if (_showCameraPanel)  DrawCameraSensorPanel();
    if (_showLiDARPanel)   DrawLiDARSensorPanel();
    if (_showRadarPanel)   DrawRadarSensorPanel();
}

// ── Helpers ─────────────────────────────────────────────────────────────────

SensorComponent* SensorLayer::FindOwnerComponent(ISensor* sensor) const
{
    auto scene = SceneManager::GetInstance()->GetActiveScene();
    if (!scene) return nullptr;
    for (auto& go : scene->GetGameObjects())
    {
        if (!go) continue;
        auto* sc = static_cast<SensorComponent*>(
            go->GetComponentByType(ComponentType::SENSOR).get());
        if (!sc) continue;
        for (size_t i = 0; i < sc->GetSensorCount(); i++)
            if (sc->GetSensor(i) == sensor) return sc;
    }
    return nullptr;
}

// ── Sensor Detail Inspector (right panel) ───────────────────────────────────
void SensorLayer::DrawSensorDetail(ISensor* sensor)
{
    ImGui::Checkbox("Enabled", &sensor->enabled);
    ImGui::Separator();

    ImGui::Text("Extrinsic");
    ImGui::DragFloat3("Position##ext", &sensor->extrinsicPosition.x, 0.01f);
    ImGui::DragFloat3("Rotation##ext", &sensor->extrinsicRotation.x, 0.5f);
    ImGui::DragFloat("Tick (s)", &sensor->tickIntervalSec, 0.01f, 0.0f, 1.0f);
    ImGui::Separator();

    switch (sensor->GetType())
    {
    case SensorType::Camera:
    {
        auto* cam = static_cast<CameraSensor*>(sensor);
        ImGui::Text("Camera");
        int w = (int)cam->config.width, h = (int)cam->config.height;
        if (ImGui::DragInt("Width",  &w, 1, 64, 4096)) cam->config.width  = (uint32_t)w;
        if (ImGui::DragInt("Height", &h, 1, 64, 4096)) cam->config.height = (uint32_t)h;
        ImGui::DragFloat("FOV",  &cam->config.fovDeg, 0.5f, 10.0f, 170.0f);
        ImGui::DragFloat("Near", &cam->config.nearZ,  0.01f, 0.001f, 10.0f);
        ImGui::DragFloat("Far",  &cam->config.farZ,   1.0f,  1.0f,   1000.0f);
        ImGui::Separator();
        ImGui::Text("Lens Distortion");
        ImGui::DragFloat("k1", &cam->config.k1, 0.001f);
        ImGui::DragFloat("k2", &cam->config.k2, 0.001f);
        ImGui::DragFloat("p1", &cam->config.p1, 0.0001f);
        ImGui::DragFloat("p2", &cam->config.p2, 0.0001f);
        ImGui::DragFloat("Shot Noise", &cam->config.shotNoiseFactor, 0.001f, 0.0f, 0.1f);
        break;
    }
    case SensorType::LiDAR:
    {
        auto* lidar = static_cast<LiDARSensor*>(sensor);
        ImGui::Text("LiDAR");
        static const char* patterns[] = { "Spinning", "Solid State" };
        int pat = (int)lidar->config.scanPattern;
        if (ImGui::Combo("Pattern", &pat, patterns, 2))
        { lidar->config.scanPattern = (LiDARScanPattern)pat; lidar->InvalidateRays(); }
        if (ImGui::DragFloat("H FOV (deg)",   &lidar->config.hFovDeg,         1.0f, 10.0f, 360.0f)) lidar->InvalidateRays();
        if (ImGui::DragFloat("V FOV (deg)",   &lidar->config.vFovDeg,         0.5f,  1.0f,  90.0f)) lidar->InvalidateRays();
        if (ImGui::DragFloat("H Res (deg)",   &lidar->config.hResolutionDeg,  0.01f, 0.01f,  5.0f)) lidar->InvalidateRays();
        if (ImGui::DragFloat("V Res (deg)",   &lidar->config.vResolutionDeg,  0.1f,  0.1f,  10.0f)) lidar->InvalidateRays();
        ImGui::DragFloat("Max Range (m)",     &lidar->config.maxRange,        1.0f,  1.0f, 500.0f);
        ImGui::DragFloat("Range Noise (m)",   &lidar->config.rangeNoiseSigma, 0.001f, 0.0f,  1.0f);
        ImGui::DragFloat("NIR Multiplier",    &lidar->config.nirMultiplier,   0.01f, 0.1f,  5.0f);
        ImGui::Separator();
        ImGui::Text("Rays: %u   Points: %u", lidar->GetNumRays(), (uint32_t)lidar->pointCloud.size());
        break;
    }
    case SensorType::Radar:
    {
        auto* radar = static_cast<RadarSensor*>(sensor);
        ImGui::Text("Radar");
        ImGui::DragFloat("Center Freq (GHz)",  &radar->config.centerFreqGHz,   0.1f,  1.0f, 300.0f);
        ImGui::DragFloat("Bandwidth (MHz)",    &radar->config.bandwidthMHz,    10.0f, 10.0f, 5000.0f);
        ImGui::DragFloat("Chirp Duration (us)",&radar->config.chirpDurationUs,  1.0f,  1.0f,  500.0f);
        int nc = (int)radar->config.numChirps;
        if (ImGui::DragInt("Num Chirps", &nc, 1, 1, 1024)) radar->config.numChirps = (uint32_t)nc;
        ImGui::DragFloat("Max Range (m)",      &radar->config.maxRangeM,       1.0f,  1.0f, 500.0f);
        ImGui::DragFloat("Max Vel (m/s)",      &radar->config.maxVelocityMps,  1.0f,  1.0f, 200.0f);
        ImGui::DragFloat("H FOV (deg)",        &radar->config.hFovDeg,         1.0f, 10.0f, 180.0f);
        int rb = (int)radar->config.rangeBins, db = (int)radar->config.dopplerBins;
        if (ImGui::DragInt("Range Bins",  &rb, 1, 16, 2048)) radar->config.rangeBins   = (uint32_t)rb;
        if (ImGui::DragInt("Doppler Bins",&db, 1, 16, 2048)) radar->config.dopplerBins = (uint32_t)db;
        ImGui::DragFloat("Noise Floor (dB)",   &radar->config.noisePowerDb,    0.5f, -60.0f, 0.0f);
        break;
    }
    default: break;
    }
}

// ── Sensor Placement Panel — list (left) + inspector (right) ────────────────
void SensorLayer::DrawSensorPlacementPanel()
{
    ImGui::Begin("Sensor Placement", &_showSensorPanel);

    auto scene = SceneManager::GetInstance()->GetActiveScene();
    if (!scene)
    {
        ImGui::TextDisabled("No active scene");
        ImGui::End();
        return;
    }

    auto allSensors = SensorManager::GetInstance()->GetAllSensors();

    // Validate selection (guard against scene reload invalidating the pointer)
    bool selectionValid = false;
    for (auto* s : allSensors)
        if (s == _selectedSensor) { selectionValid = true; break; }
    if (!selectionValid) _selectedSensor = nullptr;

    // ── Left: sensor list ─────────────────────────────────────────────────────
    ImGui::BeginChild("##sensor_list", ImVec2(200, 0), true);

    // Add-sensor toolbar (adds to the first SensorComponent in the scene)
    auto findRigComp = [&]() -> SensorComponent*
    {
        for (auto& go : scene->GetGameObjects())
        {
            if (!go) continue;
            auto* sc = static_cast<SensorComponent*>(
                go->GetComponentByType(ComponentType::SENSOR).get());
            if (sc) return sc;
        }
        return nullptr;
    };
    if (ImGui::SmallButton("+ Cam"))
    {
        auto* sc = findRigComp();
        if (sc) sc->AddCameraSensor();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("+ LiDAR"))
    {
        auto* sc = findRigComp();
        if (sc) sc->AddLiDARSensor();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("+ Radar"))
    {
        auto* sc = findRigComp();
        if (sc) sc->AddRadarSensor();
    }
    ImGui::Separator();

    ISensor* toRemove = nullptr;
    for (auto* s : allSensors)
    {
        char label[128];
        snprintf(label, sizeof(label), "[%s] %s",
                 SensorTypeToString(s->GetType()), s->GetName().c_str());

        bool selected = (_selectedSensor == s);
        if (ImGui::Selectable(label, selected))
            _selectedSensor = s;

        // Right-click context menu: remove
        if (ImGui::BeginPopupContextItem())
        {
            if (ImGui::MenuItem("Remove")) toRemove = s;
            ImGui::EndPopup();
        }
    }

    if (toRemove)
    {
        auto* sc = FindOwnerComponent(toRemove);
        if (sc)
        {
            for (size_t i = 0; i < sc->GetSensorCount(); i++)
            {
                if (sc->GetSensor(i) == toRemove)
                {
                    if (_selectedSensor == toRemove) _selectedSensor = nullptr;
                    sc->RemoveSensor(i);
                    break;
                }
            }
        }
    }

    ImGui::EndChild();

    ImGui::SameLine();

    // ── Right: detail inspector ───────────────────────────────────────────────
    ImGui::BeginChild("##sensor_detail", ImVec2(0, 0), true);
    if (_selectedSensor)
    {
        ImGui::Text("%s", _selectedSensor->GetName().c_str());
        ImGui::Separator();
        DrawSensorDetail(_selectedSensor);
    }
    else
    {
        ImGui::TextDisabled("Select a sensor from the list");
    }
    ImGui::EndChild();

    ImGui::End();
}

// ── (removed) old per-GO tree implementation replaced above ─────────────────
// Keeping Camera / LiDAR / Radar output panels below unchanged.

// ── Camera Sensor Output Panel ──────────────────────────────────────
void SensorLayer::DrawCameraSensorPanel()
{
    ImGui::Begin("Camera Sensor Output", &_showCameraPanel);

    // ── nuScenes-6 preset ─────────────────────────────────────────────────────
    if (ImGui::Button("Add nuScenes-6 Preset"))
    {
        // Standard nuScenes camera rig: 6 cameras at ~1.5m height, pointing outward
        struct NuScenesCam { const char* name; float yawDeg; float x, y, z; float fovDeg; };
        static const NuScenesCam nsCams[] = {
            { "CAM_FRONT",       0.0f,   1.70f, 1.51f, 0.0f, 70.0f },
            { "CAM_FRONT_LEFT",  55.0f,  1.55f, 1.52f, 0.0f, 70.0f },
            { "CAM_FRONT_RIGHT",-55.0f,  1.55f, 1.51f, 0.0f, 70.0f },
            { "CAM_BACK",       180.0f, -0.02f, 1.52f, 0.0f, 110.0f},
            { "CAM_BACK_LEFT",  110.0f, -0.32f, 1.52f, 0.0f, 70.0f },
            { "CAM_BACK_RIGHT",-110.0f, -0.32f, 1.51f, 0.0f, 70.0f },
        };

        auto* scene = SceneManager::GetInstance()->GetActiveScene().get();
        if (scene && !scene->GetGameObjects().empty())
        {
            // Add to the first game object (treat as vehicle)
            auto& go = scene->GetGameObjects()[0];
            auto* sc = go ? static_cast<SensorComponent*>(
                go->GetComponentByType(ComponentType::SENSOR).get()) : nullptr;
            if (sc)
            {
                for (auto& ns : nsCams)
                {
                    auto cam = std::make_shared<CameraSensor>(ns.name);
                    cam->config.width   = 1600;
                    cam->config.height  = 900;
                    cam->config.fovDeg  = ns.fovDeg;
                    cam->config.nearZ   = 0.1f;
                    cam->config.farZ    = 150.0f;
                    cam->extrinsicPosition = { ns.x, ns.y, ns.z };
                    cam->extrinsicRotation = { 0.0f, ns.yawDeg, 0.0f };
                    cam->UpdateExtrinsicFromPosRot();
                    sc->AddSensor(cam);
                }
            }
        }
    }
    ImGui::Separator();

    // ── Per-camera display ────────────────────────────────────────────────────
    auto sensors = SensorManager::GetInstance()->GetAllSensors();
    bool anyCam = false;
    for (auto* s : sensors)
    {
        if (s->GetType() != SensorType::Camera) continue;
        anyCam = true;
        auto* cam = static_cast<CameraSensor*>(s);

        if (ImGui::CollapsingHeader(cam->GetName().c_str(), ImGuiTreeNodeFlags_DefaultOpen))
        {
            // Config editors
            ImGui::Text("Resolution: %u x %u   FOV: %.1f deg",
                        cam->config.width, cam->config.height, cam->config.fovDeg);
            ImGui::SliderFloat("k1##dist", &cam->config.k1, -0.5f, 0.5f);
            ImGui::SliderFloat("k2##dist", &cam->config.k2, -0.5f, 0.5f);
            ImGui::SliderFloat("k3##dist", &cam->config.k3, -0.5f, 0.5f);
            ImGui::SliderFloat("Exposure EV100", &cam->config.exposureEV100, -3.0f, 3.0f);
            ImGui::SliderFloat("Shot noise", &cam->config.shotNoiseFactor, 0.0f, 1.0f);

            // Camera output image
            float aspect = (float)cam->config.width / (float)cam->config.height;
            ImVec2 previewSize(360.0f, 360.0f / aspect);

            if (cam->gpuTextureHandle != 0)
            {
                // S2: real GPU-rendered + distorted output
                ImGui::Image(
                    (ImTextureID)cam->gpuTextureHandle,
                    previewSize);
                if (cam->hasNewFrame)
                    ImGui::TextColored(ImVec4(0,1,0,1), "Live");
                else
                    ImGui::TextColored(ImVec4(1,1,0,1), "Waiting for frame...");
            }
            else
            {
                ImGui::BeginChild("cam_placeholder", previewSize, true);
                ImGui::TextDisabled("GPU resources initializing...");
                ImGui::EndChild();
            }
        }
    }
    if (!anyCam)
        ImGui::TextDisabled("No camera sensors. Add one in Sensor Placement or use nuScenes-6 preset.");

    ImGui::End();
}

// ── LiDAR Sensor Output Panel ───────────────────────────────────────
void SensorLayer::DrawLiDARSensorPanel()
{
    ImGui::Begin("LiDAR Sensor Output", &_showLiDARPanel);

    // ── Velodyne HDL-32E preset ───────────────────────────────────────────────
    if (ImGui::Button("Add Velodyne HDL-32E"))
    {
        auto* scene = SceneManager::GetInstance()->GetActiveScene().get();
        if (scene && !scene->GetGameObjects().empty())
        {
            auto& go = scene->GetGameObjects()[0];
            auto* sc = go ? static_cast<SensorComponent*>(
                go->GetComponentByType(ComponentType::SENSOR).get()) : nullptr;
            if (sc)
            {
                auto lidar = std::make_shared<LiDARSensor>("HDL-32E");
                lidar->config = LiDARSensorConfig::MakeHDL32E();
                lidar->extrinsicPosition = { 0.0f, 1.8f, 0.0f }; // roof mount
                lidar->UpdateExtrinsicFromPosRot();
                lidar->tickIntervalSec = 1.0f / 10.0f; // 10 Hz
                sc->AddSensor(lidar);
            }
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("32 beams, 0.2° H res → ~57,600 pts/scan");
    ImGui::Separator();

    auto sensors = SensorManager::GetInstance()->GetAllSensors();
    bool anyLidar = false;
    for (auto* s : sensors)
    {
        if (s->GetType() != SensorType::LiDAR) continue;
        anyLidar = true;
        auto* lidar = static_cast<LiDARSensor*>(s);

        if (ImGui::CollapsingHeader(lidar->GetName().c_str(), ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text("Beams: %u   Rays: %u   Points: %u",
                        (uint32_t)lidar->config.beamElevationsDeg.size(),
                        lidar->GetNumRays(),
                        (uint32_t)lidar->pointCloud.size());
            ImGui::SliderFloat("NIR multiplier", &lidar->config.nirMultiplier, 0.1f, 5.0f);
            ImGui::SliderFloat("Max range (m)", &lidar->config.maxRange, 10.0f, 200.0f);

            // Point cloud BEV visualization via ImDrawList
            ImVec2 canvasSize(400, 400);
            ImVec2 canvasPos = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("lidar_canvas", canvasSize);
            ImDrawList* dl = ImGui::GetWindowDrawList();

            // Background
            dl->AddRectFilled(canvasPos,
                ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
                IM_COL32(20, 20, 30, 255));

            // Grid
            float cx = canvasPos.x + canvasSize.x * 0.5f;
            float cy = canvasPos.y + canvasSize.y * 0.5f;
            dl->AddCircle(ImVec2(cx, cy), 5.0f, IM_COL32(0, 255, 0, 200), 12, 2.0f);

            // Range rings
            float maxR = lidar->config.maxRange;
            float scale = (canvasSize.x * 0.45f) / maxR;
            for (float r = maxR * 0.25f; r <= maxR; r += maxR * 0.25f)
            {
                dl->AddCircle(ImVec2(cx, cy), r * scale, IM_COL32(40, 40, 60, 200), 64, 1.0f);
                char buf[32]; snprintf(buf, sizeof(buf), "%.0fm", r);
                dl->AddText(ImVec2(cx + r * scale + 2, cy), IM_COL32(80, 80, 100, 200), buf);
            }

            // Draw points (BEV: X right, Z forward = up on canvas)
            for (const auto& pt : lidar->pointCloud)
            {
                float px = cx + pt.position.x * scale;
                float py = cy - pt.position.z * scale;
                uint8_t intensity = (uint8_t)(std::min(pt.intensity, 1.0f) * 255);
                dl->AddCircleFilled(ImVec2(px, py), 2.0f,
                    IM_COL32(intensity, 255 - intensity / 2, 50, 220));
            }
            if (lidar->pointCloud.empty())
                dl->AddText(ImVec2(cx - 60, cy - 8), IM_COL32(100, 100, 100, 200), "No points yet");
        }
    }
    if (!anyLidar)
        ImGui::TextDisabled("No LiDAR sensors.");
    ImGui::End();
}

// ── Radar Sensor Output Panel ───────────────────────────────────────
void SensorLayer::DrawRadarSensorPanel()
{
    ImGui::Begin("Radar Sensor Output", &_showRadarPanel);

    auto sensors = SensorManager::GetInstance()->GetAllSensors();
    bool anyRadar = false;
    for (auto* s : sensors)
    {
        if (s->GetType() != SensorType::Radar) continue;
        anyRadar = true;
        auto* radar = static_cast<RadarSensor*>(s);

        if (ImGui::CollapsingHeader(radar->GetName().c_str(), ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text("Range bins: %u, Doppler bins: %u",
                radar->config.rangeBins, radar->config.dopplerBins);
            ImGui::Text("Detections: %u", (uint32_t)radar->detections.size());

            // Range-Doppler heatmap via ImDrawList
            uint32_t rb = radar->config.rangeBins;
            uint32_t db = radar->config.dopplerBins;
            if (!radar->rangeDopplerMap.empty() && rb > 0 && db > 0)
            {
                // Draw as colored pixels
                float cellW = 400.0f / (float)db;
                float cellH = 300.0f / (float)rb;
                // Clamp for performance: max 256x256 display
                uint32_t dispRb = std::min(rb, 256u);
                uint32_t dispDb = std::min(db, 256u);
                float stepR = (float)rb / (float)dispRb;
                float stepD = (float)db / (float)dispDb;
                cellW = 400.0f / (float)dispDb;
                cellH = 300.0f / (float)dispRb;

                ImVec2 origin = ImGui::GetCursorScreenPos();
                ImGui::InvisibleButton("radar_heatmap", ImVec2(400, 300));
                ImDrawList* dl = ImGui::GetWindowDrawList();

                // Find min/max for normalization
                float vmin = radar->rangeDopplerMap[0], vmax = vmin;
                for (float v : radar->rangeDopplerMap)
                {
                    vmin = std::min(vmin, v);
                    vmax = std::max(vmax, v);
                }
                float range = (vmax - vmin) > 1e-6f ? (vmax - vmin) : 1.0f;

                for (uint32_t ri = 0; ri < dispRb; ri++)
                {
                    for (uint32_t di = 0; di < dispDb; di++)
                    {
                        uint32_t srcR = (uint32_t)(ri * stepR);
                        uint32_t srcD = (uint32_t)(di * stepD);
                        float val = radar->rangeDopplerMap[srcR * db + srcD];
                        float norm = (val - vmin) / range;

                        // Viridis-ish colormap
                        uint8_t r = (uint8_t)(std::min(1.0f, norm * 3.0f) * 255);
                        uint8_t g = (uint8_t)(std::min(1.0f, norm * 1.5f) * 255);
                        uint8_t b = (uint8_t)((1.0f - norm) * 200);

                        ImVec2 p0(origin.x + di * cellW, origin.y + ri * cellH);
                        ImVec2 p1(p0.x + cellW + 1, p0.y + cellH + 1);
                        dl->AddRectFilled(p0, p1, IM_COL32(r, g, b, 255));
                    }
                }

                // Axis labels
                dl->AddText(ImVec2(origin.x + 160, origin.y + 302), IM_COL32(200, 200, 200, 255), "Doppler");
                dl->AddText(ImVec2(origin.x - 40, origin.y + 140), IM_COL32(200, 200, 200, 255), "Range");
            }
        }
    }
    if (!anyRadar)
        ImGui::TextDisabled("No radar sensors. Add one in Sensor Placement.");

    ImGui::End();
}


// ── nuScenes OBB Wireframe Overlay ──────────────────────────────────────────
// Projects each annotation's 8 corners through VP, draws 12 edges with ImGui.
void SensorLayer::DrawAnnotationBoxes()
{
    auto* mgr = SceneManager::GetInstance();
    if (!mgr->HasNuScenesSample()) return;
    const auto& sample = mgr->GetNuScenesSample();
    if (sample.annotations.empty()) return;

    auto* backend = IRenderContext::GetBackend();
    if (!backend) return;

    using namespace DirectX;

    // VP matrix + screen size
    XMFLOAT4X4 vpF = backend->GetCurrentVP();
    XMMATRIX    VP  = XMLoadFloat4x4(&vpF);

    // With ImGuiConfigFlags_ViewportsEnable the main viewport's Pos is the window's
    // desktop position, not (0,0), and GetBackgroundDrawList() expects absolute
    // viewport coordinates. Omitting Pos offsets every box by the window position.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 scrOrigin  = vp->Pos;
    const ImVec2 scr        = vp->Size;
    if (scr.x < 1 || scr.y < 1) return;

    ImDrawList* dl = ImGui::GetBackgroundDrawList();

    // Unit box local corners (centre = origin, extent = [-0.5, 0.5] per axis)
    static const XMFLOAT3 kCorners[8] = {
        {-0.5f,-0.5f,-0.5f}, { 0.5f,-0.5f,-0.5f},
        { 0.5f, 0.5f,-0.5f}, {-0.5f, 0.5f,-0.5f},
        {-0.5f,-0.5f, 0.5f}, { 0.5f,-0.5f, 0.5f},
        { 0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f},
    };
    // 12 edges as pairs of corner indices
    static const int kEdges[12][2] = {
        {0,1},{1,2},{2,3},{3,0},  // bottom face
        {4,5},{5,6},{6,7},{7,4},  // top face
        {0,4},{1,5},{2,6},{3,7},  // verticals
    };

    // Color per category (simple hash by first char)
    auto categoryColor = [](const std::string& cat) -> ImU32 {
        if (cat.find("car")        != std::string::npos) return IM_COL32( 50,255, 50,220);
        if (cat.find("pedestrian") != std::string::npos) return IM_COL32(255,220, 50,220);
        if (cat.find("bicycle")    != std::string::npos) return IM_COL32( 50,200,255,220);
        if (cat.find("motorcycle") != std::string::npos) return IM_COL32(255,100, 50,220);
        if (cat.find("truck")      != std::string::npos) return IM_COL32(200, 50,255,220);
        if (cat.find("bus")        != std::string::npos) return IM_COL32(255, 50,150,220);
        return IM_COL32(180,180,180,180);
    };

    auto projectToScreen = [&](XMVECTOR worldPos) -> ImVec2
    {
        XMVECTOR clip = XMVector4Transform(worldPos, VP);
        float w = XMVectorGetW(clip);
        if (fabsf(w) < 1e-4f) return {-9999, -9999};
        float nx = XMVectorGetX(clip) / w;
        float ny = XMVectorGetY(clip) / w;
        float nz = XMVectorGetZ(clip) / w;
        if (nz < 0.0f || nz > 1.0f) return {-9999, -9999}; // behind camera or beyond far plane
        return { scrOrigin.x + (nx * 0.5f + 0.5f) * scr.x,
                 scrOrigin.y + (1.0f - (ny * 0.5f + 0.5f)) * scr.y };
    };

    for (const auto& box : sample.annotations)
    {
        // World matrix: scale → rotate → translate
        XMVECTOR q = XMVectorSet(box.rotation.x, box.rotation.y,
                                  box.rotation.z, box.rotation.w);
        XMMATRIX M = XMMatrixScaling(box.size.x, box.size.y, box.size.z)
                   * XMMatrixRotationQuaternion(q)
                   * XMMatrixTranslation(box.translation.x,
                                         box.translation.y,
                                         box.translation.z);

        // Transform 8 corners to world space then project
        ImVec2 screenPts[8];
        bool anyVisible = false;
        for (int ci = 0; ci < 8; ++ci)
        {
            XMVECTOR lc = XMVectorSet(kCorners[ci].x, kCorners[ci].y,
                                       kCorners[ci].z, 1.0f);
            XMVECTOR wc = XMVector4Transform(lc, M);
            screenPts[ci] = projectToScreen(wc);
            if (screenPts[ci].x > -999) anyVisible = true;
        }
        if (!anyVisible) continue;

        ImU32 col = categoryColor(box.category);
        for (auto& e : kEdges)
        {
            auto& a = screenPts[e[0]];
            auto& b = screenPts[e[1]];
            if (a.x < -999 || b.x < -999) continue;
            dl->AddLine(a, b, col, 1.5f);
        }
    }
}

// ── nuScenes Loader Popup (called top-level from OnUIRender) ─────────────────
void SensorLayer::DrawNuScenesPopup()
{
    if (s_openNuScenesPopup)
    {
        ImGui::OpenPopup("##ns_load");
        s_openNuScenesPopup = false;
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(580, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("##ns_load", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize |
                                ImGuiWindowFlags_NoTitleBar))
        return;

    auto* mgr    = SceneManager::GetInstance();
    auto& loader = mgr->GetNuScenesLoader();

    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Load nuScenes Sample");
    ImGui::Separator();

    // Data root
    ImGui::Text("nuScenes directory (v1.0-xxx):");
    ImGui::SetNextItemWidth(420);
    ImGui::InputText("##ns_root", _nsDataRoot, sizeof(_nsDataRoot));
    ImGui::SameLine();
    if (ImGui::Button("Browse"))
    {
        std::string picked = OpenFileDialog(
            "Select any file inside the nuScenes directory", "All Files\0*.*\0");
        if (!picked.empty())
        {
            auto sep = picked.find_last_of("/\\");
            std::string folder = (sep != std::string::npos) ? picked.substr(0, sep) : picked;
            strncpy_s(_nsDataRoot, folder.c_str(), sizeof(_nsDataRoot) - 1);
        }
    }

    if (ImGui::Button("Load JSON tables") && _nsDataRoot[0] != '\0')
        loader.Load(std::string(_nsDataRoot));

    if (loader.IsLoaded())
    {
        ImGui::Separator();
        const auto& scenes = loader.GetScenes();
        std::vector<const char*> names;
        for (auto& s : scenes) names.push_back(s.name.c_str());

        if (_nsSceneIdx >= (int)names.size()) _nsSceneIdx = 0;
        ImGui::SetNextItemWidth(200);
        ImGui::Combo("Scene", &_nsSceneIdx, names.data(), (int)names.size());

        if (!scenes.empty())
        {
            const auto& sc = scenes[_nsSceneIdx];
            ImGui::SameLine();
            ImGui::TextDisabled("%s", sc.description.c_str());

            _nsSampleIdx = std::clamp(_nsSampleIdx, 0, std::max(0, sc.numSamples - 1));
            ImGui::SetNextItemWidth(200);
            ImGui::SliderInt("Sample", &_nsSampleIdx, 0, std::max(0, sc.numSamples - 1));
            ImGui::Text("%d samples total", sc.numSamples);

            ImGui::Separator();
            if (ImGui::Button("Load Scene", ImVec2(200, 0)))
            {
                mgr->LoadNuScenesScene(std::string(_nsDataRoot), sc.token, _nsSampleIdx);
                ImGui::CloseCurrentPopup();
            }
        }
    }
    else
    {
        ImGui::Separator();
        ImGui::TextDisabled("Enter the path above and click \"Load JSON tables\".");
    }

    ImGui::Separator();
    if (ImGui::Button("Cancel", ImVec2(120, 0)))
        ImGui::CloseCurrentPopup();

    ImGui::EndPopup();
}

// ── nuScenes Loader Panel ────────────────────────────────────────────────────
void SensorLayer::DrawNuScenesPanel()
{
    ImGui::Begin("nuScenes Loader", &_showNuScenesPanel);

    auto* mgr = SceneManager::GetInstance();
    auto& loader = mgr->GetNuScenesLoader();

    // ── Data root path ────────────────────────────────────────────────────────
    ImGui::Text("Data Root (v1.0-xxx directory):");
    ImGui::InputText("##ns_root", _nsDataRoot, sizeof(_nsDataRoot));
    ImGui::SameLine();
    if (ImGui::Button("Browse..."))
    {
        // Pick any file inside the nuScenes directory; strip the filename to get the folder.
        std::string picked = OpenFileDialog("Select any file in nuScenes directory", "All Files\0*.*\0");
        if (!picked.empty())
        {
            auto sep = picked.find_last_of("/\\");
            std::string folder = (sep != std::string::npos) ? picked.substr(0, sep) : picked;
            strncpy_s(_nsDataRoot, folder.c_str(), sizeof(_nsDataRoot) - 1);
        }
    }

    // ── Load JSON tables ──────────────────────────────────────────────────────
    if (ImGui::Button("Load JSON") && _nsDataRoot[0] != '\0')
    {
        loader.Load(std::string(_nsDataRoot));
    }

    if (!loader.IsLoaded())
    {
        ImGui::TextDisabled("Not loaded. Set data root and click Load JSON.");
        ImGui::End();
        return;
    }

    // ── Scene selector ────────────────────────────────────────────────────────
    ImGui::Separator();
    const auto& scenes = loader.GetScenes();
    std::vector<const char*> sceneNames;
    for (auto& s : scenes) sceneNames.push_back(s.name.c_str());

    ImGui::Text("%zu scenes loaded", scenes.size());
    if (!sceneNames.empty())
    {
        if (_nsSceneIdx >= (int)sceneNames.size()) _nsSceneIdx = 0;
        ImGui::Combo("Scene", &_nsSceneIdx, sceneNames.data(), (int)sceneNames.size());

        const auto& sc = scenes[_nsSceneIdx];
        ImGui::TextWrapped("  %s", sc.description.c_str());
        ImGui::Text("Samples: %d", sc.numSamples);

        _nsSampleIdx = std::clamp(_nsSampleIdx, 0, std::max(0, sc.numSamples - 1));
        ImGui::SliderInt("Sample", &_nsSampleIdx, 0, std::max(0, sc.numSamples - 1));

        ImGui::Separator();
        if (ImGui::Button("Load Scene", ImVec2(-1, 0)))
        {
            mgr->LoadNuScenesScene(std::string(_nsDataRoot), sc.token, _nsSampleIdx);
        }
    }

    // ── Sample status ─────────────────────────────────────────────────────────
    if (mgr->HasNuScenesSample())
    {
        ImGui::Separator();
        const auto& sd = mgr->GetNuScenesSample();
        ImGui::Text("Annotations: %zu", sd.annotations.size());
        ImGui::Text("LiDAR points: %zu (%s)",
                    sd.lidarPoints.size(), sd.lidarLoaded ? "loaded" : "missing");
        ImGui::Text("LIDAR_TOP pos: (%.2f, %.2f, %.2f)",
                    sd.lidarPosition.x, sd.lidarPosition.y, sd.lidarPosition.z);
    }

    ImGui::End();
}

} // namespace Luna

