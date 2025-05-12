#pragma once

namespace Luna
{
class GameObject;

class Scene
{
public:
    void Awake();
    void Start();
    void Update();
    void LateUpdate();

    void AddGameObject(shared_ptr<GameObject> gameObject);
    void RemoveGameObject(shared_ptr<GameObject> gameObject);

private:
    // TODO: Layer (0 ~ ) Hash
    vector<shared_ptr<GameObject>> _gameObjects;
};
}
