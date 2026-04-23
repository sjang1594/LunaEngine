#pragma once

#include <cstdint>
#include <DirectXMath.h>
#include <vector>
#include <memory>
#include <string>

using namespace DirectX;

namespace Luna
{
struct Mesh;  // defined in Renderer/Mesh.h — forward-declared to avoid pulling in D3D12MA here

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

    virtual const char *GetBackendName() const = 0;
};
} // namespace Luna
