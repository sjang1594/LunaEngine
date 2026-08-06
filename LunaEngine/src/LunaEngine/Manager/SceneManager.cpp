#include "LunaPCH.h"
#include "SceneManager.h"
#include "Sensor/SensorComponent.h"
#include "Sensor/CameraSensor.h"
#include "Sensor/LiDARSensor.h"
#include "Sensor/SensorTypes.h"
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

    auto* backend = IRenderContext::GetBackend();
    if (!backend)
    {
        LUNA_LOG_ERROR("LoadTestScene: no render backend available");
        return scene;
    }

    // Load glTF scene if one was specified via --scene CLI argument
    if (!_sceneAsset.empty())
    {
        std::string assetPath = GetAssetPath("Assets/" + _sceneAsset).string();
        LUNA_LOG_INFO("Scene asset: %s", assetPath.c_str());
        auto meshes = backend->LoadMeshes(assetPath);
        auto transforms = backend->GetLastLoadTransforms();
        for (size_t i = 0; i < meshes.size(); ++i)
        {
            auto go = make_shared<GameObject>(); go->Init();
            if (i < transforms.size())
                if (auto t = go->GetTransform()) t->SetWorldMatrix(transforms[i]);
            auto mr = make_shared<MeshRenderer>(); mr->SetMesh(meshes[i]);
            go->AddComponent(mr);
            scene->AddGameObject(go);
        }
    }

    // IBL environment (optional — silently skipped if missing)
    std::string hdrPath = GetAssetPath("Assets/environment.hdr").string();
    if (backend->LoadHDREnvironment(hdrPath))
        LUNA_LOG_INFO("IBL environment loaded");

    // Main Camera
    {
        auto go = make_shared<GameObject>(); go->Init();
        go->AddComponent(make_shared<CameraComponent>());
        scene->AddGameObject(go);
    }
    // Directional Light
    {
        auto go = make_shared<GameObject>(); go->Init();
        go->AddComponent(make_shared<LightComponent>());
        scene->AddGameObject(go);
    }
    LUNA_LOG_INFO("LoadTestScene: empty scene ready — use File > Load nuScenes Sample");
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

// ── nuScenes scene loader ────────────────────────────────────────────────────
void SceneManager::LoadNuScenesScene(const std::string& dataRoot,
                                      const std::string& sceneToken,
                                      int                sampleIndex)
{
    auto* backend = IRenderContext::GetBackend();
    if (!backend) { LUNA_LOG_ERROR("LoadNuScenesScene: no backend"); return; }

    // Load nuScenes JSON tables
    if (!_nsLoader.IsLoaded() || dataRoot != _nsLoader.GetDataRoot())
    {
        if (!_nsLoader.Load(dataRoot))
        {
            LUNA_LOG_ERROR("LoadNuScenesScene: failed to load JSON from %s", dataRoot.c_str());
            return;
        }
    }

    _hasNsSample = _nsLoader.LoadSample(sceneToken, sampleIndex, _nsSample);
    if (!_hasNsSample) { LUNA_LOG_ERROR("LoadNuScenesScene: LoadSample failed"); return; }

    if (_activeScene) _activeScene.reset();
    auto scene = make_shared<Scene>();

    // ── Compute all instance world matrices ───────────────────────────────────
    // One oriented box per annotation — no ground plane (nuScenes road is non-flat;
    // real LiDAR scan in _nsSample.lidarPoints serves as ground truth reference).
    using namespace DirectX;

    std::vector<XMFLOAT4X4> instanceTransforms;

    // Annotation boxes only
    for (auto& box : _nsSample.annotations)
    {
        XMVECTOR q = XMVectorSet(box.rotation.x, box.rotation.y, box.rotation.z, box.rotation.w);
        XMMATRIX M = XMMatrixScaling(box.size.x, box.size.y, box.size.z)
                   * XMMatrixRotationQuaternion(q)
                   * XMMatrixTranslation(box.translation.x, box.translation.y, box.translation.z);
        XMFLOAT4X4 world; XMStoreFloat4x4(&world, M);
        instanceTransforms.push_back(world);
    }

    // Annotation boxes are rendered as ImGui OBB wireframes (DrawAnnotationBoxes),
    // so no solid mesh or GPU-driven pipeline is needed for them.
    LUNA_LOG_INFO("LoadNuScenesScene: %zu annotations (wireframe overlay)", instanceTransforms.size());

    // ── SensorRig: LIDAR_TOP at nuScenes calibrated position ─────────────────
    {
        auto rigGO = make_shared<GameObject>(); rigGO->Init();
        XMFLOAT4X4 identity; XMStoreFloat4x4(&identity, XMMatrixIdentity());
        if (auto t = rigGO->GetTransform()) t->SetWorldMatrix(identity);

        auto sensorComp = make_shared<SensorComponent>(); rigGO->AddComponent(sensorComp);

        auto lidar = make_shared<LiDARSensor>("LIDAR_TOP");
        lidar->config            = LiDARSensorConfig::MakeHDL32E();
        lidar->extrinsicPosition = _nsSample.lidarPosition;
        lidar->tickIntervalSec   = 1.0f / 10.0f;
        lidar->UpdateExtrinsicFromPosRot();
        lidar->Simulate(identity, lidar->tickIntervalSec + 0.001f);

        // Feed the recorded scan into the sensor's point cloud.
        //
        // Without this the 34k points loaded from the .pcd.bin stayed in _nsSample and
        // never reached the renderer: LiDARSensor::pointCloud is otherwise only filled by
        // the GPU raycast readback, which needs a TLAS. An empty scene therefore swallowed
        // real sensor data. Recorded points are measurements, not simulation output — they
        // must render regardless of whether there is any geometry to raycast against.
        if (_nsSample.lidarLoaded && !_nsSample.lidarPoints.empty())
        {
            lidar->pointCloud.clear();
            lidar->pointCloud.reserve(_nsSample.lidarPoints.size());
            for (const auto& src : _nsSample.lidarPoints)
            {
                LiDARPoint p{};
                p.position    = src.position;
                p.intensity   = src.intensity;
                p.returnIndex = 0;          // recorded scan carries no multi-return index
                lidar->pointCloud.push_back(p);
            }
            LUNA_LOG_INFO("LoadNuScenesScene: bound %zu recorded points to '%s'",
                          lidar->pointCloud.size(), lidar->GetName().c_str());
        }

        sensorComp->AddSensor(lidar);

        scene->AddGameObject(rigGO);
        LUNA_LOG_INFO("LoadNuScenesScene: LIDAR_TOP at (%.2f, %.2f, %.2f)",
                      _nsSample.lidarPosition.x, _nsSample.lidarPosition.y, _nsSample.lidarPosition.z);
    }

    { auto g = make_shared<GameObject>(); g->Init(); g->AddComponent(make_shared<CameraComponent>()); scene->AddGameObject(g); }
    { auto g = make_shared<GameObject>(); g->Init(); g->AddComponent(make_shared<LightComponent>());  scene->AddGameObject(g); }

    _activeScene = scene;
    _activeScene->Awake();
    _activeScene->Start();
    LUNA_LOG_INFO("LoadNuScenesScene: ready (lidar scan: %s)",
                  _nsSample.lidarLoaded ? "loaded" : "missing");
}

} // namespace Luna
