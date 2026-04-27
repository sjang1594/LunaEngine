#include "LunaPCH.h"
#include "CameraComponent.h"
#include "Components/Transform.h"

namespace Luna
{

CameraComponent::CameraComponent()
    : Component(ComponentType::CAMERA)
    , _fovRad(XMConvertToRadians(45.0f))
    , _aspect(16.0f / 9.0f)
    , _nearZ(0.1f)
    , _farZ(500.0f)
{
    // Default: 30° elevation, looking down at target from above-behind.
    XMVECTOR q = XMQuaternionRotationAxis(XMVectorSet(1, 0, 0, 0), XMConvertToRadians(-30.0f));
    XMStoreFloat4(&_orientation, XMQuaternionNormalize(q));
}

void CameraComponent::Orbit(float dYaw, float dPitch)
{
    XMVECTOR q = XMLoadFloat4(&_orientation);

    // Yaw around world Y (vertical axis in Y-up)
    XMVECTOR qYaw = XMQuaternionRotationAxis(
        XMVectorSet(0, 1, 0, 0), XMConvertToRadians(dYaw));
    q = XMQuaternionMultiply(qYaw, q);

    // Pitch around camera's local right (X)
    XMVECTOR right = XMVector3Rotate(XMVectorSet(1, 0, 0, 0), q);
    XMVECTOR qPitch = XMQuaternionRotationAxis(right, XMConvertToRadians(-dPitch));
    q = XMQuaternionNormalize(XMQuaternionMultiply(qPitch, q));

    XMStoreFloat4(&_orientation, q);
}

void CameraComponent::Zoom(float delta)
{
    _radius -= delta;
    if (_radius < 0.5f)   _radius = 0.5f;
    if (_radius > 1000.0f) _radius = 1000.0f;
}

void CameraComponent::SetOrientation(XMVECTOR quat)
{
    XMStoreFloat4(&_orientation, XMQuaternionNormalize(quat));
}

XMMATRIX CameraComponent::GetViewMatrix() const
{
    XMFLOAT3 eye = ComputeEye();
    XMVECTOR eyeV    = XMLoadFloat3(&eye);
    XMVECTOR targetV = XMLoadFloat3(&_target);
    XMVECTOR q       = XMLoadFloat4(&_orientation);
    XMVECTOR upV     = XMVector3Rotate(XMVectorSet(0, 1, 0, 0), q);
    return XMMatrixLookAtLH(eyeV, targetV, upV);
}

XMMATRIX CameraComponent::GetProjectionMatrix() const
{
    return XMMatrixPerspectiveFovLH(_fovRad, _aspect, _nearZ, _farZ);
}

XMFLOAT3 CameraComponent::GetEyePosition() const
{
    return ComputeEye();
}

XMFLOAT3 CameraComponent::ComputeEye() const
{
    XMVECTOR q      = XMLoadFloat4(&_orientation);
    XMVECTOR offset = XMVector3Rotate(XMVectorSet(0, 0, -_radius, 0), q);
    XMVECTOR tgt    = XMLoadFloat3(&_target);
    XMFLOAT3 eye;
    XMStoreFloat3(&eye, XMVectorAdd(tgt, offset));
    return eye;
}

} // namespace Luna

