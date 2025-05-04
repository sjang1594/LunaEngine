#include "LunaPCH.h"
#include "DX12VertexBuffer.h"

#include "DX12Backend.h"

namespace Luna
{
    extern DX12Backend* g_DX12Backend;
    
    DX12VertexBuffer::DX12VertexBuffer(void* vertices, uint32_t size)
    {
        auto device = g_DX12Backend->GetDevice();

        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC resourceDesc = {};
        resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        resourceDesc.Width = size;
        resourceDesc.Height = 1;
        resourceDesc.DepthOrArraySize = 1;
        resourceDesc.MipLevels  = 1;
        resourceDesc.SampleDesc.Count = 1;
        resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        HRESULT hr = device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &resourceDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&_vertexBuffer)
            );

        assert(SUCCEEDED(hr) && "Failed to Create Vertex Buffer");

        void* mappedData = nullptr;
        D3D12_RANGE range = {0, 0};
        _vertexBuffer->Map(0, &range, &mappedData);
        memcpy(mappedData, vertices, size);
        _vertexBuffer->Unmap(0, nullptr);

        _vbView.BufferLocation = _vertexBuffer->GetGPUVirtualAddress();
        _vbView.SizeInBytes = size;
        _vbView.StrideInBytes = sizeof(float) * 5;
    }

    DX12VertexBuffer::~DX12VertexBuffer()
    {
        _vertexBuffer.Reset();
    }

    void DX12VertexBuffer::Bind()
    {
        g_DX12Backend->GetCommandList()->IASetVertexBuffers(0, 1, &_vbView);
    }
}
