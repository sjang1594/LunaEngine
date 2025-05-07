#include "LunaPCH.h"
#include "LunaEngine/Renderer/DX12/public/DX12Buffer.h"
#include "LunaEngine/Renderer/DX12/public/DX12Backend.h"
#include "Renderer/IRenderContext.h"

namespace Luna
{

DX12Buffer::DX12Buffer(BufferUsage usage, void *data, uint32_t size, uint32_t stride)
    : _usage(usage), _size(size), _stride(stride)
{
    if (auto dx12backend = GetBackend())
    {
        // ROOT Signature START
        auto device = dx12backend->GetDevice();
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
    
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = size;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; // ROW MAJOR
         
        HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                                         D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                         IID_PPV_ARGS(&_resource));
        assert(SUCCEEDED(hr));
    
        if (data)
        {
            // Copy the triangle data to the vertex buffer
            void *mapped = nullptr;
            D3D12_RANGE range = {0, 0}; // we do not intended to read from this resource on the CPU
            _resource->Map(0, &range, &mapped);
            memcpy(mapped, data, size);
            _resource->Unmap(0, nullptr);
        }
        
        if (_usage == BufferUsage::Vertex)
        {
            _vertexBufferView.BufferLocation = _resource->GetGPUVirtualAddress();
            _vertexBufferView.SizeInBytes = size;
            _vertexBufferView.StrideInBytes = _stride;
        }
        else if (_usage == BufferUsage::Index)
        {
            _indexBufferView.BufferLocation = _resource->GetGPUVirtualAddress();
            _indexBufferView.Format = DXGI_FORMAT_R32_UINT;
            _indexBufferView.SizeInBytes = size;
        }
        // Root Signature END
    }
    else
    {
        assert(false && "IRenderBackend is not DX12Backend");
    }
}

DX12Buffer::~DX12Buffer()
{
    _resource.Reset();
}

DX12Backend* DX12Buffer::GetBackend() const
{
    if (auto backend = IRenderContext::GetBackend())
    {
        return dynamic_cast<DX12Backend*>(backend);
    }
    else
    {
        return nullptr;
    }
}

void DX12Buffer::Bind(uint32_t slot)
{
    // ComPtr<ID3D12GraphicsCommandList>
    auto cmdList = GetBackend()->GetCommandList();

    switch (_usage)
    {
    case BufferUsage::Vertex:
        cmdList->IASetVertexBuffers(slot, 1, &_vertexBufferView);
        break;
    case BufferUsage::Index:
        cmdList->IASetIndexBuffer(&_indexBufferView);
        break;

    case BufferUsage::Uniform:
        cmdList->SetGraphicsRootConstantBufferView(slot, _resource->GetGPUVirtualAddress());
        break;
    default:
        break;
    }
}

void *DX12Buffer::Map()
{
    void *data = nullptr;
    D3D12_RANGE range = {0, 0};
    _resource->Map(0, &range, &data);
    return data;
}

void DX12Buffer::Unmap()
{
    _resource->Unmap(0, nullptr);
}
} // namespace Luna
