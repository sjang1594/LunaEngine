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

    // Output: populated by DX12Backend::RenderLiDARSensors() after GPU readback
    std::vector<LiDARPoint> pointCloud;

    // S3: GPU handle (set by backend after resource init — opaque to sensor layer)
    uint64_t gpuResourceHandle = 0;

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

        // S3: GPU raycasting dispatched by DX12Backend::RenderLiDARSensors()
        // Ray directions are re-generated whenever config changes (InvalidateRays() called)
        // pointCloud is populated after GPU readback
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
        if (_raysGenerated) return;
        _rayDirections.clear();

        float hHalf = config.hFovDeg * 0.5f;

        // S3: use per-beam elevations when provided (e.g. HDL-32E datasheet angles)
        if (!config.beamElevationsDeg.empty())
        {
            for (float elDeg : config.beamElevationsDeg)
            {
                float elRad = static_cast<float>(elDeg * M_PI / 180.0);
                for (float az = -hHalf; az < hHalf; az += config.hResolutionDeg)
                {
                    float azRad = static_cast<float>(az * M_PI / 180.0);
                    DirectX::XMFLOAT3 dir;
                    dir.x = cosf(elRad) * sinf(azRad);
                    dir.y = sinf(elRad);
                    dir.z = cosf(elRad) * cosf(azRad);
                    _rayDirections.push_back(dir);
                }
            }
        }
        else
        {
            // Uniform elevation grid (legacy / solid-state fallback)
            float vHalf = config.vFovDeg * 0.5f;
            for (float el = -vHalf; el <= vHalf; el += config.vResolutionDeg)
            {
                float elRad = static_cast<float>(el * M_PI / 180.0);
                for (float az = -hHalf; az < hHalf; az += config.hResolutionDeg)
                {
                    float azRad = static_cast<float>(az * M_PI / 180.0);
                    DirectX::XMFLOAT3 dir;
                    dir.x = cosf(elRad) * sinf(azRad);
                    dir.y = sinf(elRad);
                    dir.z = cosf(elRad) * cosf(azRad);
                    _rayDirections.push_back(dir);
                }
            }
        }
        _raysGenerated = true;
    }

public:
    // Force immediate ray regeneration (bypasses tick rate — for GPU resource init)
    void ForceGenerateRays() { _raysGenerated = false; GenerateRayDirections(); }
    // Call when config changes to force ray regeneration on next tick
    void InvalidateRays() { _raysGenerated = false; }
};

} // namespace Luna

