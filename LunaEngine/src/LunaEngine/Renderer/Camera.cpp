#include "LunaPCH.h"
#include "Renderer/Camera.h"

namespace Luna
{

Camera::Camera(float fovDeg, float aspect, float nearZ, float farZ)
    : _fovRad(XMConvertToRadians(fovDeg))
    , _aspect(aspect)
    , _nearZ(nearZ)
    , _farZ(farZ)
{
}

void Camera::SetAspect(float aspect)
{
    _aspect = aspect;
}

void Camera::Orbit(float dYaw, float dPitch)
{
    _yaw   += dYaw;
    _pitch += dPitch;
    ClampPitch();
}

void Camera::Zoom(float delta)
{
    _radius -= delta;
    if (_radius < 0.5f) _radius = 0.5f;
    if (_radius > 1000.0f) _radius = 1000.0f;
}

XMMATRIX Camera::GetViewMatrix() const
{
    XMFLOAT3 eye = ComputeEye();
    XMVECTOR eyeV    = XMLoadFloat3(&eye);
    XMVECTOR targetV = XMLoadFloat3(&_target);
    XMVECTOR upV     = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    return XMMatrixLookAtLH(eyeV, targetV, upV);
}

XMMATRIX Camera::GetProjectionMatrix() const
{
    return XMMatrixPerspectiveFovLH(_fovRad, _aspect, _nearZ, _farZ);
}

XMFLOAT3 Camera::GetEyePosition() const
{
    return ComputeEye();
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------
void Camera::ClampPitch()
{
    if (_pitch >  89.0f) _pitch =  89.0f;
    if (_pitch < -89.0f) _pitch = -89.0f;
}

XMFLOAT3 Camera::ComputeEye() const
{
    // Spherical → Cartesian (left-handed, Y-up)
    float pitchRad = XMConvertToRadians(_pitch);
    float yawRad   = XMConvertToRadians(_yaw);

    float cosP = cosf(pitchRad);
    float sinP = sinf(pitchRad);
    float cosY = cosf(yawRad);
    float sinY = sinf(yawRad);

    return XMFLOAT3(
        _target.x + _radius * cosP * sinY,
        _target.y + _radius * sinP,
        _target.z + _radius * cosP * cosY);
}

} // namespace Luna
