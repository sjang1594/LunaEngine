#pragma once

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
    void LoadScene(wstring sceneName);

    // Release the active scene before backend shutdown so D3D12MA allocations
    // in Mesh objects are freed while the allocator is still alive.
    void ResetActiveScene() { _activeScene.reset(); }

    shared_ptr<Scene> GetActiveScene() { return _activeScene; }
private:
    shared_ptr<Scene> _activeScene;
    shared_ptr<Scene> LoadTestScene(); 
};
}
