#pragma once
#include <DirectXMath.h>
using namespace DirectX;

namespace Luna
{
// ---------------------------------------------------------------------------
// Orbital camera — quaternion-based to avoid gimbal lock.
//   Orbit()  — mouse drag (yaw / pitch in degrees)
//   Zoom()   — scroll wheel (adjusts orbit radius)
//   SetAspect() — called on window resize
//   SetOrientation() / GetOrientation() — for gizmo snap-to-view
// ---------------------------------------------------------------------------
class Camera
{
  public:
    Camera(float fovDeg = 45.0f, float aspect = 16.0f / 9.0f,
           float nearZ = 0.1f, float farZ = 500.0f);

    void SetAspect(float aspect);

    // delta values are in degrees
    void Orbit(float dYaw, float dPitch);

    // positive delta zooms in; negative zooms out
    void Zoom(float delta);

    XMMATRIX  GetViewMatrix()       const;
    XMMATRIX  GetProjectionMatrix() const;
    XMFLOAT3  GetEyePosition()      const;

    // Quaternion orientation access (for gizmo snap-to-view)
    XMVECTOR  GetOrientation()      const { return XMLoadFloat4(&_orientation); }
    void      SetOrientation(XMVECTOR quat);

    float     GetRadius() const { return _radius; }
    void      SetRadius(float r) { _radius = r; if (_radius < 0.5f) _radius = 0.5f; }

  private:
    XMFLOAT3 ComputeEye() const;

    XMFLOAT4 _orientation;         // unit quaternion
    float    _radius =   3.0f;     // orbit radius

    XMFLOAT3 _target = {0.0f, 0.0f, 0.0f};

    float _fovRad;
    float _aspect;
    float _nearZ;
    float _farZ;
};
} // namespace Luna
