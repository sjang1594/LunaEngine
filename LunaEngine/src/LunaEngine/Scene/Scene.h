#pragma once

namespace Luna
{
class GameObject;
class CameraComponent;
class LightComponent;

class Scene
{
public:
    void Awake();
    void Start();
    void Update();
    void LateUpdate();

    void AddGameObject(shared_ptr<GameObject> gameObject);
    void RemoveGameObject(shared_ptr<GameObject> gameObject);

    const vector<shared_ptr<GameObject>>& GetGameObjects() const { return _gameObjects; }

    // Find the first GameObject with a CameraComponent / LightComponent
    shared_ptr<CameraComponent> GetMainCamera() const;
    shared_ptr<LightComponent>  GetDirectionalLight() const;

private:
    vector<shared_ptr<GameObject>> _gameObjects;
};
}
