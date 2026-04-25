#include "LunaPCH.h"
#include "Scene/Scene.h"
#include "Components/GameObject.h"
#include "Components/CameraComponent.h"
#include "Components/LightComponent.h"

namespace Luna
{
// ...existing code...

void Scene::Awake()
{
    for (const shared_ptr<GameObject>& gameObject : _gameObjects)
    {
        gameObject->Awake();
    }
}

void Scene::Start()
{
    for (const shared_ptr<GameObject>& gameObject : _gameObjects)
    {
        gameObject->Start();
    }
}

void Scene::Update()
{
    for (const shared_ptr<GameObject>& gameObject : _gameObjects)
    {
        gameObject->Update();
    }
}

void Scene::LateUpdate()
{
    for (const shared_ptr<GameObject>& gameObject : _gameObjects)
    {
        gameObject->LateUpdate();
    }
}

void Scene::AddGameObject(shared_ptr<GameObject> gameObject)
{
    _gameObjects.push_back(gameObject);
}

void Scene::RemoveGameObject(shared_ptr<GameObject> gameObject)
{
    auto findIt = std::find(_gameObjects.begin(), _gameObjects.end(), gameObject);
    if (findIt != _gameObjects.end())
    {
        _gameObjects.erase(findIt);
    }
}

shared_ptr<CameraComponent> Scene::GetMainCamera() const
{
    for (const auto& go : _gameObjects)
    {
        auto comp = go->GetComponentByType(ComponentType::CAMERA);
        if (comp) return std::static_pointer_cast<CameraComponent>(comp);
    }
    return nullptr;
}

shared_ptr<LightComponent> Scene::GetDirectionalLight() const
{
    for (const auto& go : _gameObjects)
    {
        auto comp = go->GetComponentByType(ComponentType::LIGHT);
        if (comp) return std::static_pointer_cast<LightComponent>(comp);
    }
    return nullptr;
}

}