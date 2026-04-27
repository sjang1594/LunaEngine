#pragma once
#include "Components/Component.h"
#include "Sensor/ISensor.h"
#include "Sensor/CameraSensor.h"
#include "Sensor/LiDARSensor.h"
#include "Sensor/RadarSensor.h"
#include <vector>
#include <memory>
#include <string>

namespace Luna
{

// Component that holds multiple sensor instances attached to a GameObject.
class SensorComponent : public Component
{
public:
    SensorComponent();
    virtual ~SensorComponent() = default;

    void Update() override;

    // Add a sensor and return a raw pointer for immediate configuration
    ISensor* AddSensor(std::shared_ptr<ISensor> sensor);

    // Factory helpers
    CameraSensor* AddCameraSensor(const std::string& name = "");
    LiDARSensor*  AddLiDARSensor(const std::string& name = "");
    RadarSensor*  AddRadarSensor(const std::string& name = "");

    void RemoveSensor(size_t index);

    size_t GetSensorCount() const { return _sensors.size(); }
    ISensor* GetSensor(size_t index) { return index < _sensors.size() ? _sensors[index].get() : nullptr; }
    const std::vector<std::shared_ptr<ISensor>>& GetSensors() const { return _sensors; }

private:
    std::vector<std::shared_ptr<ISensor>> _sensors;
    uint32_t _cameraCounter = 0;
    uint32_t _lidarCounter  = 0;
    uint32_t _radarCounter  = 0;
};

} // namespace Luna

