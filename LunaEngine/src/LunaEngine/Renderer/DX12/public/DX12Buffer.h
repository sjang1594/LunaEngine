#pragma once
#include "Graphics/IBuffer.h"

namespace Luna
{
class DX12Buffer : public IBuffer
{
  public:
    DX12Buffer(BufferUsage usage, const void *data, uint32_t size, uint32_t stride);
    ~DX12Buffer() override;
    class DX12Backend* GetBackend() const;
    void Bind(uint32_t slot = 0) override;
    void *Map() override;
    void Unmap() override;
    uint32_t GetSize() const override { return _size; }
    void UpdateData(void* data, uint32_t size);
    const D3D12_VERTEX_BUFFER_VIEW* GetVBView() const { return &_vertexBufferView; }
    const D3D12_CONSTANT_BUFFER_VIEW_DESC* GetCBView() const { return &_constantBufferView; }
    
  private:
    BufferUsage _usage;
    ComPtr<ID3D12Resource> _resource;
    uint32_t _size;
    uint32_t _stride;
    D3D12_VERTEX_BUFFER_VIEW _vertexBufferView;
    D3D12_INDEX_BUFFER_VIEW _indexBufferView;
    D3D12_CONSTANT_BUFFER_VIEW_DESC _constantBufferView;
};
} // namespace Luna