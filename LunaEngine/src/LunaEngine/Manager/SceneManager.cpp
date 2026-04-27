#include "LunaPCH.h"
#include "SceneManager.h"
#include "Scene/Scene.h"
#include "Components/GameObject.h"
#include "Components/Transform.h"
#include "Components/MeshRenderer.h"
#include "Components/CameraComponent.h"
#include "Components/LightComponent.h"
#include "Logger/Logger.h"
#include "LunaEngine/Renderer/HAL/Public/IRenderContext.h"
#include "LunaEngine/utils/FileSystemUtil.h"

// Set to 1 to load a procedural flat quad instead of glTF (rendering pipeline debug)
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

void SceneManager::LoadScene(std::wstring sceneName)
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
    std::string assetPath = GetAssetPath("Assets/" + _sceneAsset).string();
    LUNA_LOG_INFO("Scene asset: %s", assetPath.c_str());
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

    // Phase 21: retrieve per-mesh transforms from the backend (if available)
    auto transforms = backend->GetLastLoadTransforms();

    for (size_t i = 0; i < meshes.size(); ++i)
    {
        auto go = make_shared<GameObject>();
        go->Init();

        // Apply per-mesh transform from glTF node hierarchy
        if (i < transforms.size())
        {
            auto transform = go->GetTransform();
            if (transform)
                transform->SetWorldMatrix(transforms[i]);
        }

        auto mr = make_shared<MeshRenderer>();
        mr->SetMesh(meshes[i]);
        go->AddComponent(mr);

        scene->AddGameObject(go);
    }

    LUNA_LOG_INFO("LoadTestScene: created %zu game objects", meshes.size());

    // --- Main Camera (Isaac Sim-style: camera as scene object) ---
    {
        auto camGO = make_shared<GameObject>();
        camGO->Init();
        auto cam = make_shared<CameraComponent>();
        camGO->AddComponent(cam);
        scene->AddGameObject(camGO);
        LUNA_LOG_INFO("Scene: added Main Camera");
    }

    // --- Directional Light (scene object, replaces hardcoded light dir) ---
    {
        auto lightGO = make_shared<GameObject>();
        lightGO->Init();
        auto light = make_shared<LightComponent>();
        // Default direction = normalize(1,2,1), same as old hardcoded value
        lightGO->AddComponent(light);
        scene->AddGameObject(lightGO);
        LUNA_LOG_INFO("Scene: added Directional Light");
    }

    return scene;
}

void SceneManager::LoadSceneFromFile(const std::string& absolutePath)
{
    auto* backend = IRenderContext::GetBackend();
    if (!backend)
    {
        LUNA_LOG_ERROR("LoadSceneFromFile: no render backend available");
        return;
    }

    LUNA_LOG_INFO("Importing scene: %s", absolutePath.c_str());

    // Reset current scene (releases GPU resources while backend is alive)
    if (_activeScene)
        _activeScene.reset();

    auto scene = make_shared<Scene>();

    auto meshes = backend->LoadMeshes(absolutePath);
    if (meshes.empty())
    {
        LUNA_LOG_WARN("LoadSceneFromFile: no meshes loaded from '%s'", absolutePath.c_str());
        _activeScene = scene;
        return;
    }

    auto transforms = backend->GetLastLoadTransforms();

    for (size_t i = 0; i < meshes.size(); ++i)
    {
        auto go = make_shared<GameObject>();
        go->Init();

        if (i < transforms.size())
        {
            auto transform = go->GetTransform();
            if (transform)
                transform->SetWorldMatrix(transforms[i]);
        }

        auto mr = make_shared<MeshRenderer>();
        mr->SetMesh(meshes[i]);
        go->AddComponent(mr);

        scene->AddGameObject(go);
    }

    _activeScene = scene;
    _activeScene->Awake();
    _activeScene->Start();

    // Add Camera + Light to imported scenes too
    {
        auto camGO = make_shared<GameObject>();
        camGO->Init();
        camGO->AddComponent(make_shared<CameraComponent>());
        scene->AddGameObject(camGO);
    }
    {
        auto lightGO = make_shared<GameObject>();
        lightGO->Init();
        lightGO->AddComponent(make_shared<LightComponent>());
        scene->AddGameObject(lightGO);
    }

    LUNA_LOG_INFO("LoadSceneFromFile: created %zu game objects from '%s'", meshes.size(), absolutePath.c_str());
}

} // namespace Luna
