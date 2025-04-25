#pragma once

#include "IRenderBackend.h"
#include "Application/ApplicationSpecification.h"

namespace Luna {
class RenderContext {
 public:
  static void Initialize(RenderBackendType backendType, void* windowHandler,
                         uint32_t width, uint32_t height);
  static void Shutdown();
  static void BeginFrame();
  static void DrawFrame();
  static void EndFrame();
  static void Resize(uint32_t width, uint32_t height);

  static void InitImGui(void* windowHandler);
  static void StartImGuiFrame();
  static void RenderImGui();
  static void ShutdownImGui();

  static IRenderBackend* GetBackend();
  static RenderBackendType GetCurrentBackendType();
 private:
  static std::unique_ptr<IRenderBackend> s_Backend;
  static RenderBackendType s_BackendType;
};
}  // namespace Luna