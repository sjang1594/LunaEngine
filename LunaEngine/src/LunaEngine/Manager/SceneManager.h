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

    shared_ptr<Scene> GetActiveScene() { return _activeScene; }
private:
    shared_ptr<Scene> _activeScene;
    shared_ptr<Scene> LoadTestScene(); 
};
}
