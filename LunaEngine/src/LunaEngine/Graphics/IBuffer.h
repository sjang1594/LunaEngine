#pragma once
#include <cstdint>
#include <memory>

namespace Luna
{
enum class BufferUsage
{
    Vertex,
    Index,
    Uniform,
    Storage,
    Constant,
};

class IBuffer
{
  public:
    virtual ~IBuffer() = default;
    virtual void Bind(uint32_t slot = 0) = 0;
    virtual void *Map() = 0;
    virtual void Unmap() = 0;
    virtual uint32_t GetSize() const = 0;
};

// P0-05: Factory function is DX12-only; not yet backed by a Vulkan implementation.
// Use DX12Backend::GetD3D12MAAllocator() + MeshLoader for production buffer creation.
#ifdef _WIN32
std::shared_ptr<IBuffer> CreateBuffer(BufferUsage usage, void *data, uint32_t size,
                                        uint32_t stride = 0);
#endif
} // namespace Luna