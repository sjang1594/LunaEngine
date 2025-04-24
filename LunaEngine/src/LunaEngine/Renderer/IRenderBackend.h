#pragma once

#include <cstdint>

namespace Luna {
class IRenderBackend {
 public:
  virtual ~IRenderBackend() = default;

  // Window Handler (GLFWindow*)
  virtual bool Init(void* windowHandler, const uint32_t& width,
                    const uint32_t& height) = 0;

  // Frame start
  virtual void BeginFrame() = 0;

  // Frame Draw (submit queue)
  virtual void DrawFrame() = 0;

  // Frame End (etc: Present)
  virtual void EndFrame() = 0;


  virtual void Resize(uint32_t width, uint32_t height) = 0;

  // [DEBUG]
  virtual const char* GetBackendName() const = 0;
};
}  // namespace Luna