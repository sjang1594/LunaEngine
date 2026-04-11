#include "LunaPCH.h"
#include "SceneManager.h"
#include "Scene/Scene.h"
#include "Components/GameObject.h"
#include "Components/MeshRenderer.h"
#include "Logger/Logger.h"
#include "LunaEngine/Renderer/HAL/Public/IRenderContext.h"
#include "LunaEngine/Renderer/DX12/Public/DX12Backend.h"

namespace Luna
{

// Out-of-line definition required for the static singleton pointer
SceneManager* SceneManager::_instance = nullptr;

void SceneManager::Update()
{
    if (_activeScene == nullptr)
        return;
    _activeScene->Update();
    _activeScene->LateUpdate();
}

void SceneManager::LoadScene(wstring sceneName)
{
    _activeScene = LoadTestScene();
    _activeScene->Awake();
    _activeScene->Start();
}

shared_ptr<Scene> SceneManager::LoadTestScene()
{
    auto scene = make_shared<Scene>();

    // Load glTF meshes via the DX12 backend (cast from the abstract IRenderContext backend).
    // If the backend is not DX12 or the file is missing, the scene starts empty
    // and the triangle fallback in DrawFrame() remains visible.
    auto* backend = IRenderContext::GetBackend();
    auto* dx12    = dynamic_cast<DX12Backend*>(backend);

    if (dx12)
    {
        auto meshes = dx12->LoadMeshes("Assets/DamagedHelmet.glb");
        if (meshes.empty())
        {
            LUNA_LOG_WARN("LoadTestScene: no meshes loaded — check Assets/DamagedHelmet.glb exists");
        }
        for (auto& mesh : meshes)
        {
            auto go = make_shared<GameObject>();
            go->Init();

            auto mr = make_shared<MeshRenderer>();
            mr->SetMesh(mesh);
            go->AddComponent(mr);

            scene->AddGameObject(go);
        }
    }
    else
    {
        LUNA_LOG_WARN("LoadTestScene: not running on DX12 backend — skipping mesh load");
    }

    return scene;
}

} // namespace Luna
