#pragma once

#include <cstdint>
#include <DirectXMath.h>
#include <vector>
#include <memory>
#include <string>

namespace Luna
{
using namespace DirectX;  // scoped to Luna — avoids polluting global namespace
struct Mesh;  // defined in Renderer/Mesh.h — forward-declared to avoid pulling in D3D12MA here
class IGPUProfiler;  // Phase 22: GPU profiler interface

class IRenderBackend
{
  public:
    virtual ~IRenderBackend() = default;

    /* Backend Initialization
     * Window Handler (GLFWindow*)
     */
    virtual bool Init(void *windowHandler, uint32_t width, uint32_t height) = 0;
    virtual void Shutdown() = 0;

    /* Frame Control */
    virtual void BeginFrame() = 0;
    virtual void DrawFrame() = 0;
    virtual void EndFrame() = 0;
    virtual void Resize(uint32_t width, uint32_t height) = 0;

    /* IMGUI Support */
    virtual void InitImGui(void *windowHandler) = 0;
    virtual void StartImGui() = 0;
    virtual void RenderImGui() = 0;
    virtual void ShutdownImGui() = 0;

    /* Rendering Core */
    virtual void Draw(uint32_t vertexCount) = 0;
    virtual void SetVertexBuffer(class IBuffer* buffer) = 0;
    virtual void BindPipeline(class IPipeline* pipeline) = 0;

    /* Per-frame MVP update — default no-op so Vulkan stub doesn't need to override */
    virtual void UpdateMVP(const XMFLOAT4X4& model, const XMFLOAT4X4& view,
                           const XMFLOAT4X4& proj) {}

    /* Per-object mesh draw — recorded into the open command list between BeginFrame/EndFrame */
    virtual void DrawMesh(const Mesh* mesh, const XMFLOAT4X4& model) {}

    /* Load meshes from a glTF file — backend-specific implementation */
    virtual std::vector<std::shared_ptr<Mesh>> LoadMeshes(const std::string& path) { return {}; }

    /* Phase 21: retrieve per-mesh transforms from the last LoadMeshes() call */
    virtual std::vector<XMFLOAT4X4> GetLastLoadTransforms() const { return {}; }

    /* Debug: load a procedural flat quad instead of a glTF file */
    virtual std::vector<std::shared_ptr<Mesh>> LoadDebugQuad() { return {}; }

    /* VSync toggle — P2-04 */
    virtual void SetVSync(bool vsync) {}

    /* Phase 12: Flush recorded DrawMesh() calls — upload instances, dispatch GPU cull, execute indirect draws.
       Called after all DrawMesh() calls and before CompositeFrame(). */
    virtual void FlushDraws() {}

    /* Phase 7: Deferred composite — reads G-buffer, runs lighting, writes back buffer */
    virtual void CompositeFrame() {}

    /* Phase 14: IBL — load equirectangular HDR and run GPU precompute. */
    virtual bool LoadHDREnvironment(const std::string& /*hdrPath*/) { return false; }

    /* Phase 22: Get GPU profiler interface for timing queries */
    virtual IGPUProfiler* GetGPUProfiler() { return nullptr; }

    /* Phase 24: Point light data for clustered lighting */
    struct PointLightDesc {
        float position[3]; float radius;
        float color[3];    float intensity;
    };
    virtual void SetPointLights(const std::vector<PointLightDesc>& /*lights*/) {}

    /* Phase 29: Volumetric fog parameters */
    struct VolumetricFogParams {
        bool  enabled       = false;
        float density       = 0.02f;
        float heightFalloff = 0.15f;
        float baseHeight    = -2.0f;
        float scattering    = 0.8f;
        float extinction    = 1.0f;
        float phaseG        = 0.3f;
    };
    virtual void SetVolumetricFogParams(const VolumetricFogParams& /*p*/) {}

    /* Phase 30: Global Illumination parameters */
    struct GIParams {
        float probeGridOrigin[3]  = {-8.0f, 0.0f, -8.0f};
        float probeGridSpacing[3] = { 2.0f, 2.0f,  2.0f};
        float temporalAlpha       = 0.1f;
        int   numSSGIRays         = 8;
        float maxRayDist          = 5.0f;
    };
    virtual void SetGIParams(const GIParams& /*p*/) {}

    // Calibration range scene — procedural geometry (ground + obstacles at known positions)
    virtual std::vector<std::shared_ptr<Mesh>> LoadCalibrationScene() { return {}; }

    // Generic procedural box scene: creates ONE unit-box mesh, sets up GPU-driven rendering,
    // and builds a TLAS with the supplied per-instance world transforms.
    // Returns the shared Mesh (nullptr on failure). instanceTransforms must not be empty.
    virtual std::shared_ptr<Mesh> CreateProceduralBoxScene(
        const std::vector<DirectX::XMFLOAT4X4>& instanceTransforms) { return nullptr; }

    // S2: render all active camera sensors into their per-camera offscreen RTs
    virtual void RenderCameraSensors() {}

    // S3: dispatch LiDAR GPU raycasting for all active LiDAR sensors
    virtual void RenderLiDARSensors() {}
    // S3: overlay LiDAR point clouds onto the viewport (call after RenderLiDARSensors)
    virtual void RenderLiDARPointClouds() {}

    virtual const char *GetBackendName() const = 0;
    virtual DirectX::XMFLOAT4X4 GetCurrentVP() const { return {}; }
};
} // namespace Luna
