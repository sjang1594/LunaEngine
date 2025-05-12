#include "LunaPCH.h"
#include "SceneManager.h"
#include "Scene/Scene.h"

namespace Luna
{

void SceneManager::Update()
{
    if (_activeScene == nullptr)
        return;
    _activeScene->Update();
    _activeScene->LateUpdate();
}

void SceneManager::LoadScene(wstring sceneName)
{
    // previous scene clear()
    _activeScene = LoadTestScene();
    _activeScene->Awake();
    _activeScene->Start();
}

shared_ptr<Scene> SceneManager::LoadTestScene(){
}
}