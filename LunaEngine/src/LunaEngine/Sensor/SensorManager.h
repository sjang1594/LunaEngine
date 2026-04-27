#pragma once
#include <vector>
#include <memory>

namespace Luna
{
class ISensor;
class SensorComponent;

// Singleton that ticks all sensors across all GameObjects in the active scene.
class SensorManager
{
    SensorManager() = default;
    ~SensorManager() = default;
    static SensorManager* _instance;

public:
    static SensorManager* GetInstance()
    {
        if (!_instance) _instance = new SensorManager();
        return _instance;
    }

    // Called each frame from Application::Run() — iterates active scene's SensorComponents
    void Update(float dt);

    // Convenience: collect all sensors of a given type from the active scene
    std::vector<ISensor*> GetAllSensors() const;
};

} // namespace Luna

