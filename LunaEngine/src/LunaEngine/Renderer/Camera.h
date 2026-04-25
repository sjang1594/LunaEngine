#pragma once
#include <DirectXMath.h>
using namespace DirectX;

namespace Luna
{
class Camera
{
  public:
    Camera(float fovDeg = 45.0f, float aspect = 16.0f / 9.0f,
           float nearZ = 0.1f, float farZ = 500.0f);

    void SetAspect(float aspect);

    void Orbit(float dYaw, float dPitch);
    void Zoom(float delta);

    XMMATRIX  GetViewMatrix()       const;
    XMMATRIX  GetProjectionMatrix() const;
    XMFLOAT3  GetEyePosition()      const;

    XMVECTOR  GetOrientation()      const { return XMLoadFloat4(&_orientation); }
    void      SetOrientation(XMVECTOR quat);

    float     GetRadius() const { return _radius; }
    void      SetRadius(float r) { _radius = r; if (_radius < 0.5f) _radius = 0.5f; }

  private:
    XMFLOAT3 ComputeEye() const;

    XMFLOAT4 _orientation;
    float    _radius = 3.0f;

    XMFLOAT3 _target = {0.0f, 0.0f, 0.0f};

    float _fovRad;
    float _aspect;
    float _nearZ;
    float _farZ;
};
} // namespace Luna
