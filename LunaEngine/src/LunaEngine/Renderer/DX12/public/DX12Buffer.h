#pragma once
#include "Graphics/IBuffer.h"

namespace Luna
{
class DX12Buffer : public IBuffer
{
  public:
    DX12Buffer(BufferUsage usage, void *data, uint32_t size, uint32_t stride);
    ~DX12Buffer() override;
    class DX12Backend* GetBackend() const;
    void Bind(uint32_t slot = 0) override;
    void *Map() override;
    void Unmap() override;
    uint32_t GetSize() const override
    {
        return _size;
    }
    const D3D12_VERTEX_BUFFER_VIEW* GetVBView() const { return &_vertexBufferView; }

  private:
    BufferUsage _usage;
    ComPtr<ID3D12Resource> _resource;
    uint32_t _size;
    uint32_t _stride;
    D3D12_VERTEX_BUFFER_VIEW _vertexBufferView;
    D3D12_INDEX_BUFFER_VIEW _indexBufferView;
};
} // namespace Luna