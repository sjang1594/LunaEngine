#include "Application.h"

#include "Renderer/DX12/DX12Backend.h"
#include "Renderer/Vulkan/VulkanBackend.h

namespace Luna {
std::unique_ptr<IRenderBackend> CreateRenderBackend(RenderAPI api) {
  switch (api) { case RenderAPI::DirectX12:
      return std::make_unique<DX12Backend>();
      break;
    case RenderAPI::Vulkan:
        return std::make_unique<Vulkan
  }
}