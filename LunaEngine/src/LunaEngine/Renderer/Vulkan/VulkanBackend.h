#pragma once

#include <string>

#include "../IRenderBackend.h"

namespace Luna {
class VulkanBackend : public IRenderBackend {
 public:
  VulkanBackend();
  ~VulkanBackend();

  bool Init(void* windowHandler, const uint32_t& width,
            const uint32_t& height) override;
  void BeginFrame() override;
  void DrawFrame() override;
  void EndFrame() override;
  void Resize(uint32_t width, uint32_t height) override;
  const char* GetBackendName() const override;

 private:
  void CreateInstanceAndAurface(void* windowHandle);
};
}  // namespace Luna