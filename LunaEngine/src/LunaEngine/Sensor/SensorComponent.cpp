#include "LunaPCH.h"
#include "Sensor/SensorComponent.h"
#include "Components/Transform.h"

namespace Luna
{

SensorComponent::SensorComponent()
    : Component(ComponentType::SENSOR)
{
}

void SensorComponent::Update()
{
    auto transform = GetTransform();
    if (!transform) return;

    XMFLOAT4X4 worldMat;
    XMStoreFloat4x4(&worldMat, transform->GetWorldMatrix());

    for (auto& sensor : _sensors)
    {
        if (sensor && sensor->enabled)
        {
            sensor->UpdateExtrinsicFromPosRot();
            sensor->Simulate(worldMat, 0.016f); // TODO: pass actual dt
        }
    }
}

ISensor* SensorComponent::AddSensor(std::shared_ptr<ISensor> sensor)
{
    _sensors.push_back(sensor);
    return sensor.get();
}

CameraSensor* SensorComponent::AddCameraSensor(const std::string& name)
{
    std::string n = name.empty() ? "Camera" + std::to_string(_cameraCounter++) : name;
    auto sensor = std::make_shared<CameraSensor>(n);
    _sensors.push_back(sensor);
    return sensor.get();
}

LiDARSensor* SensorComponent::AddLiDARSensor(const std::string& name)
{
    std::string n = name.empty() ? "LiDAR" + std::to_string(_lidarCounter++) : name;
    auto sensor = std::make_shared<LiDARSensor>(n);
    _sensors.push_back(sensor);
    return sensor.get();
}

RadarSensor* SensorComponent::AddRadarSensor(const std::string& name)
{
    std::string n = name.empty() ? "Radar" + std::to_string(_radarCounter++) : name;
    auto sensor = std::make_shared<RadarSensor>(n);
    _sensors.push_back(sensor);
    return sensor.get();
}

void SensorComponent::RemoveSensor(size_t index)
{
    if (index < _sensors.size())
        _sensors.erase(_sensors.begin() + index);
}

} // namespace Luna

