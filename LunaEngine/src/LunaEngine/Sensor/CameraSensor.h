#pragma once
#include "ISensor.h"
#include <vector>
#include <cmath>

namespace Luna
{

// Camera sensor: produces RGB image + linear depth from a viewpoint.
// Phase 1: config + output stubs. GPU rendering added in Phase 2.
class CameraSensor : public ISensor
{
public:
    CameraSensorConfig config;

    // Output buffers (CPU readback, populated by backend in S2)
    std::vector<uint8_t>  rgbOutput;     // RGBA8, width*height*4
    std::vector<float>    depthOutput;   // linear depth in meters, width*height

    // S2: set to true each time a new frame has been written to rgbOutput/depthOutput
    bool hasNewFrame = false;

    // S2: GPU descriptor handle for ImGui::Image() — set by DX12Backend after resources init
    // Cast to ImTextureID: ImGui::Image((ImTextureID)cam->gpuTextureHandle, size)
    uint64_t gpuTextureHandle = 0;

    CameraSensor(const std::string& name = "Camera0")
        : ISensor(SensorType::Camera, name) {}

    void Simulate(const DirectX::XMFLOAT4X4& ownerWorld, float dt) override
    {
        if (!enabled || !ShouldTick(dt)) return;

        // Compute sensor world transform = owner * extrinsic
        using namespace DirectX;
        XMMATRIX owner = XMLoadFloat4x4(&ownerWorld);
        XMMATRIX ext   = XMLoadFloat4x4(&_extrinsic);
        XMMATRIX sensorWorld = ext * owner;
        XMStoreFloat4x4(&_sensorWorldMatrix, sensorWorld);

        // Build view matrix (inverse of sensorWorld)
        XMVECTOR det;
        XMMATRIX view = XMMatrixInverse(&det, sensorWorld);
        XMStoreFloat4x4(&_viewMatrix, view);

        // Build projection matrix
        float aspect = static_cast<float>(config.width) / static_cast<float>(config.height);
        XMMATRIX proj = XMMatrixPerspectiveFovLH(
            XMConvertToRadians(config.fovDeg), aspect, config.nearZ, config.farZ);
        XMStoreFloat4x4(&_projMatrix, proj);

        // S2: GPU offscreen render dispatched by DX12Backend::RenderCameraSensors()
        // (called once per display frame; this Simulate() just updates matrices + tick flag)
        hasNewFrame = false; // backend sets this after readback
    }

    const DirectX::XMFLOAT4X4& GetViewMatrix()  const { return _viewMatrix; }
    const DirectX::XMFLOAT4X4& GetProjMatrix()  const { return _projMatrix; }
    const DirectX::XMFLOAT4X4& GetSensorWorld() const { return _sensorWorldMatrix; }

private:
    DirectX::XMFLOAT4X4 _viewMatrix;
    DirectX::XMFLOAT4X4 _projMatrix;
    DirectX::XMFLOAT4X4 _sensorWorldMatrix;
};

} // namespace Luna

