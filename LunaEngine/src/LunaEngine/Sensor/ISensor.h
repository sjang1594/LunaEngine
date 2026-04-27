#pragma once
#include "SensorTypes.h"
#include <string>
#include <vector>
#include <DirectXMath.h>

namespace Luna
{

// Base interface for all sensor instances attached to a SensorComponent.
class ISensor
{
public:
    ISensor(SensorType type, const std::string& name)
        : _type(type), _name(name)
    {
        DirectX::XMStoreFloat4x4(&_extrinsic, DirectX::XMMatrixIdentity());
    }
    virtual ~ISensor() = default;

    SensorType  GetType()  const { return _type; }
    const std::string& GetName() const { return _name; }
    void SetName(const std::string& n) { _name = n; }

    // Extrinsic: local offset relative to owning GameObject's transform
    const DirectX::XMFLOAT4X4& GetExtrinsic() const { return _extrinsic; }
    void SetExtrinsic(const DirectX::XMFLOAT4X4& m)  { _extrinsic = m; }

    // Extrinsic helpers (position + euler rotation offset in degrees)
    DirectX::XMFLOAT3 extrinsicPosition = {0, 0, 0};
    DirectX::XMFLOAT3 extrinsicRotation = {0, 0, 0}; // degrees

    // Update extrinsic matrix from position + rotation
    void UpdateExtrinsicFromPosRot()
    {
        using namespace DirectX;
        XMMATRIX rot = XMMatrixRotationRollPitchYaw(
            XMConvertToRadians(extrinsicRotation.x),
            XMConvertToRadians(extrinsicRotation.y),
            XMConvertToRadians(extrinsicRotation.z));
        XMMATRIX trans = XMMatrixTranslation(extrinsicPosition.x, extrinsicPosition.y, extrinsicPosition.z);
        XMStoreFloat4x4(&_extrinsic, rot * trans);
    }

    // Called each frame (or at configured rate) with owning object's world matrix
    virtual void Simulate(const DirectX::XMFLOAT4X4& ownerWorld, float dt) = 0;

    // Is this sensor actively simulating?
    bool enabled = true;

    // Tick rate limiter: 0 = every frame
    float tickIntervalSec  = 0.0f;

protected:
    SensorType  _type;
    std::string _name;
    DirectX::XMFLOAT4X4 _extrinsic;
    float _tickAccumulator = 0.0f;

    // Returns true if enough time has elapsed for a tick
    bool ShouldTick(float dt)
    {
        if (tickIntervalSec <= 0.0f) return true;
        _tickAccumulator += dt;
        if (_tickAccumulator >= tickIntervalSec)
        {
            _tickAccumulator -= tickIntervalSec;
            return true;
        }
        return false;
    }
};

} // namespace Luna

