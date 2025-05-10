#pragma once
#include "Transform.h"
#include "Components/Component.h"

class Transform;
class MeshRenderer;

namespace Luna
{
class GameObject : public enable_shared_from_this<GameObject>
{

public:
    GameObject();
    virtual ~GameObject();

    void Init();
    void Awake();
    void Start();
    void Update();
    void LastUpdate();

    shared_ptr<Transform> GetTransform() const;
    void AddComponent(const shared_ptr<Transform> & shared);
    
private:
    array<shared_ptr<Component>, FIXED_COMPONENT_COUNT> _components;
};
}