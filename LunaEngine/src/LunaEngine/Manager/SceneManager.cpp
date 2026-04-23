#include "LunaPCH.h"
#include "SceneManager.h"
#include "Scene/Scene.h"
#include "Components/GameObject.h"
#include "Components/MeshRenderer.h"
#include "Logger/Logger.h"
#include "LunaEngine/Renderer/HAL/Public/IRenderContext.h"
#include "LunaEngine/utils/FileSystemUtil.h"

// Set to 1 to load a procedural flat quad instead of DamagedHelmet (rendering pipeline debug)
#define LUNA_DEBUG_QUAD 0

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

    // Load glTF meshes via the active backend (polymorphic call via IRenderBackend)
    auto* backend = IRenderContext::GetBackend();
    if (!backend)
    {
        LUNA_LOG_ERROR("LoadTestScene: no render backend available");
        return scene;
    }

    LUNA_LOG_INFO("Loading meshes via %s backend", backend->GetBackendName());
#if LUNA_DEBUG_QUAD
    auto meshes = backend->LoadDebugQuad();
#else
    std::string assetPath = GetAssetPath("Assets/DamagedHelmet.glb").string();
    auto meshes = backend->LoadMeshes(assetPath);
#endif

    // Phase 14: load HDR environment for IBL
    std::string hdrPath = GetAssetPath("Assets/environment.hdr").string();
    if (backend->LoadHDREnvironment(hdrPath))
        LUNA_LOG_INFO("IBL environment loaded");
    else
        LUNA_LOG_WARN("IBL environment not loaded — check '%s' exists (IBL disabled)", hdrPath.c_str());

    if (meshes.empty())
    {
#if LUNA_DEBUG_QUAD
        LUNA_LOG_WARN("LoadTestScene: LoadDebugQuad returned no meshes");
#else
        LUNA_LOG_WARN("LoadTestScene: no meshes loaded -- check '%s' exists", assetPath.c_str());
#endif
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

    LUNA_LOG_INFO("LoadTestScene: created %zu game objects", meshes.size());
    return scene;
}

} // namespace Luna
