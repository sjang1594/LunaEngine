#pragma once

#include <cstdint>

namespace Luna
{
class IRenderBackend
{
  public:
    virtual ~IRenderBackend() = default;

    /* Backend Initialization
     * Window Handler (GLFWindow*)
     */
    virtual bool Init(void *windowHandler, uint32_t width, uint32_t height) = 0;
    virtual void Shutdown() = 0;
    
    /* Frame Control
     * BeginFrame() / EndFrame() / DrawFrame() / EndFrame()
    */
    virtual void BeginFrame() = 0;
    virtual void DrawFrame() = 0;
    virtual void EndFrame() = 0;
    virtual void Resize(uint32_t width, uint32_t height) = 0;

    /* IMGUI Support */
    virtual void InitImGui(void *windowHandler) = 0;
    virtual void StartImGui() = 0;
    virtual void RenderImGui() = 0;
    virtual void ShutdownImGui() = 0;

    // Rendering Core
    virtual void Draw(uint32_t vertexCount) = 0;
    virtual void SetVertexBuffer(class IBuffer* buffer) = 0;
    virtual void BindPipeline(class IPipeline* pipeline) = 0;

    virtual const char *GetBackendName() const = 0;
};
} // namespace Luna