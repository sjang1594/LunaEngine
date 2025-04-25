#pragma once

#include <LunaEngine/Renderer/IRenderBackend.h>
#include <LunaEngine/LunaPCH.h>

namespace Luna {
class VulkanBackend : public IRenderBackend {
 public:
  VulkanBackend();
  ~VulkanBackend();

  bool Init(void* windowHandler, uint32_t width,
            uint32_t height) override;
  void BeginFrame() override;
  void InitImGui(void* windowHandler) override;
  void StartImGui() override;
  void RenderImGui() override;
  void DrawFrame() override;
  void ShutdownImGui() override;
  void EndFrame() override;
  void Resize(uint32_t width, uint32_t height) override;
  const char* GetBackendName() const override;

 private:
  void CreateInstanceAndAurface(void* windowHandle);
};
}  // namespace Luna