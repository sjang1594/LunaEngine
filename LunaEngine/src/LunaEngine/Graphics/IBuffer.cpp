#include "LunaPCH.h"
#include "IBuffer.h"

#ifdef _WIN32
#include "Renderer/HAL/Public/IRenderContext.h"
#include "Renderer/DX12/Public/DX12Buffer.h"

namespace Luna
{

std::shared_ptr<IBuffer> CreateBuffer(BufferUsage usage, void *data, uint32_t size,
                                       uint32_t stride)
{
    switch (IRenderContext::GetCurrentBackendType())
    {
    case RenderBackendType::DirectX12:
        return std::make_shared<DX12Buffer>(usage, data, size, stride);
    default:
        return nullptr;
    }
}

} // namespace Luna
#endif // _WIN32
