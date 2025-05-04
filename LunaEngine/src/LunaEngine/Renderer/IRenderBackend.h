#pragma once

#include <cstdint>

namespace Luna {
class IRenderBackend {
public:
  virtual ~IRenderBackend() = default;

  // Window Handler (GLFWindow*)
  virtual bool Init(void* windowHandler,
                    uint32_t width,
                    uint32_t height) = 0;

  // Frame start
  virtual void BeginFrame() = 0;

  // Frame Draw (submit queue)
  virtual void DrawFrame() = 0;

  // Frame End (etc: Present)
  virtual void EndFrame() = 0;

  virtual void Resize(uint32_t width, uint32_t height) = 0;

  virtual void InitImGui(void* windowHandler) = 0;

  virtual void StartImGui() = 0;

  virtual void RenderImGui() = 0;

  virtual void ShutdownImGui() = 0;

  virtual const char* GetBackendName() const = 0;
};
}  // namespace Luna