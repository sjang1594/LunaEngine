#pragma once
#include "ISensor.h"
#include <vector>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace Luna
{

// LiDAR sensor: generates point cloud via raycasting.
// Phase 1: config + ray direction generation. GPU raycasting added in Phase 3.
class LiDARSensor : public ISensor
{
public:
    LiDARSensorConfig config;

    // Output
    std::vector<LiDARPoint> pointCloud;

    LiDARSensor(const std::string& name = "LiDAR0")
        : ISensor(SensorType::LiDAR, name) {}

    void Simulate(const DirectX::XMFLOAT4X4& ownerWorld, float dt) override
    {
        if (!enabled || !ShouldTick(dt)) return;

        using namespace DirectX;
        XMMATRIX owner = XMLoadFloat4x4(&ownerWorld);
        XMMATRIX ext   = XMLoadFloat4x4(&_extrinsic);
        XMMATRIX sensorWorld = ext * owner;
        XMStoreFloat4x4(&_sensorWorldMatrix, sensorWorld);

        // Generate ray directions in sensor-local space
        GenerateRayDirections();

        // TODO Phase 3: dispatch GPU raycasting against scene acceleration structure
        // For now, clear output
        pointCloud.clear();
    }

    const DirectX::XMFLOAT4X4& GetSensorWorld() const { return _sensorWorldMatrix; }
    const std::vector<DirectX::XMFLOAT3>& GetRayDirections() const { return _rayDirections; }

    uint32_t GetNumRays() const { return static_cast<uint32_t>(_rayDirections.size()); }

private:
    DirectX::XMFLOAT4X4 _sensorWorldMatrix;
    std::vector<DirectX::XMFLOAT3> _rayDirections;
    bool _raysGenerated = false;

    void GenerateRayDirections()
    {
        if (_raysGenerated) return; // Only regenerate if config changes

        _rayDirections.clear();

        float hHalf = config.hFovDeg * 0.5f;
        float vHalf = config.vFovDeg * 0.5f;

        for (float el = -vHalf; el <= vHalf; el += config.vResolutionDeg)
        {
            for (float az = -hHalf; az < hHalf; az += config.hResolutionDeg)
            {
                float azRad = static_cast<float>(az * M_PI / 180.0);
                float elRad = static_cast<float>(el * M_PI / 180.0);
                DirectX::XMFLOAT3 dir;
                dir.x = cosf(elRad) * sinf(azRad);
                dir.y = sinf(elRad);
                dir.z = cosf(elRad) * cosf(azRad);
                _rayDirections.push_back(dir);
            }
        }
        _raysGenerated = true;
    }

public:
    // Call when config changes to force ray regeneration
    void InvalidateRays() { _raysGenerated = false; }
};

} // namespace Luna

