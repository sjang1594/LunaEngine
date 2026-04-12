#pragma once
#include "D3D12MemAlloc.h"
#include <string>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct D3D12_CPU_DESCRIPTOR_HANDLE;

namespace Luna
{

class Texture
{
  public:
    Texture() = default;
    ~Texture();

    // Non-copyable
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;
    Texture(Texture&&) = default;
    Texture& operator=(Texture&&) = default;

    // Load an image from disk (RGBA8), upload to DEFAULT heap.
    // Commands are recorded into cmdList; caller must execute + wait.
    // stagingOut receives the UPLOAD buffer (keep alive until GPU-done).
    bool Load(const std::string& path,
              ID3D12Device* device,
              D3D12MA::Allocator* allocator,
              ID3D12GraphicsCommandList* cmdList,
              ComPtr<ID3D12Resource>& stagingOut,
              D3D12MA::Allocation**   stagingAllocOut);

    // Load an image from a memory buffer (GLB embedded textures — JPEG/PNG raw bytes).
    // Same upload semantics as Load(); uses stbi_load_from_memory internally.
    bool LoadFromMemory(const uint8_t* data, size_t byteSize,
                        ID3D12Device* device,
                        D3D12MA::Allocator* allocator,
                        ID3D12GraphicsCommandList* cmdList,
                        ComPtr<ID3D12Resource>& stagingOut,
                        D3D12MA::Allocation**   stagingAllocOut);

    // Create a shader-resource view at dest (CPU handle, caller owns the descriptor heap slot)
    void CreateSRV(ID3D12Device* device, D3D12_CPU_DESCRIPTOR_HANDLE dest);

    bool IsLoaded() const { return _resource != nullptr; }
    UINT Width()    const { return _width; }
    UINT Height()   const { return _height; }

  private:
    ComPtr<ID3D12Resource> _resource;
    D3D12MA::Allocation*   _allocation = nullptr;
    UINT _width  = 0;
    UINT _height = 0;
};

} // namespace Luna
