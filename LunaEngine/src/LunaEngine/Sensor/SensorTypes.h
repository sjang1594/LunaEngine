#pragma once
#include <cstdint>
#include <vector>
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
    // Brown-Conrady lens distortion (OpenCV compatible: k1,k2,k3 radial + p1,p2 tangential)
    float    k1 = 0.0f, k2 = 0.0f, k3 = 0.0f;
    float    p1 = 0.0f, p2 = 0.0f;
    // Radiometric model
    float    exposureEV100    = 0.0f;  // exposure value at ISO 100 (0 = neutral)
    float    shotNoiseFactor  = 0.0f;  // Poisson shot noise scale (0 = clean)
    float    readNoiseSigma   = 0.0f;  // Gaussian read noise std-dev in [0,1] (0 = clean)
    // Capture rate (0 = every frame)
    uint32_t captureIntervalFrames = 5; // render every N display frames (~12 Hz at 60 FPS)
};

// Derived pinhole intrinsics from CameraSensorConfig
struct CameraIntrinsics
{
    float fx, fy; // focal lengths in pixels
    float cx, cy; // principal point in pixels

    static CameraIntrinsics FromConfig(const CameraSensorConfig& cfg)
    {
        CameraIntrinsics k;
        float tanHalfFov = std::tanf(cfg.fovDeg * 0.5f * 3.14159265f / 180.0f);
        k.fx = (cfg.width  * 0.5f) / tanHalfFov;
        k.fy = k.fx; // square pixels
        k.cx = cfg.width  * 0.5f;
        k.cy = cfg.height * 0.5f;
        return k;
    }
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
    float vFovDeg                  = 30.0f;    // vertical FOV (ignored when beamElevationsDeg set)
    float hResolutionDeg           = 0.2f;     // horizontal angular resolution
    float vResolutionDeg           = 2.0f;     // vertical angular resolution (ignored when beamElevationsDeg set)
    float maxRange                 = 120.0f;   // meters
    float rangeNoiseSigma          = 0.02f;    // meters (σ ≈ 2 cm for HDL-32E)
    float beamDivergenceMrad       = 0.5f;
    uint32_t returnsPerPulse       = 1;        // single/dual return
    // S3: non-uniform beam elevations — when non-empty, overrides vFovDeg/vResolutionDeg
    std::vector<float> beamElevationsDeg;
    // S3: per-sensor NIR reflectivity scale (1.0 = use albedo luminance proxy)
    float nirMultiplier            = 1.0f;

    // HDL-32E preset: 32 beams, non-uniform elevations from Velodyne datasheet
    static LiDARSensorConfig MakeHDL32E()
    {
        LiDARSensorConfig cfg;
        cfg.scanPattern     = LiDARScanPattern::Spinning;
        cfg.hFovDeg         = 360.0f;
        cfg.hResolutionDeg  = 0.2f;   // ~1800 azimuth steps → 57,600 points/scan
        cfg.maxRange        = 100.0f;
        cfg.rangeNoiseSigma = 0.02f;
        cfg.beamElevationsDeg = {
            -30.67f, -29.33f, -28.00f, -26.67f,
            -25.33f, -24.00f, -22.67f, -21.33f,
            -20.00f, -18.67f, -17.33f, -16.00f,
            -14.67f, -13.33f, -12.00f, -10.67f,
             -9.33f,  -8.00f,  -6.67f,  -5.33f,
             -4.00f,  -2.67f,  -1.33f,   0.00f,
              1.33f,   2.67f,   4.00f,   5.33f,
              6.67f,   8.00f,   9.33f,  10.67f,
        };
        return cfg;
    }
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

