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
    for (shared_ptr<Component> component : _components)
    {
        if (component)
            component->Start();
    }
}

void GameObject::Update()
{
    for (shared_ptr<Component> component : _components)
    {
        if (component)
            component->Update();
    }
}

void GameObject::LateUpdate()
{
    for (shared_ptr<Component> component : _components)
    {
        if (component)
            component->LateUpdate();
    }
}

void GameObject::AddComponent(shared_ptr<Component> component)
{
    component->SetGameObject(shared_from_this());
    uint8_t index = static_cast<uint8_t>(component->GetComponentType());
    if (index < FIXED_COMPONENT_COUNT)
    {
        _components[index] = component;
    }
    else
    {
        // _scripts.push_back(dynamic_pointer_cast<Monobehaviour>(component);
    }
}
}