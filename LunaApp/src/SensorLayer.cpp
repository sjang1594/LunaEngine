#include <LunaEngine/LunaPCH.h>
#include "SensorLayer.h"
#include <LunaEngine/Sensor/SensorManager.h>
#include <LunaEngine/Sensor/SensorComponent.h>
#include <LunaEngine/Sensor/CameraSensor.h>
#include <LunaEngine/Sensor/LiDARSensor.h>
#include <LunaEngine/Sensor/RadarSensor.h>
#include <LunaEngine/Manager/SceneManager.h>
#include <Scene/Scene.h>
#include <Components/GameObject.h>
#include <Components/Transform.h>
#include <imgui.h>
#include <algorithm>
#include <cmath>

namespace Luna
{

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
            ImGui::MenuItem("Fusion BEV",       nullptr, &_showFusionPanel);
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

    if (_showSensorPanel)  DrawSensorPlacementPanel();
    if (_showCameraPanel)  DrawCameraSensorPanel();
    if (_showLiDARPanel)   DrawLiDARSensorPanel();
    if (_showRadarPanel)   DrawRadarSensorPanel();
    if (_showFusionPanel)  DrawFusionBEVPanel();
}

// ── Sensor Placement Panel ──────────────────────────────────────────
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

    const auto& objects = scene->GetGameObjects();
    for (int idx = 0; idx < (int)objects.size(); idx++)
    {
        const auto& go = objects[idx];
        if (!go) continue;

        auto sensorComp = std::dynamic_pointer_cast<SensorComponent>(
            go->GetComponentByType(ComponentType::SENSOR));

        // Show add-sensor buttons for objects without sensors too
        auto transform = go->GetTransform();
        if (!transform) continue;

        std::string goName = "Object " + std::to_string(idx);
        ImGui::PushID(idx);

        bool hasNode = ImGui::TreeNodeEx(goName.c_str(),
            sensorComp ? ImGuiTreeNodeFlags_DefaultOpen : 0);

        if (hasNode)
        {
            // Add sensor buttons
            if (ImGui::SmallButton("+ Camera"))
            {
                if (!sensorComp)
                {
                    sensorComp = std::make_shared<SensorComponent>();
                    go->AddComponent(sensorComp);
                }
                sensorComp->AddCameraSensor();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("+ LiDAR"))
            {
                if (!sensorComp)
                {
                    sensorComp = std::make_shared<SensorComponent>();
                    go->AddComponent(sensorComp);
                }
                sensorComp->AddLiDARSensor();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("+ Radar"))
            {
                if (!sensorComp)
                {
                    sensorComp = std::make_shared<SensorComponent>();
                    go->AddComponent(sensorComp);
                }
                sensorComp->AddRadarSensor();
            }

            // List existing sensors
            if (sensorComp)
            {
                int removeIdx = -1;
                for (size_t si = 0; si < sensorComp->GetSensorCount(); si++)
                {
                    auto* sensor = sensorComp->GetSensor(si);
                    if (!sensor) continue;

                    ImGui::PushID((int)si);
                    const char* typeStr = SensorTypeToString(sensor->GetType());
                    char label[128];
                    snprintf(label, sizeof(label), "[%s] %s", typeStr, sensor->GetName().c_str());

                    bool open = ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_DefaultOpen);
                    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 20);
                    if (ImGui::SmallButton("X")) removeIdx = (int)si;

                    if (open)
                    {
                        ImGui::Checkbox("Enabled", &sensor->enabled);
                        ImGui::DragFloat3("Offset Pos", &sensor->extrinsicPosition.x, 0.01f);
                        ImGui::DragFloat3("Offset Rot", &sensor->extrinsicRotation.x, 0.5f);
                        ImGui::DragFloat("Tick Interval (s)", &sensor->tickIntervalSec, 0.01f, 0.0f, 1.0f);

                        // Type-specific config
                        switch (sensor->GetType())
                        {
                        case SensorType::Camera:
                        {
                            auto* cam = static_cast<CameraSensor*>(sensor);
                            int w = (int)cam->config.width, h = (int)cam->config.height;
                            if (ImGui::DragInt("Width",  &w, 1, 64, 4096)) cam->config.width = (uint32_t)w;
                            if (ImGui::DragInt("Height", &h, 1, 64, 4096)) cam->config.height = (uint32_t)h;
                            ImGui::DragFloat("FOV",   &cam->config.fovDeg, 0.5f, 10.0f, 170.0f);
                            ImGui::DragFloat("Near",  &cam->config.nearZ, 0.01f, 0.001f, 10.0f);
                            ImGui::DragFloat("Far",   &cam->config.farZ, 1.0f, 1.0f, 1000.0f);
                            ImGui::Separator();
                            ImGui::Text("Lens Distortion");
                            ImGui::DragFloat("k1", &cam->config.k1, 0.001f);
                            ImGui::DragFloat("k2", &cam->config.k2, 0.001f);
                            ImGui::DragFloat("p1", &cam->config.p1, 0.0001f);
                            ImGui::DragFloat("p2", &cam->config.p2, 0.0001f);
                            ImGui::DragFloat("Noise Sigma", &cam->config.noiseSigma, 0.001f, 0.0f, 0.1f);
                            break;
                        }
                        case SensorType::LiDAR:
                        {
                            auto* lidar = static_cast<LiDARSensor*>(sensor);
                            static const char* patterns[] = { "Spinning", "Solid State" };
                            int pat = (int)lidar->config.scanPattern;
                            if (ImGui::Combo("Scan Pattern", &pat, patterns, 2))
                            {
                                lidar->config.scanPattern = (LiDARScanPattern)pat;
                                lidar->InvalidateRays();
                            }
                            if (ImGui::DragFloat("H FOV", &lidar->config.hFovDeg, 1.0f, 10.0f, 360.0f))
                                lidar->InvalidateRays();
                            if (ImGui::DragFloat("V FOV", &lidar->config.vFovDeg, 0.5f, 1.0f, 90.0f))
                                lidar->InvalidateRays();
                            if (ImGui::DragFloat("H Res (deg)", &lidar->config.hResolutionDeg, 0.01f, 0.01f, 5.0f))
                                lidar->InvalidateRays();
                            if (ImGui::DragFloat("V Res (deg)", &lidar->config.vResolutionDeg, 0.1f, 0.1f, 10.0f))
                                lidar->InvalidateRays();
                            ImGui::DragFloat("Max Range (m)", &lidar->config.maxRange, 1.0f, 1.0f, 500.0f);
                            ImGui::DragFloat("Range Noise (m)", &lidar->config.rangeNoiseSigma, 0.001f, 0.0f, 1.0f);
                            ImGui::Text("Ray Count: %u", lidar->GetNumRays());
                            break;
                        }
                        case SensorType::Radar:
                        {
                            auto* radar = static_cast<RadarSensor*>(sensor);
                            ImGui::DragFloat("Center Freq (GHz)", &radar->config.centerFreqGHz, 0.1f, 1.0f, 300.0f);
                            ImGui::DragFloat("Bandwidth (MHz)", &radar->config.bandwidthMHz, 10.0f, 10.0f, 5000.0f);
                            ImGui::DragFloat("Chirp Duration (us)", &radar->config.chirpDurationUs, 1.0f, 1.0f, 500.0f);
                            int nc = (int)radar->config.numChirps;
                            if (ImGui::DragInt("Num Chirps", &nc, 1, 1, 1024))
                                radar->config.numChirps = (uint32_t)nc;
                            ImGui::DragFloat("Max Range (m)", &radar->config.maxRangeM, 1.0f, 1.0f, 500.0f);
                            ImGui::DragFloat("Max Velocity (m/s)", &radar->config.maxVelocityMps, 1.0f, 1.0f, 200.0f);
                            ImGui::DragFloat("H FOV (deg)", &radar->config.hFovDeg, 1.0f, 10.0f, 180.0f);
                            int rb = (int)radar->config.rangeBins, db = (int)radar->config.dopplerBins;
                            if (ImGui::DragInt("Range Bins", &rb, 1, 16, 2048))
                                radar->config.rangeBins = (uint32_t)rb;
                            if (ImGui::DragInt("Doppler Bins", &db, 1, 16, 2048))
                                radar->config.dopplerBins = (uint32_t)db;
                            ImGui::DragFloat("Noise Floor (dB)", &radar->config.noisePowerDb, 0.5f, -60.0f, 0.0f);
                            break;
                        }
                        default: break;
                        }
                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                }
                if (removeIdx >= 0)
                    sensorComp->RemoveSensor((size_t)removeIdx);
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    ImGui::End();
}

// ── Camera Sensor Output Panel ──────────────────────────────────────
void SensorLayer::DrawCameraSensorPanel()
{
    ImGui::Begin("Camera Sensor Output", &_showCameraPanel);

    auto sensors = SensorManager::GetInstance()->GetAllSensors();
    bool anyCam = false;
    for (auto* s : sensors)
    {
        if (s->GetType() != SensorType::Camera) continue;
        anyCam = true;
        auto* cam = static_cast<CameraSensor*>(s);

        if (ImGui::CollapsingHeader(cam->GetName().c_str(), ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text("Resolution: %u x %u", cam->config.width, cam->config.height);
            ImGui::Text("FOV: %.1f deg", cam->config.fovDeg);

            // View/Proj matrix display
            auto& vm = cam->GetViewMatrix();
            ImGui::Text("View pos: (%.2f, %.2f, %.2f)", vm._41, vm._42, vm._43);

            // TODO Phase 2: display rendered RGB and depth textures via ImGui::Image()
            ImVec2 previewSize(320, 240);
            ImGui::BeginChild("rgb_preview", previewSize, true);
            ImGui::TextDisabled("RGB output (Phase 2)");
            ImGui::EndChild();
            ImGui::SameLine();
            ImGui::BeginChild("depth_preview", previewSize, true);
            ImGui::TextDisabled("Depth output (Phase 2)");
            ImGui::EndChild();
        }
    }
    if (!anyCam)
        ImGui::TextDisabled("No camera sensors. Add one in Sensor Placement.");

    ImGui::End();
}

// ── LiDAR Sensor Output Panel ───────────────────────────────────────
void SensorLayer::DrawLiDARSensorPanel()
{
    ImGui::Begin("LiDAR Sensor Output", &_showLiDARPanel);

    auto sensors = SensorManager::GetInstance()->GetAllSensors();
    bool anyLidar = false;
    for (auto* s : sensors)
    {
        if (s->GetType() != SensorType::LiDAR) continue;
        anyLidar = true;
        auto* lidar = static_cast<LiDARSensor*>(s);

        if (ImGui::CollapsingHeader(lidar->GetName().c_str(), ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text("Rays: %u", lidar->GetNumRays());
            ImGui::Text("Points: %u", (uint32_t)lidar->pointCloud.size());

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

            // Draw points (BEV: X right, Z up on screen as forward)
            for (const auto& pt : lidar->pointCloud)
            {
                float px = cx + pt.position.x * scale;
                float py = cy - pt.position.z * scale; // Z forward = up
                uint8_t intensity = (uint8_t)(std::min(pt.intensity, 1.0f) * 255);
                dl->AddCircleFilled(ImVec2(px, py), 2.0f,
                    IM_COL32(intensity, 255 - intensity / 2, 50, 220));
            }

            if (lidar->pointCloud.empty())
            {
                dl->AddText(ImVec2(cx - 60, cy - 8), IM_COL32(100, 100, 100, 200),
                    "No points (Phase 3)");
            }
        }
    }
    if (!anyLidar)
        ImGui::TextDisabled("No LiDAR sensors. Add one in Sensor Placement.");

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

// ── Fusion Bird's-Eye View Panel ────────────────────────────────────
void SensorLayer::DrawFusionBEVPanel()
{
    ImGui::Begin("Fusion BEV", &_showFusionPanel);

    ImVec2 canvasSize(500, 500);
    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("bev_canvas", canvasSize);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Background
    dl->AddRectFilled(canvasPos,
        ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
        IM_COL32(15, 15, 25, 255));

    float cx = canvasPos.x + canvasSize.x * 0.5f;
    float cy = canvasPos.y + canvasSize.y * 0.75f; // ego at bottom-center
    float scale = 3.0f; // pixels per meter

    // Ego vehicle marker
    dl->AddTriangleFilled(
        ImVec2(cx, cy - 10), ImVec2(cx - 6, cy + 6), ImVec2(cx + 6, cy + 6),
        IM_COL32(0, 200, 255, 255));

    // Grid
    for (float d = 20; d <= 100; d += 20)
    {
        dl->AddCircle(ImVec2(cx, cy), d * scale, IM_COL32(30, 30, 50, 150), 64, 1.0f);
        char buf[16]; snprintf(buf, sizeof(buf), "%.0fm", d);
        dl->AddText(ImVec2(cx + 3, cy - d * scale - 12), IM_COL32(60, 60, 80, 200), buf);
    }

    auto sensors = SensorManager::GetInstance()->GetAllSensors();

    // Camera FOV wedges (blue)
    for (auto* s : sensors)
    {
        if (s->GetType() != SensorType::Camera || !s->enabled) continue;
        auto* cam = static_cast<CameraSensor*>(s);
        float halfFov = cam->config.fovDeg * 0.5f * 3.14159f / 180.0f;
        float range = cam->config.farZ * scale;
        range = std::min(range, canvasSize.y * 0.7f);

        ImVec2 left(cx + sinf(-halfFov) * range, cy - cosf(-halfFov) * range);
        ImVec2 right(cx + sinf(halfFov) * range, cy - cosf(halfFov) * range);
        dl->AddTriangleFilled(ImVec2(cx, cy), left, right, IM_COL32(50, 100, 200, 40));
        dl->AddLine(ImVec2(cx, cy), left, IM_COL32(50, 100, 200, 120), 1.0f);
        dl->AddLine(ImVec2(cx, cy), right, IM_COL32(50, 100, 200, 120), 1.0f);
    }

    // Radar FOV wedges (orange)
    for (auto* s : sensors)
    {
        if (s->GetType() != SensorType::Radar || !s->enabled) continue;
        auto* radar = static_cast<RadarSensor*>(s);
        float halfFov = radar->config.hFovDeg * 0.5f * 3.14159f / 180.0f;
        float range = radar->config.maxRangeM * scale;
        range = std::min(range, canvasSize.y * 0.7f);

        ImVec2 left(cx + sinf(-halfFov) * range, cy - cosf(-halfFov) * range);
        ImVec2 right(cx + sinf(halfFov) * range, cy - cosf(halfFov) * range);
        dl->AddTriangleFilled(ImVec2(cx, cy), left, right, IM_COL32(255, 150, 30, 30));
        dl->AddLine(ImVec2(cx, cy), left, IM_COL32(255, 150, 30, 100), 1.0f);
        dl->AddLine(ImVec2(cx, cy), right, IM_COL32(255, 150, 30, 100), 1.0f);
    }

    // LiDAR points (green)
    for (auto* s : sensors)
    {
        if (s->GetType() != SensorType::LiDAR || !s->enabled) continue;
        auto* lidar = static_cast<LiDARSensor*>(s);
        for (const auto& pt : lidar->pointCloud)
        {
            float px = cx + pt.position.x * scale;
            float py = cy - pt.position.z * scale;
            dl->AddCircleFilled(ImVec2(px, py), 1.5f, IM_COL32(0, 255, 80, 200));
        }
    }

    // Radar detections (red diamonds)
    for (auto* s : sensors)
    {
        if (s->GetType() != SensorType::Radar || !s->enabled) continue;
        auto* radar = static_cast<RadarSensor*>(s);
        for (const auto& det : radar->detections)
        {
            float azRad = det.azimuthDeg * 3.14159f / 180.0f;
            float px = cx + sinf(azRad) * det.rangeM * scale;
            float py = cy - cosf(azRad) * det.rangeM * scale;
            float sz = 5.0f;
            dl->AddQuadFilled(
                ImVec2(px, py - sz), ImVec2(px + sz, py),
                ImVec2(px, py + sz), ImVec2(px - sz, py),
                IM_COL32(255, 50, 50, 230));
        }
    }

    // Legend
    float ly = canvasPos.y + 10;
    dl->AddRectFilled(ImVec2(canvasPos.x + 10, ly), ImVec2(canvasPos.x + 20, ly + 10), IM_COL32(50, 100, 200, 200));
    dl->AddText(ImVec2(canvasPos.x + 25, ly - 2), IM_COL32(200, 200, 200, 255), "Camera FOV");
    ly += 16;
    dl->AddRectFilled(ImVec2(canvasPos.x + 10, ly), ImVec2(canvasPos.x + 20, ly + 10), IM_COL32(255, 150, 30, 200));
    dl->AddText(ImVec2(canvasPos.x + 25, ly - 2), IM_COL32(200, 200, 200, 255), "Radar FOV");
    ly += 16;
    dl->AddCircleFilled(ImVec2(canvasPos.x + 15, ly + 5), 3, IM_COL32(0, 255, 80, 200));
    dl->AddText(ImVec2(canvasPos.x + 25, ly - 2), IM_COL32(200, 200, 200, 255), "LiDAR Points");
    ly += 16;
    dl->AddQuadFilled(
        ImVec2(canvasPos.x + 15, ly), ImVec2(canvasPos.x + 20, ly + 5),
        ImVec2(canvasPos.x + 15, ly + 10), ImVec2(canvasPos.x + 10, ly + 5),
        IM_COL32(255, 50, 50, 230));
    dl->AddText(ImVec2(canvasPos.x + 25, ly - 2), IM_COL32(200, 200, 200, 255), "Radar Detections");

    ImGui::End();
}

} // namespace Luna

