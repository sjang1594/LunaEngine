#pragma once
#include <cstdint>
#include <DirectXMath.h>

namespace Luna
{

enum class SensorType : uint8_t
{
    Camera,
    LiDAR,
    Radar,
    COUNT
};

inline const char* SensorTypeToString(SensorType t)
{
    switch (t)
    {
    case SensorType::Camera: return "Camera";
    case SensorType::LiDAR:  return "LiDAR";
    case SensorType::Radar:  return "Radar";
    default:                  return "Unknown";
    }
}

// ── Camera Sensor Configuration ──────────────────────────────────────
struct CameraSensorConfig
{
    uint32_t width      = 640;
    uint32_t height     = 480;
    float    fovDeg     = 60.0f;
    float    nearZ      = 0.1f;
    float    farZ       = 200.0f;
    // Brown-Conrady lens distortion
    float    k1 = 0.0f, k2 = 0.0f;
    float    p1 = 0.0f, p2 = 0.0f;
    // Gaussian noise sigma (0 = clean)
    float    noiseSigma = 0.0f;
};

// ── LiDAR Sensor Configuration ──────────────────────────────────────
enum class LiDARScanPattern : uint8_t
{
    Spinning,       // 360° horizontal, limited vertical (e.g. Velodyne)
    SolidState      // limited H/V FOV, dense (e.g. Livox)
};

struct LiDARSensorConfig
{
    LiDARScanPattern scanPattern   = LiDARScanPattern::Spinning;
    float hFovDeg                  = 360.0f;   // horizontal FOV
    float vFovDeg                  = 30.0f;    // vertical FOV
    float hResolutionDeg           = 0.2f;     // horizontal angular resolution
    float vResolutionDeg           = 2.0f;     // vertical angular resolution
    float maxRange                 = 120.0f;   // meters
    float rangeNoiseSigma          = 0.02f;    // meters
    float beamDivergenceMrad       = 0.5f;
    uint32_t returnsPerPulse       = 1;        // single/dual return
};

// ── Radar Sensor Configuration (FMCW) ──────────────────────────────
struct RadarSensorConfig
{
    float centerFreqGHz   = 77.0f;
    float bandwidthMHz    = 500.0f;
    float chirpDurationUs = 50.0f;
    uint32_t numChirps    = 128;
    float maxRangeM       = 100.0f;
    float maxVelocityMps  = 50.0f;
    float hFovDeg         = 120.0f;
    float vFovDeg         = 20.0f;
    uint32_t rangeBins    = 256;
    uint32_t dopplerBins  = 128;
    float noisePowerDb    = -20.0f;
};

// ── LiDAR output point ──────────────────────────────────────────────
struct LiDARPoint
{
    DirectX::XMFLOAT3 position;
    float   intensity;
    uint32_t returnIndex;
};

// ── Radar detection after CFAR/peak detection ───────────────────────
struct RadarDetection
{
    float rangeM;
    float velocityMps;
    float azimuthDeg;
    float powerDb;
};

} // namespace Luna

