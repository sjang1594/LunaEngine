#pragma once
#include "Components/Component.h"

namespace Luna
{
class Transform : public Component
{
public:
    Transform();
    virtual ~Transform();

    XMFLOAT3 position = {0.f, 0.f, 0.f};
    XMFLOAT3 rotation = {0.f, 0.f, 0.f}; // degrees
    XMFLOAT3 scale    = {1.f, 1.f, 1.f};

    bool       useRawMatrix = false;
    XMFLOAT4X4 rawMatrix;

    void SetWorldMatrix(const XMFLOAT4X4& mat) { rawMatrix = mat; useRawMatrix = true; }

    XMMATRIX GetWorldMatrix() const;
};
}
