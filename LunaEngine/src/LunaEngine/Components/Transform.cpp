#include "LunaPCH.h"
#include "Transform.h"

namespace Luna
{
Transform::Transform() : Component(ComponentType::TRANSFORM) {}

Transform::~Transform() {}

XMMATRIX Transform::GetWorldMatrix() const
{
    XMMATRIX S = XMMatrixScaling(scale.x, scale.y, scale.z);
    XMMATRIX R = XMMatrixRotationRollPitchYaw(
        XMConvertToRadians(rotation.x),
        XMConvertToRadians(rotation.y),
        XMConvertToRadians(rotation.z));
    XMMATRIX T = XMMatrixTranslation(position.x, position.y, position.z);
    return S * R * T;
}
} // namespace Luna
