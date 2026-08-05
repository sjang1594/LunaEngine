#include "LunaPCH.h"
#include "NuScenesLoader.h"
#include "Logger/Logger.h"

// Suppress warnings from the large header-only library
#pragma warning(push, 0)
#include <nlohmann/json.hpp>
#pragma warning(pop)

#include <fstream>
#include <sstream>
#include <cmath>
#include <algorithm>

using json = nlohmann::json;
using namespace DirectX;

namespace Luna
{

// ---------------------------------------------------------------------------
// Coordinate transform helpers
// nuScenes ego frame: X-forward, Y-left, Z-up (right-handed)
// LunaEngine frame:   X-right,   Y-up,   Z-forward (Y-up DX)
// Mapping: lu = (-ns.y, ns.z, ns.x)
// ---------------------------------------------------------------------------

XMFLOAT3 NuScenesLoader::NsToLu(float x, float y, float z)
{
    return { -y, z, x };
}

// q_F is the quaternion representing the frame-change rotation F.
// F maps nuScenes basis vectors to LunaEngine basis vectors:
//   ns.X(fwd) → lu.Z(fwd),  ns.Y(left) → lu.-X(right→-left),  ns.Z(up) → lu.Y(up)
// q_F = (w=0.5, x=-0.5, y=-0.5, z=0.5)
// To transform a nuScenes quaternion q_ns into LunaEngine frame:
//   q_lu = q_F * q_ns * conjugate(q_F)
XMFLOAT4 NuScenesLoader::NsQuatToLu(float qw, float qx, float qy, float qz)
{
    // Frame-change quaternion and its conjugate
    const XMFLOAT4 qF  = { -0.5f, -0.5f,  0.5f, 0.5f }; // (x,y,z,w)
    const XMFLOAT4 qFc = {  0.5f,  0.5f, -0.5f, 0.5f }; // conjugate

    XMFLOAT4 qNs = { qx, qy, qz, qw };                   // (x,y,z,w)
    return QuatNormalize(QuatMul(QuatMul(qF, qNs), qFc));
}

XMFLOAT4 NuScenesLoader::QuatMul(XMFLOAT4 a, XMFLOAT4 b)
{
    // a = (ax,ay,az,aw), b = (bx,by,bz,bw) in (x,y,z,w) layout
    return {
        a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
        a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
        a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
        a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z
    };
}

XMFLOAT3 NuScenesLoader::QuatRotate(XMFLOAT4 q, XMFLOAT3 v)
{
    XMVECTOR Q = XMLoadFloat4(&q);
    XMVECTOR V = XMVectorSet(v.x, v.y, v.z, 0.0f);
    XMVECTOR R = XMVector3Rotate(V, Q);
    XMFLOAT3 out; XMStoreFloat3(&out, R); return out;
}

XMFLOAT4 NuScenesLoader::QuatConjugate(XMFLOAT4 q)
{
    return { -q.x, -q.y, -q.z, q.w };
}

XMFLOAT4 NuScenesLoader::QuatNormalize(XMFLOAT4 q)
{
    float len = sqrtf(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
    if (len < 1e-9f) return { 0,0,0,1 };
    float inv = 1.0f / len;
    return { q.x*inv, q.y*inv, q.z*inv, q.w*inv };
}

// ---------------------------------------------------------------------------
// JSON loading
// ---------------------------------------------------------------------------

static std::string ReadFile(const std::string& path)
{
    std::ifstream f(path);
    if (!f.is_open()) return {};
    std::ostringstream ss; ss << f.rdbuf();
    return ss.str();
}

bool NuScenesLoader::LoadJsonTable(const std::string& filename,
                                    const std::string& tokenField,
                                    JsonMap& out)
{
    std::string path = _dataRoot + "/" + filename;
    std::string raw = ReadFile(path);
    if (raw.empty())
    {
        LUNA_LOG_ERROR("NuScenes: cannot read %s", path.c_str());
        return false;
    }
    json arr = json::parse(raw, nullptr, false);
    if (!arr.is_array())
    {
        LUNA_LOG_ERROR("NuScenes: %s is not a JSON array", filename.c_str());
        return false;
    }
    for (auto& obj : arr)
    {
        std::string token = obj.value(tokenField, "");
        if (!token.empty())
            out[token] = obj.dump();
    }
    LUNA_LOG_INFO("NuScenes: loaded %s (%zu entries)", filename.c_str(), out.size());
    return true;
}

bool NuScenesLoader::Load(const std::string& dataRoot)
{
    _dataRoot = dataRoot;
    _loaded   = false;

    // Clear previous state
    _sampleMap.clear();
    _sampleDataMap.clear();
    _sampleAnnotMap.clear();
    _calibSensorMap.clear();
    _egoPoseMap.clear();
    _sensorMap.clear();
    _annBySample.clear();
    _sampleDataBySensor.clear();
    _scenes.clear();

    // Load all JSON tables
    if (!LoadJsonTable("sample.json",            "token", _sampleMap))          return false;
    if (!LoadJsonTable("sample_data.json",       "token", _sampleDataMap))      return false;
    if (!LoadJsonTable("sample_annotation.json", "token", _sampleAnnotMap))     return false;
    if (!LoadJsonTable("calibrated_sensor.json", "token", _calibSensorMap))     return false;
    if (!LoadJsonTable("ego_pose.json",          "token", _egoPoseMap))         return false;
    if (!LoadJsonTable("sensor.json",            "token", _sensorMap))          return false;

    // Build annotation-by-sample index
    for (auto& [tok, raw] : _sampleAnnotMap)
    {
        json obj = json::parse(raw, nullptr, false);
        std::string sampleToken = obj.value("sample_token", "");
        if (!sampleToken.empty())
            _annBySample[sampleToken].push_back(tok);
    }

    // Build sample_data-by-sensor-channel index and resolve sensor channel names
    // sensor_token → channel name (e.g. "LIDAR_TOP")
    std::unordered_map<std::string, std::string> sensorChannel;
    {
        std::string raw2 = ReadFile(_dataRoot + "/sensor.json");
        if (!raw2.empty())
        {
            json arr = json::parse(raw2, nullptr, false);
            for (auto& s : arr)
                sensorChannel[s.value("token", "")] = s.value("channel", "");
        }
    }

    for (auto& [tok, raw] : _sampleDataMap)
    {
        json obj = json::parse(raw, nullptr, false);
        std::string sampleTok = obj.value("sample_token", "");
        std::string calibTok  = obj.value("calibrated_sensor_token", "");
        if (sampleTok.empty() || calibTok.empty()) continue;

        // Resolve channel via calibrated_sensor → sensor
        std::string channel;
        auto cit = _calibSensorMap.find(calibTok);
        if (cit != _calibSensorMap.end())
        {
            json cobj = json::parse(cit->second, nullptr, false);
            std::string sensorTok = cobj.value("sensor_token", "");
            auto sit = sensorChannel.find(sensorTok);
            if (sit != sensorChannel.end()) channel = sit->second;
        }
        if (!channel.empty())
            _sampleDataBySensor[sampleTok][channel] = tok;
    }

    // Build scene list with ordered sample tokens
    std::string sceneRaw = ReadFile(_dataRoot + "/scene.json");
    if (sceneRaw.empty()) { LUNA_LOG_ERROR("NuScenes: scene.json missing"); return false; }
    json sceneArr = json::parse(sceneRaw, nullptr, false);

    for (auto& s : sceneArr)
    {
        SceneInfo info;
        info.token       = s.value("token", "");
        info.name        = s.value("name", "");
        info.description = s.value("description", "");
        info.numSamples  = s.value("nbr_samples", 0);

        // Walk linked list: first_sample_token → next → ... → ""
        std::string cur = s.value("first_sample_token", "");
        while (!cur.empty())
        {
            info.sampleTokens.push_back(cur);
            auto it = _sampleMap.find(cur);
            if (it == _sampleMap.end()) break;
            json sobj = json::parse(it->second, nullptr, false);
            cur = sobj.value("next", "");
        }
        _scenes.push_back(std::move(info));
    }

    LUNA_LOG_INFO("NuScenes: loaded %zu scenes from %s", _scenes.size(), dataRoot.c_str());
    _loaded = true;
    return true;
}

// ---------------------------------------------------------------------------
// Sample loading
// ---------------------------------------------------------------------------

bool NuScenesLoader::LoadSample(const std::string& sceneToken, int sampleIndex, SampleData& out)
{
    out = SampleData{};

    // Find scene
    const SceneInfo* scene = nullptr;
    for (auto& sc : _scenes)
        if (sc.token == sceneToken) { scene = &sc; break; }
    if (!scene || sampleIndex < 0 || sampleIndex >= (int)scene->sampleTokens.size())
    {
        LUNA_LOG_ERROR("NuScenes: invalid scene/sample index");
        return false;
    }
    std::string sampleToken = scene->sampleTokens[sampleIndex];

    // ── 1. Ego pose for this sample ──────────────────────────────────────────
    // Get sample_data for LIDAR_TOP to resolve ego_pose_token
    auto sdIt = _sampleDataBySensor.find(sampleToken);
    if (sdIt == _sampleDataBySensor.end())
    {
        LUNA_LOG_ERROR("NuScenes: no sample_data for sample %s", sampleToken.c_str());
        return false;
    }
    auto lidarSdIt = sdIt->second.find("LIDAR_TOP");
    if (lidarSdIt == sdIt->second.end())
    {
        LUNA_LOG_ERROR("NuScenes: no LIDAR_TOP data for sample %s", sampleToken.c_str());
        return false;
    }

    json sdObj;
    {
        auto it = _sampleDataMap.find(lidarSdIt->second);
        if (it == _sampleDataMap.end()) return false;
        sdObj = json::parse(it->second, nullptr, false);
    }

    std::string egoPoseTok   = sdObj.value("ego_pose_token", "");
    std::string calibSensTok = sdObj.value("calibrated_sensor_token", "");
    std::string lidarFilename = sdObj.value("filename", "");

    // Parse ego pose (global frame)
    XMFLOAT3 egoT{}; XMFLOAT4 egoQ{0,0,0,1};
    {
        auto it = _egoPoseMap.find(egoPoseTok);
        if (it != _egoPoseMap.end())
        {
            json ep = json::parse(it->second, nullptr, false);
            auto& t = ep["translation"]; auto& r = ep["rotation"];
            egoT = { (float)t[0], (float)t[1], (float)t[2] };
            // nuScenes quaternion is (w, x, y, z)
            egoQ = { (float)r[1], (float)r[2], (float)r[3], (float)r[0] }; // stored as (x,y,z,w)
        }
    }

    // ── 2. LIDAR_TOP calibrated sensor (sensor-in-ego frame) ────────────────
    XMFLOAT3 lidarT{}; XMFLOAT4 lidarQ{0,0,0,1};
    {
        auto it = _calibSensorMap.find(calibSensTok);
        if (it != _calibSensorMap.end())
        {
            json cs = json::parse(it->second, nullptr, false);
            auto& t = cs["translation"]; auto& r = cs["rotation"];
            lidarT = { (float)t[0], (float)t[1], (float)t[2] };
            lidarQ = { (float)r[1], (float)r[2], (float)r[3], (float)r[0] };
        }
    }
    // LiDAR position in ego frame → LunaEngine
    out.lidarPosition = NsToLu(lidarT.x, lidarT.y, lidarT.z);
    out.lidarRotation = NsQuatToLu(lidarQ.w, lidarQ.x, lidarQ.y, lidarQ.z);

    // ── 3. Annotations (global frame → ego-relative → LunaEngine) ───────────
    // Inverse ego rotation for global→ego transform
    XMFLOAT4 egoQInv = QuatConjugate(egoQ);

    auto annIt = _annBySample.find(sampleToken);
    if (annIt != _annBySample.end())
    {
        for (auto& annTok : annIt->second)
        {
            auto it = _sampleAnnotMap.find(annTok);
            if (it == _sampleAnnotMap.end()) continue;
            json ann = json::parse(it->second, nullptr, false);

            auto& t = ann["translation"]; auto& sz = ann["size"]; auto& r = ann["rotation"];

            // Global position → ego-relative (subtract ego translation, apply inv ego rotation)
            XMFLOAT3 globalPos = { (float)t[0] - egoT.x,
                                   (float)t[1] - egoT.y,
                                   (float)t[2] - egoT.z };
            XMFLOAT3 egoPos = QuatRotate(egoQInv, globalPos);

            // Size: nuScenes [width(Y), length(X), height(Z)] → LunaEngine [X=width, Y=height, Z=length]
            AnnotationBox box;
            box.translation = NsToLu(egoPos.x, egoPos.y, egoPos.z);
            box.size        = { (float)sz[0], (float)sz[2], (float)sz[1] }; // (w, h, l)

            // Annotation rotation: global quaternion → ego → LunaEngine
            // (w,x,y,z) in nuScenes
            float rw = (float)r[0], rx = (float)r[1], ry = (float)r[2], rz = (float)r[3];
            XMFLOAT4 qGlobalAnn = { rx, ry, rz, rw }; // (x,y,z,w)
            XMFLOAT4 qEgoAnn    = QuatMul(egoQInv, qGlobalAnn);
            box.rotation   = NsQuatToLu(qEgoAnn.w, qEgoAnn.x, qEgoAnn.y, qEgoAnn.z);
            box.category   = ann.value("category_name", "unknown");

            out.annotations.push_back(box);
        }
    }
    LUNA_LOG_INFO("NuScenes: %zu annotations for sample %d", out.annotations.size(), sampleIndex);

    // ── 4. Load real LiDAR .pcd.bin (sensor frame → ego frame → LunaEngine) ─
    if (!lidarFilename.empty())
    {
        std::string pcdPath = _dataRoot + "/../" + lidarFilename;
        // Normalize path separators
        std::replace(pcdPath.begin(), pcdPath.end(), '\\', '/');
        std::ifstream f(pcdPath, std::ios::binary);
        if (f.is_open())
        {
            // nuScenes .pcd.bin: each point = 5 float32 (x, y, z, intensity, ring_index)
            f.seekg(0, std::ios::end);
            auto bytes = (size_t)f.tellg();
            f.seekg(0, std::ios::beg);
            size_t nPts = bytes / (5 * sizeof(float));
            std::vector<float> buf(nPts * 5);
            f.read(reinterpret_cast<char*>(buf.data()), nPts * 5 * sizeof(float));

            out.lidarPoints.reserve(nPts);
            for (size_t i = 0; i < nPts; ++i)
            {
                float sx = buf[i*5+0], sy = buf[i*5+1], sz = buf[i*5+2];
                float intensity = buf[i*5+3];

                // Sensor → ego: p_ego = R_lidar * p_sensor + t_lidar
                XMFLOAT3 pSensor = { sx, sy, sz };
                XMFLOAT3 pEgo    = QuatRotate(lidarQ, pSensor);
                pEgo.x += lidarT.x; pEgo.y += lidarT.y; pEgo.z += lidarT.z;

                // Ego → LunaEngine Y-up
                LiDARPoint pt;
                pt.position  = NsToLu(pEgo.x, pEgo.y, pEgo.z);
                pt.intensity = std::min(intensity / 255.0f, 1.0f);
                out.lidarPoints.push_back(pt);
            }
            out.lidarLoaded = true;
            LUNA_LOG_INFO("NuScenes: loaded %zu LiDAR points from %s",
                          out.lidarPoints.size(), lidarFilename.c_str());
        }
        else
        {
            LUNA_LOG_WARN("NuScenes: cannot open LiDAR file: %s", pcdPath.c_str());
        }
    }

    return true;
}

} // namespace Luna
