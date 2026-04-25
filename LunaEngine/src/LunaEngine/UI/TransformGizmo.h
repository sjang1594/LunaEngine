#pragma once

namespace Luna
{
class Camera;
class CameraComponent;
class Transform;

// Gizmo mode (Unreal-style: W=Translate, E=Rotate, R=Scale)
enum class GizmoMode { Translate, Rotate, Scale };

// Set/get current gizmo mode
void SetGizmoMode(GizmoMode mode);
GizmoMode GetGizmoMode();

// Draw the transform gizmo for the selected object.
// Only shows manipulators for the current mode.
void DrawTransformGizmo(Camera& camera, Transform& transform,
                        bool localSpace = false, float pixelSize = 120.0f);
void DrawTransformGizmo(CameraComponent& camera, Transform& transform,
                        bool localSpace = false, float pixelSize = 120.0f);

bool IsGizmoDragging();
bool IsGizmoHovered();

} // namespace Luna

