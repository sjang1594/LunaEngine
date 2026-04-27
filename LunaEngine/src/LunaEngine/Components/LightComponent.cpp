#include "LunaPCH.h"
#include "LightComponent.h"

namespace Luna
{

LightComponent::LightComponent()
    : Component(ComponentType::LIGHT)
{
    // Default: same as the old hardcoded normalize(1,2,1)
    XMVECTOR v = XMVector3Normalize(XMVectorSet(1.0f, 2.0f, 1.0f, 0.0f));
    XMStoreFloat3(&_direction, v);
}

void LightComponent::SetDirection(const XMFLOAT3& dir)
{
    XMVECTOR v = XMVector3Normalize(XMLoadFloat3(&dir));
    XMStoreFloat3(&_direction, v);
}

} // namespace Luna

