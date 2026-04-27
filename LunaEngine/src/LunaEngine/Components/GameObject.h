#pragma once
#include "Transform.h"
#include "Components/Component.h"

class Transform;
class MeshRenderer;

namespace Luna
{
// UNITY Style
class GameObject : public std::enable_shared_from_this<GameObject>
{
public:
    GameObject();
    virtual ~GameObject();

    void Init();

    void Awake();
    void Start();
    void Update();
    void LateUpdate();

    shared_ptr<Transform> GetTransform() const;
    shared_ptr<Component> GetComponentByType(ComponentType type) const
    {
        uint8_t idx = static_cast<uint8_t>(type);
        if (idx < FIXED_COMPONENT_COUNT) return _components[idx];
        return nullptr;
    }
    void AddComponent(shared_ptr<Component> shared);
    
private:
    array<shared_ptr<Component>, FIXED_COMPONENT_COUNT> _components;
};
}