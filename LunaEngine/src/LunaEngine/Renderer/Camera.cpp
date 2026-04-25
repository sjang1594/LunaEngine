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
    XMVECTOR q = XMQuaternionRotationAxis(XMVectorSet(1, 0, 0, 0), XMConvertToRadians(-30.0f));
    XMStoreFloat4(&_orientation, XMQuaternionNormalize(q));
}

void Camera::SetAspect(float aspect)
{
    _aspect = aspect;
}

void Camera::Orbit(float dYaw, float dPitch)
{
    XMVECTOR q = XMLoadFloat4(&_orientation);

    XMVECTOR qYaw = XMQuaternionRotationAxis(
        XMVectorSet(0, 1, 0, 0), XMConvertToRadians(dYaw));
    q = XMQuaternionMultiply(qYaw, q);

    XMVECTOR right = XMVector3Rotate(XMVectorSet(1, 0, 0, 0), q);
    XMVECTOR qPitch = XMQuaternionRotationAxis(right, XMConvertToRadians(-dPitch));
    q = XMQuaternionNormalize(XMQuaternionMultiply(qPitch, q));

    XMStoreFloat4(&_orientation, q);
}

void Camera::Zoom(float delta)
{
    _radius -= delta;
    if (_radius < 0.5f) _radius = 0.5f;
    if (_radius > 1000.0f) _radius = 1000.0f;
}

void Camera::SetOrientation(XMVECTOR quat)
{
    XMStoreFloat4(&_orientation, XMQuaternionNormalize(quat));
}

XMMATRIX Camera::GetViewMatrix() const
{
    XMFLOAT3 eye = ComputeEye();
    XMVECTOR eyeV    = XMLoadFloat3(&eye);
    XMVECTOR targetV = XMLoadFloat3(&_target);

    XMVECTOR q = XMLoadFloat4(&_orientation);
    XMVECTOR upV = XMVector3Rotate(XMVectorSet(0, 1, 0, 0), q);

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

XMFLOAT3 Camera::ComputeEye() const
{
    XMVECTOR q = XMLoadFloat4(&_orientation);
    XMVECTOR offset = XMVector3Rotate(XMVectorSet(0, 0, -_radius, 0), q);
    XMVECTOR targetV = XMLoadFloat3(&_target);
    XMFLOAT3 eye;
    XMStoreFloat3(&eye, XMVectorAdd(targetV, offset));
    return eye;
}

} // namespace Luna
