#pragma once
#include "Graphics/Buffer.h"

namespace Luna
{
    class DX12VertexBuffer : public VertexBuffer
    {
    public:
        DX12VertexBuffer(void* vertices, uint32_t size);
        ~DX12VertexBuffer();

        virtual void Bind() override;

    private:
        ComPtr<ID3D12Resource> _vertexBuffer;
        D3D12_VERTEX_BUFFER_VIEW _vertexBufferView;
    };
}