#pragma once
#include <string>
#include <memory>
#include "Loader/NuScenesLoader.h"

namespace Luna
{
class Scene;
class SceneManager
{
    SceneManager(){} 
    ~SceneManager() {}
    static SceneManager* _instance;

public:
    static SceneManager* GetInstance()
    {
        if (_instance == nullptr)
            _instance = new SceneManager();
        return _instance;
    }

    void Update();
    void LoadScene(std::wstring sceneName);

    // Phase 21: set the glTF asset to load (relative to Assets/ directory)
    void SetSceneAsset(const std::string& assetName) { _sceneAsset = assetName; }

    // Phase 21: load a scene from an absolute file path at runtime (File > Import Scene)
    void LoadSceneFromFile(const std::string& absolutePath);

    // nuScenes: load one sample (annotation boxes + reference LiDAR scan + sensor calibration)
    void LoadNuScenesScene(const std::string& dataRoot,
                           const std::string& sceneToken,
                           int                sampleIndex);

    // nuScenes: access loaded data for rendering/UI
    NuScenesLoader&       GetNuScenesLoader()       { return _nsLoader; }
    const NuScenesLoader::SampleData& GetNuScenesSample() const { return _nsSample; }
    bool HasNuScenesSample() const { return _hasNsSample; }

    // Release the active scene before backend shutdown so D3D12MA allocations
    // in Mesh objects are freed while the allocator is still alive.
    void ResetActiveScene() { _activeScene.reset(); }

    std::shared_ptr<Scene> GetActiveScene() { return _activeScene; }
private:
    std::shared_ptr<Scene> _activeScene;
    std::string _sceneAsset;  // empty = empty scene; set via --scene CLI or SetSceneAsset()
    std::shared_ptr<Scene> LoadTestScene();

    NuScenesLoader            _nsLoader;
    NuScenesLoader::SampleData _nsSample;
    bool                       _hasNsSample = false;
};
}
