#include "LunaPCH.h"

// stb_image single-file implementation — compiled here and only here
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "Graphics/Texture.h"
#include "Logger/Logger.h"

namespace Luna
{

Texture::~Texture()
{
    if (_allocation)
    {
        _allocation->Release();
        _allocation = nullptr;
    }
}

bool Texture::Load(const std::string& path,
                   ID3D12Device* device,
                   D3D12MA::Allocator* allocator,
                   ID3D12GraphicsCommandList* cmdList,
                   ComPtr<ID3D12Resource>& stagingOut,
                   D3D12MA::Allocation** stagingAllocOut)
{
    // ---- Load pixels via stb_image ----
    int w, h, channels;
    stbi_uc* pixels = stbi_load(path.c_str(), &w, &h, &channels, 4); // force RGBA8
    if (!pixels)
    {
        LUNA_LOG_ERROR("Texture::Load — stbi_load failed: %s", path.c_str());
        return false;
    }

    _width  = static_cast<UINT>(w);
    _height = static_cast<UINT>(h);

    // ---- Compute row pitch aligned to D3D12_TEXTURE_DATA_PITCH_ALIGNMENT ----
    const UINT bytesPerPixel = 4; // RGBA8
    const UINT rowPitch      = (_width * bytesPerPixel + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)
                                & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    const UINT64 totalBytes  = static_cast<UINT64>(rowPitch) * _height;

    // ---- Create DEFAULT heap Texture2D ----
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width              = _width;
    texDesc.Height             = _height;
    texDesc.DepthOrArraySize   = 1;
    texDesc.MipLevels          = 1;
    texDesc.Format             = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count   = 1;
    texDesc.Flags              = D3D12_RESOURCE_FLAG_NONE;

    D3D12MA::ALLOCATION_DESC dstAllocDesc = {};
    dstAllocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    HRESULT hr = allocator->CreateResource(&dstAllocDesc, &texDesc,
                                           D3D12_RESOURCE_STATE_COPY_DEST,
                                           nullptr, &_allocation, IID_PPV_ARGS(&_resource));
    if (FAILED(hr))
    {
        LUNA_LOG_ERROR("Texture: DEFAULT heap alloc failed: 0x%08lX", (unsigned long)hr);
        stbi_image_free(pixels);
        return false;
    }

    // ---- Create UPLOAD staging buffer ----
    D3D12_RESOURCE_DESC stagingDesc = CD3DX12_RESOURCE_DESC::Buffer(totalBytes);
    D3D12MA::ALLOCATION_DESC stagingAllocDesc = {};
    stagingAllocDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;

    ComPtr<ID3D12Resource> stagingBuf;
    hr = allocator->CreateResource(&stagingAllocDesc, &stagingDesc,
                                   D3D12_RESOURCE_STATE_GENERIC_READ,
                                   nullptr, stagingAllocOut, IID_PPV_ARGS(&stagingBuf));
    if (FAILED(hr))
    {
        LUNA_LOG_ERROR("Texture: UPLOAD staging alloc failed: 0x%08lX", (unsigned long)hr);
        stbi_image_free(pixels);
        return false;
    }

    // ---- Copy pixel rows to staging (with row-pitch alignment) ----
    void* mapped = nullptr;
    D3D12_RANGE noRead = {0, 0};
    stagingBuf->Map(0, &noRead, &mapped);
    for (UINT row = 0; row < _height; ++row)
    {
        const stbi_uc* src = pixels + row * _width * bytesPerPixel;
        uint8_t*       dst = static_cast<uint8_t*>(mapped) + row * rowPitch;
        memcpy(dst, src, _width * bytesPerPixel);
    }
    stagingBuf->Unmap(0, nullptr);

    // ---- Record copy command ----
    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource                          = stagingBuf.Get();
    srcLoc.Type                               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint.Offset             = 0;
    srcLoc.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
    srcLoc.PlacedFootprint.Footprint.Width    = _width;
    srcLoc.PlacedFootprint.Footprint.Height   = _height;
    srcLoc.PlacedFootprint.Footprint.Depth    = 1;
    srcLoc.PlacedFootprint.Footprint.RowPitch = rowPitch;

    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource        = _resource.Get();
    dstLoc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;

    cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        _resource.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmdList->ResourceBarrier(1, &barrier);

    stbi_image_free(pixels);
    stagingOut = stagingBuf; // caller keeps alive until GPU-done
    return true;
}

bool Texture::LoadFromMemory(const uint8_t* data, size_t byteSize,
                              ID3D12Device* device,
                              D3D12MA::Allocator* allocator,
                              ID3D12GraphicsCommandList* cmdList,
                              ComPtr<ID3D12Resource>& stagingOut,
                              D3D12MA::Allocation** stagingAllocOut)
{
    // ---- Decode image from memory (JPEG/PNG raw bytes from GLB buffer_view) ----
    int w, h, channels;
    stbi_uc* pixels = stbi_load_from_memory(
        data, static_cast<int>(byteSize), &w, &h, &channels, 4); // force RGBA8
    if (!pixels)
    {
        LUNA_LOG_ERROR("Texture::LoadFromMemory — stbi_load_from_memory failed (%zu bytes)",
                       byteSize);
        return false;
    }

    _width  = static_cast<UINT>(w);
    _height = static_cast<UINT>(h);

    const UINT bytesPerPixel = 4;
    const UINT rowPitch      = (_width * bytesPerPixel + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)
                                & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    const UINT64 totalBytes  = static_cast<UINT64>(rowPitch) * _height;

    // ---- DEFAULT heap Texture2D ----
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width              = _width;
    texDesc.Height             = _height;
    texDesc.DepthOrArraySize   = 1;
    texDesc.MipLevels          = 1;
    texDesc.Format             = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count   = 1;
    texDesc.Flags              = D3D12_RESOURCE_FLAG_NONE;

    D3D12MA::ALLOCATION_DESC dstAllocDesc = {};
    dstAllocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    HRESULT hr = allocator->CreateResource(&dstAllocDesc, &texDesc,
                                           D3D12_RESOURCE_STATE_COPY_DEST,
                                           nullptr, &_allocation, IID_PPV_ARGS(&_resource));
    if (FAILED(hr))
    {
        LUNA_LOG_ERROR("Texture::LoadFromMemory — DEFAULT heap alloc failed: 0x%08lX",
                       (unsigned long)hr);
        stbi_image_free(pixels);
        return false;
    }

    // ---- UPLOAD staging ----
    D3D12_RESOURCE_DESC stagingDesc = CD3DX12_RESOURCE_DESC::Buffer(totalBytes);
    D3D12MA::ALLOCATION_DESC stagingAllocDesc = {};
    stagingAllocDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;

    ComPtr<ID3D12Resource> stagingBuf;
    hr = allocator->CreateResource(&stagingAllocDesc, &stagingDesc,
                                   D3D12_RESOURCE_STATE_GENERIC_READ,
                                   nullptr, stagingAllocOut, IID_PPV_ARGS(&stagingBuf));
    if (FAILED(hr))
    {
        LUNA_LOG_ERROR("Texture::LoadFromMemory — UPLOAD staging alloc failed: 0x%08lX",
                       (unsigned long)hr);
        stbi_image_free(pixels);
        return false;
    }

    // ---- Copy rows with pitch alignment ----
    void* mapped = nullptr;
    D3D12_RANGE noRead = {0, 0};
    stagingBuf->Map(0, &noRead, &mapped);
    for (UINT row = 0; row < _height; ++row)
    {
        const stbi_uc* src = pixels + row * _width * bytesPerPixel;
        uint8_t*       dst = static_cast<uint8_t*>(mapped) + row * rowPitch;
        memcpy(dst, src, _width * bytesPerPixel);
    }
    stagingBuf->Unmap(0, nullptr);

    // ---- Record GPU copy + transition ----
    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource                          = stagingBuf.Get();
    srcLoc.Type                               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint.Offset             = 0;
    srcLoc.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
    srcLoc.PlacedFootprint.Footprint.Width    = _width;
    srcLoc.PlacedFootprint.Footprint.Height   = _height;
    srcLoc.PlacedFootprint.Footprint.Depth    = 1;
    srcLoc.PlacedFootprint.Footprint.RowPitch = rowPitch;

    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource        = _resource.Get();
    dstLoc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;

    cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        _resource.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmdList->ResourceBarrier(1, &barrier);

    stbi_image_free(pixels);
    stagingOut = stagingBuf;
    return true;
}

void Texture::CreateSRV(ID3D12Device* device, D3D12_CPU_DESCRIPTOR_HANDLE dest)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels     = 1;
    device->CreateShaderResourceView(_resource.Get(), &srvDesc, dest);
}

} // namespace Luna
