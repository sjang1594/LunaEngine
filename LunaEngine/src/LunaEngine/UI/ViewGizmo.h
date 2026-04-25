#pragma once

namespace Luna { class Camera; class CameraComponent; }

namespace Luna
{
// Draws a 3D orientation gizmo (XYZ axes) in the top-right corner.
// Clicking an axis tip snaps the camera to that view direction.
void DrawViewGizmo(Camera& camera, float size = 80.0f);
void DrawViewGizmo(CameraComponent& camera, float size = 80.0f);
} // namespace Luna

