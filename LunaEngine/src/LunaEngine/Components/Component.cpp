#include "LunaPCH.h"
#include "Component.h"
#include "GameObject.h"

namespace Luna
{

Component::Component(ComponentType type) : _componentType(type)
{
}

shared_ptr<GameObject> Component::GetGameObject() const
{
    return _gameObject.lock();
}

shared_ptr<Transform> Component::GetTransform() const
{
    return _gameObject.lock()->GetTransform();
}
}