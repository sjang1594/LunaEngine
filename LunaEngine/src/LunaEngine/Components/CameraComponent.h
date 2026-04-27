#pragma once
#include "Components/Component.h"

namespace Luna
{

// Isaac Sim-style: Camera lives as a Component on a GameObject in the scene.
// Internally uses Y-up LH for rendering; Z-up is handled at the display layer.
class CameraComponent : public Component
{
public:
    CameraComponent();
    virtual ~CameraComponent() = default;

    // Orbital controls (called from Application input callbacks)
    void Orbit(float dYaw, float dPitch);
    void Zoom(float delta);
    void SetOrientation(XMVECTOR quat);

    // Matrices for the rendering pipeline (Y-up LH)
    XMMATRIX GetViewMatrix() const;
    XMMATRIX GetProjectionMatrix() const;

    // Accessors
    XMFLOAT3 GetEyePosition() const;
    void     SetAspect(float aspect) { _aspect = aspect; }
    float    GetFovDegrees() const { return XMConvertToDegrees(_fovRad); }
    float    GetNearZ() const { return _nearZ; }
    float    GetFarZ()  const { return _farZ; }
    float    GetRadius() const { return _radius; }
    XMFLOAT4 GetOrientationQuat() const { return _orientation; }

    float _fovRad;
    float _aspect;
    float _nearZ;
    float _farZ;
    float _radius = 3.0f;

private:
    XMFLOAT3 ComputeEye() const;

    XMFLOAT4 _orientation = {0, 0, 0, 1};
    XMFLOAT3 _target      = {0, 0, 0};
};

} // namespace Luna

