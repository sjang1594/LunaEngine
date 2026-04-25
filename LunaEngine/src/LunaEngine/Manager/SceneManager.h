#pragma once
#include <string>
#include <memory>

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

    void SetSceneAsset(const std::string& assetName) { _sceneAsset = assetName; }
    void LoadSceneFromFile(const std::string& absolutePath);

    // Release the active scene before backend shutdown so D3D12MA allocations
    // in Mesh objects are freed while the allocator is still alive.
    void ResetActiveScene() { _activeScene.reset(); }

    std::shared_ptr<Scene> GetActiveScene() { return _activeScene; }
private:
    std::shared_ptr<Scene> _activeScene;
    std::string _sceneAsset = "DamagedHelmet.glb";  // default asset
    std::shared_ptr<Scene> LoadTestScene(); 
};
}
