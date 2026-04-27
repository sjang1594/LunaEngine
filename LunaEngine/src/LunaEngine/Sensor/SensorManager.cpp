#include "LunaPCH.h"
#include "Sensor/SensorManager.h"
#include "Sensor/SensorComponent.h"
#include "Manager/SceneManager.h"
#include "Scene/Scene.h"
#include "Components/GameObject.h"

namespace Luna
{

SensorManager* SensorManager::_instance = nullptr;

void SensorManager::Update(float dt)
{
    auto scene = SceneManager::GetInstance()->GetActiveScene();
    if (!scene) return;

    const auto& objects = scene->GetGameObjects();
    for (const auto& go : objects)
    {
        if (!go) continue;
        auto sensorComp = std::dynamic_pointer_cast<SensorComponent>(
            go->GetComponentByType(ComponentType::SENSOR));
        if (!sensorComp) continue;

        // SensorComponent::Update() ticks all attached sensors
        // We pass dt through the component's Update override
        // The component is already called via GameObject::Update(), but we
        // also support manual tick here for explicit control
        for (size_t i = 0; i < sensorComp->GetSensorCount(); i++)
        {
            auto* sensor = sensorComp->GetSensor(i);
            if (sensor && sensor->enabled)
            {
                auto transform = go->GetTransform();
                if (!transform) continue;
                XMFLOAT4X4 worldMat;
                XMStoreFloat4x4(&worldMat, transform->GetWorldMatrix());
                sensor->UpdateExtrinsicFromPosRot();
                sensor->Simulate(worldMat, dt);
            }
        }
    }
}

std::vector<ISensor*> SensorManager::GetAllSensors() const
{
    std::vector<ISensor*> result;
    auto scene = SceneManager::GetInstance()->GetActiveScene();
    if (!scene) return result;

    const auto& objects = scene->GetGameObjects();
    for (const auto& go : objects)
    {
        if (!go) continue;
        auto sensorComp = std::dynamic_pointer_cast<SensorComponent>(
            go->GetComponentByType(ComponentType::SENSOR));
        if (!sensorComp) continue;

        for (size_t i = 0; i < sensorComp->GetSensorCount(); i++)
        {
            auto* sensor = sensorComp->GetSensor(i);
            if (sensor) result.push_back(sensor);
        }
    }
    return result;
}

} // namespace Luna

