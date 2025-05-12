#pragma once

namespace Luna
{
class GameObject;
class Transform;

enum class ComponentType
{
    NONE,
    MESH_RENDERER,
    TRANSFORM,
    END
};

enum
{
    FIXED_COMPONENT_COUNT = static_cast<uint8_t>(ComponentType::END) - 1
};

class Component
{
public:
    Component(ComponentType type);
    virtual ~Component() = default;
    virtual void Awake() {}
    virtual void Start() {}
    virtual void Update() {}
    virtual void LateUpdate() {}
    
    ComponentType GetComponentType() const;
    bool IsValid() { return _gameObject.expired() == false; }
    shared_ptr<GameObject> GetGameObject() const;
    shared_ptr<Transform> GetTransform() const;
    
protected:
    ComponentType _componentType;
    weak_ptr<GameObject> _gameObject;

private:
    friend class GameObject;
    void SetGameObject(const shared_ptr<GameObject> &gameObject) { _gameObject = gameObject; }
};
}