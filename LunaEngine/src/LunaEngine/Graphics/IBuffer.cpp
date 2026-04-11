#include "LunaPCH.h"
#include "IBuffer.h"
#include "Renderer/HAL/Public/IRenderContext.h"

// Platform-specific buffer implementations
#ifdef _WIN32
#include "Renderer/DX12/Public/DX12Buffer.h"
#include "Renderer/DX12/Public/DX12Backend.h"
#endif

namespace Luna
{

std::shared_ptr<IBuffer> CreateBuffer(BufferUsage usage, void *data, uint32_t size,
                                       uint32_t stride)
{
    switch (IRenderContext::GetCurrentBackendType())
    {
#ifdef _WIN32
    case RenderBackendType::DirectX12:
    {
        auto* backend = static_cast<DX12Backend*>(IRenderContext::GetBackend());
        if (!backend)
            return nullptr;

        return std::make_shared<DX12Buffer>(
            backend->GetDevice().Get(),
            backend->GetD3D12MAAllocator(),
            usage, data, size, stride);
    }
#endif
    default:
        return nullptr;
    }
}

} // namespace Luna
