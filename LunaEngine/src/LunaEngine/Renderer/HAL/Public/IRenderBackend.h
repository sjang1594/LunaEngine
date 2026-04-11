#pragma once

#include <cstdint>
#include <DirectXMath.h>

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

    /* Vsync control — default no-op; DX12Backend drives Present() interval with this */
    virtual void SetVSync(bool /*vsync*/) {}

    /* Per-frame MVP update — default no-op so Vulkan stub doesn't need to override */
    virtual void UpdateMVP(const XMFLOAT4X4& model, const XMFLOAT4X4& view,
                           const XMFLOAT4X4& proj) {}

    /* Per-object mesh draw — recorded into the open command list between BeginFrame/EndFrame */
    virtual void DrawMesh(const Mesh* mesh, const XMFLOAT4X4& model) {}

    virtual const char *GetBackendName() const = 0;
};
} // namespace Luna
