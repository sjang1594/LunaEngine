#pragma once
#include "Components/Component.h"

namespace Luna
{

enum class LightType { Directional, Point };

class LightComponent : public Component
{
public:
    LightComponent();
    virtual ~LightComponent() = default;

    XMFLOAT3 GetDirection() const { return _direction; }
    void     SetDirection(const XMFLOAT3& dir);

    LightType type      = LightType::Directional;
    XMFLOAT3  color     = {1.0f, 1.0f, 1.0f};
    float     intensity = 3.0f;

private:
    XMFLOAT3 _direction = {0.4082f, 0.8165f, 0.4082f};
};

} // namespace Luna

