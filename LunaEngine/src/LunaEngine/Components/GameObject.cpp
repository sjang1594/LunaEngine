#include "LunaPCH.h"
#include "GameObject.h"
#include "Transform.h"

namespace Luna
{

GameObject::GameObject()
{
     
}

GameObject::~GameObject()
{
}

void GameObject::Init()
{
    AddComponent(make_shared<Transform>());
}

void GameObject::Awake()
{
    for (shared_ptr<Component> component : _components) {
        if (component)
            component->Awake();
    }
}

void GameObject::Start()
{
    
}

void GameObject::AddComponent(const shared_ptr<Transform> &component)
{
    component->SetGameObject(shared_from_this());
    uint8 index = static_cast<uint8>(component->GetComponentType());
    if (index < FIXED_COMPONENT_COUNT)
    {
        _components[index] = component;
    }
}

void GameObject::AddComponent(shared_ptr<Component> component)
{
}


}