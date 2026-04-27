#pragma once
#include <LunaEngine/Layer.h>

namespace Luna
{

// ImGui layer for sensor simulation controls and output visualization.
class SensorLayer : public Layer
{
public:
    void OnAttach() override;
    void OnDetach() override;
    void OnUpdate(float dt) override;
    void OnUIRender() override;

private:
    void DrawSensorPlacementPanel();
    void DrawCameraSensorPanel();
    void DrawLiDARSensorPanel();
    void DrawRadarSensorPanel();
    void DrawFusionBEVPanel();

    bool _showSensorPanel  = true;
    bool _showCameraPanel  = true;
    bool _showLiDARPanel   = true;
    bool _showRadarPanel   = true;
    bool _showFusionPanel  = true;
};

} // namespace Luna

