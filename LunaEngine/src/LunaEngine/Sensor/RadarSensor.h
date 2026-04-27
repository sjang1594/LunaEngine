#pragma once
#include "ISensor.h"
#include <vector>
#include <cmath>
#include <algorithm>
#include <random>

namespace Luna
{

// FMCW Radar sensor: produces range-Doppler map from scene objects.
// Phase 1: config + simplified CPU simulation. GPU FFT in Phase 4.
class RadarSensor : public ISensor
{
public:
    RadarSensorConfig config;

    // Range-Doppler map output (rangeBins x dopplerBins), power in dB
    std::vector<float> rangeDopplerMap;

    // Detected targets after thresholding
    std::vector<RadarDetection> detections;

    RadarSensor(const std::string& name = "Radar0")
        : ISensor(SensorType::Radar, name) {}

    void Simulate(const DirectX::XMFLOAT4X4& ownerWorld, float dt) override
    {
        if (!enabled || !ShouldTick(dt)) return;

        using namespace DirectX;
        XMMATRIX owner = XMLoadFloat4x4(&ownerWorld);
        XMMATRIX ext   = XMLoadFloat4x4(&_extrinsic);
        XMMATRIX sensorWorld = ext * owner;
        XMStoreFloat4x4(&_sensorWorldMatrix, sensorWorld);

        // Extract sensor position
        _sensorPosition = { sensorWorld.r[3].m128_f32[0],
                            sensorWorld.r[3].m128_f32[1],
                            sensorWorld.r[3].m128_f32[2] };

        // Extract forward direction (Z axis of sensor frame)
        XMFLOAT3 fwd;
        XMStoreFloat3(&fwd, XMVector3Normalize(sensorWorld.r[2]));
        _sensorForward = fwd;

        // TODO Phase 4: query scene objects, synthesize IF signal, run FFT
        // For now, generate a test pattern
        GenerateTestPattern();
    }

    const DirectX::XMFLOAT4X4& GetSensorWorld()    const { return _sensorWorldMatrix; }
    const DirectX::XMFLOAT3&   GetSensorPosition() const { return _sensorPosition; }
    const DirectX::XMFLOAT3&   GetSensorForward()  const { return _sensorForward; }

private:
    DirectX::XMFLOAT4X4 _sensorWorldMatrix;
    DirectX::XMFLOAT3   _sensorPosition = {0, 0, 0};
    DirectX::XMFLOAT3   _sensorForward  = {0, 0, 1};

    void GenerateTestPattern()
    {
        uint32_t total = config.rangeBins * config.dopplerBins;
        rangeDopplerMap.resize(total);

        // Noise floor
        std::mt19937 rng(42);
        std::normal_distribution<float> noise(config.noisePowerDb, 3.0f);
        for (uint32_t i = 0; i < total; i++)
            rangeDopplerMap[i] = noise(rng);

        detections.clear();
    }
};

} // namespace Luna

