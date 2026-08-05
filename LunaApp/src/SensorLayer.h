#pragma once
#include <LunaEngine/Layer.h>

namespace Luna
{
class ISensor;
class SensorComponent;

// ImGui layer for sensor simulation controls and output visualization.
class SensorLayer : public Layer
{
public:
    void OnAttach() override;
    void OnDetach() override;
    void OnUpdate(float dt) override;
    void OnUIRender() override;

    static void RequestNuScenesPopup() { s_openNuScenesPopup = true; }

private:
    void DrawSensorPlacementPanel();
    void DrawSensorDetail(ISensor* sensor);   // right-panel inspector
    void DrawCameraSensorPanel();
    void DrawLiDARSensorPanel();
    void DrawRadarSensorPanel();

    // Finds the SensorComponent that owns the given sensor (for add/remove ops)
    SensorComponent* FindOwnerComponent(ISensor* sensor) const;

    // nuScenes load + sample selector
    void DrawNuScenesPanel();               

    bool     _showSensorPanel  = true;
    bool     _showCameraPanel  = true;
    bool     _showLiDARPanel   = true;
    bool     _showRadarPanel   = true;
    bool     _showNuScenesPanel = true;
    ISensor* _selectedSensor   = nullptr;   // currently selected in the list

    // nuScenes popup + OBB overlay
    void DrawNuScenesPopup();
    void DrawAnnotationBoxes();      // 3D OBB wireframe projected via ImGui
    static bool s_openNuScenesPopup;

    char _nsDataRoot[512] = {};
    int  _nsSceneIdx      = 0;
    int  _nsSampleIdx     = 0;
};

} // namespace Luna