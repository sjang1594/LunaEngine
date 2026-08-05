#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <DirectXMath.h>

namespace Luna
{

// Loads a single nuScenes sample into LunaEngine-compatible data.
//
// Coordinate transform: nuScenes ego frame (X-fwd, Y-left, Z-up, right-handed)
//   → LunaEngine (X-right, Y-up, Z-fwd, Y-up DX convention)
// Mapping: lu = (-ns.y, ns.z, ns.x)
//
// All output positions are in ego-relative LunaEngine space (ego vehicle at origin).
class NuScenesLoader
{
public:
    // 3D bounding box of one detected object, in LunaEngine ego-relative space
    struct AnnotationBox
    {
        DirectX::XMFLOAT3 translation;  // centre position (LunaEngine Y-up)
        DirectX::XMFLOAT3 size;         // (X=width, Y=height, Z=length)
        DirectX::XMFLOAT4 rotation;     // quaternion (x,y,z,w) in LunaEngine frame
        std::string       category;     // e.g. "vehicle.car"
    };

    // Reference LiDAR point (from real nuScenes scan), ego-relative LunaEngine space
    struct LiDARPoint
    {
        DirectX::XMFLOAT3 position;
        float             intensity;
    };

    // Metadata about one nuScenes scene
    struct SceneInfo
    {
        std::string token;
        std::string name;
        std::string description;
        int         numSamples = 0;
        std::vector<std::string> sampleTokens; // ordered by timestamp
    };

    // Result of loading one sample
    struct SampleData
    {
        std::vector<AnnotationBox> annotations;
        std::vector<LiDARPoint>   lidarPoints;       // real scan, ego-relative
        DirectX::XMFLOAT3         lidarPosition;     // LIDAR_TOP in ego-relative LunaEngine
        DirectX::XMFLOAT4         lidarRotation;     // quaternion (x,y,z,w)
        bool                      lidarLoaded = false;
    };

    // Load nuScenes JSON tables from dataRoot (the v1.0-xxx directory).
    // Returns false if any required JSON is missing.
    bool Load(const std::string& dataRoot);

    bool IsLoaded() const { return _loaded; }
    const std::string& GetDataRoot() const { return _dataRoot; }
    const std::vector<SceneInfo>& GetScenes() const { return _scenes; }

    // Load one sample by index within a scene (0-based).
    // Returns false if sample data or LiDAR file is unavailable.
    bool LoadSample(const std::string& sceneToken, int sampleIndex, SampleData& out);

private:
    bool _loaded = false;
    std::string _dataRoot;
    std::vector<SceneInfo> _scenes;

    // Token-keyed lookup tables (token → JSON string → parsed on demand)
    using JsonMap = std::unordered_map<std::string, std::string>;
    JsonMap _sampleMap;           // sample_token → JSON object string
    JsonMap _sampleDataMap;       // sample_data_token → JSON object string
    JsonMap _sampleAnnotMap;      // annotation_token → JSON object string
    JsonMap _calibSensorMap;      // calibrated_sensor_token → JSON object string
    JsonMap _egoPoseMap;          // ego_pose_token → JSON object string
    JsonMap _sensorMap;           // sensor_token → JSON object string (for sensor modality)

    // sample_token → list of annotation tokens
    std::unordered_map<std::string, std::vector<std::string>> _annBySample;
    // sample_token → map of sensor channel → sample_data_token
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> _sampleDataBySensor;

    bool LoadJsonTable(const std::string& filename, const std::string& tokenField, JsonMap& out);

    // Coordinate helpers
    static DirectX::XMFLOAT3 NsToLu(float x, float y, float z);
    static DirectX::XMFLOAT4 NsQuatToLu(float qw, float qx, float qy, float qz);
    static DirectX::XMFLOAT4 QuatMul(DirectX::XMFLOAT4 a, DirectX::XMFLOAT4 b);
    static DirectX::XMFLOAT3 QuatRotate(DirectX::XMFLOAT4 q, DirectX::XMFLOAT3 v);
    static DirectX::XMFLOAT4 QuatConjugate(DirectX::XMFLOAT4 q);
    static DirectX::XMFLOAT4 QuatNormalize(DirectX::XMFLOAT4 q);
};

} // namespace Luna
