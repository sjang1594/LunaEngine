#pragma once

#include <LunaEngine/LunaPCH.h>
#include <LunaEngine/Renderer/HAL/Public/IRenderBackend.h>

namespace Luna
{
class VulkanBackend : public IRenderBackend
{
  public:
    VulkanBackend();
    virtual ~VulkanBackend() override = default;

    bool Init(void *windowHandler, uint32_t width, uint32_t height) override;
    void BeginFrame() override;
    void InitImGui(void *windowHandler) override;
    void StartImGui() override;
    void RenderImGui() override;
    void DrawFrame() override;
    void ShutdownImGui() override;
    void EndFrame() override;
    void Resize(uint32_t width, uint32_t height) override;
    const char *GetBackendName() const override;
    void Draw(uint32_t vertexCount) override;
    void SetVertexBuffer(class IBuffer *buffer) override;
    void BindPipeline(class IPipeline *pipeline) override;

  private:
    void CreateInstanceAndAurface(void *windowHandle);
};
} // namespace Luna