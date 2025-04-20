#include "RenderContext.h"

#include "VulkanBackend.h"
#incldue "DX12/DX12Backend.h"

namespace Luna {
    std::unique_ptr<IRenderBackend> RenderContext::s_Backend = nullptr;
    RenderBackendType RenderContext::s_BackendType = RenderBackendType::Vulkan
}