#include "LunaPCH.h"
#include "LunaEngine/Renderer/DX12/Public/DX12AccelStructure.h"
#include "Logger/Logger.h"

namespace Luna
{

// ---------------------------------------------------------------------------
// Helper: allocate a committed DEFAULT-heap buffer for AS storage / scratch
// ---------------------------------------------------------------------------
static ComPtr<ID3D12Resource> AllocASBuffer(ID3D12Device* device, UINT64 size,
                                             D3D12_RESOURCE_FLAGS flags)
{
    D3D12_HEAP_PROPERTIES heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width              = size;
    desc.Height             = 1;
    desc.DepthOrArraySize   = 1;
    desc.MipLevels          = 1;
    desc.SampleDesc.Count   = 1;
    desc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags              = flags;

    ComPtr<ID3D12Resource> res;
    HRESULT hr = device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
        nullptr, IID_PPV_ARGS(&res));

    if (FAILED(hr))
    {
        LUNA_LOG_ERROR("AllocASBuffer failed: 0x%08lX", (unsigned long)hr);
        return nullptr;
    }
    return res;
}

static ComPtr<ID3D12Resource> AllocScratchBuffer(ID3D12Device* device, UINT64 size)
{
    D3D12_HEAP_PROPERTIES heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(
        size, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    ComPtr<ID3D12Resource> res;
    HRESULT hr = device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr, IID_PPV_ARGS(&res));

    if (FAILED(hr))
    {
        LUNA_LOG_ERROR("AllocScratchBuffer failed: 0x%08lX", (unsigned long)hr);
        return nullptr;
    }
    return res;
}

// ---------------------------------------------------------------------------
// BuildBLAS — one BLAS per Mesh (triangle geometry)
// ---------------------------------------------------------------------------
bool DX12AccelStructure::BuildBLAS(ID3D12Device5*              device,
                                    ID3D12GraphicsCommandList4* cmdList,
                                    const Mesh&                 mesh)
{
    D3D12_RAYTRACING_GEOMETRY_DESC geomDesc = {};
    geomDesc.Type  = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geomDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE; // skip AnyHit for shadow perf

    auto& tri = geomDesc.Triangles;
    tri.VertexBuffer.StartAddress  = mesh.vertexBuffer->GetGPUVirtualAddress();
    tri.VertexBuffer.StrideInBytes = sizeof(PBRVertex);
    tri.VertexCount                = mesh.vbView.SizeInBytes / sizeof(PBRVertex);
    tri.VertexFormat               = DXGI_FORMAT_R32G32B32_FLOAT; // PBRVertex.position at offset 0
    tri.IndexBuffer                = mesh.indexBuffer->GetGPUVirtualAddress();
    tri.IndexCount                 = mesh.indexCount;
    tri.IndexFormat                = DXGI_FORMAT_R32_UINT;
    tri.Transform3x4               = 0; // no per-primitive transform

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
    inputs.Type           = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.DescsLayout    = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.NumDescs       = 1;
    inputs.pGeometryDescs = &geomDesc;
    inputs.Flags          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
    device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);

    BLASEntry entry;
    entry.blas    = AllocASBuffer(device, info.ResultDataMaxSizeInBytes,
                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    entry.scratch = AllocScratchBuffer(device, info.ScratchDataSizeInBytes);

    if (!entry.blas || !entry.scratch) return false;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
    buildDesc.Inputs                           = inputs;
    buildDesc.DestAccelerationStructureData    = entry.blas->GetGPUVirtualAddress();
    buildDesc.ScratchAccelerationStructureData = entry.scratch->GetGPUVirtualAddress();

    cmdList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

    // UAV barrier — TLAS must wait for all BLAS builds to finish
    D3D12_RESOURCE_BARRIER uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(entry.blas.Get());
    cmdList->ResourceBarrier(1, &uavBarrier);

    _blases.push_back(std::move(entry));
    return true;
}

// ---------------------------------------------------------------------------
// BuildTLAS — one instance per BLAS (identity transform)
// ---------------------------------------------------------------------------
bool DX12AccelStructure::BuildTLAS(ID3D12Device5*              device,
                                    ID3D12GraphicsCommandList4* cmdList)
{
    if (_blases.empty())
    {
        LUNA_LOG_WARN("BuildTLAS: no BLASes to reference");
        return false;
    }

    const UINT instanceCount = static_cast<UINT>(_blases.size());

    // Write instance descriptors to an UPLOAD heap buffer (read by the GPU)
    const UINT64 instBufSize = sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * instanceCount;
    D3D12_HEAP_PROPERTIES uploadProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC   instDesc    = CD3DX12_RESOURCE_DESC::Buffer(instBufSize);

    HRESULT hr = device->CreateCommittedResource(
        &uploadProps, D3D12_HEAP_FLAG_NONE, &instDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&_instanceDescs));

    if (FAILED(hr))
    {
        LUNA_LOG_ERROR("BuildTLAS: instance desc buffer alloc failed: 0x%08lX", (unsigned long)hr);
        return false;
    }

    D3D12_RAYTRACING_INSTANCE_DESC* mapped = nullptr;
    D3D12_RANGE noRead = {0, 0};
    _instanceDescs->Map(0, &noRead, reinterpret_cast<void**>(&mapped));

    for (UINT i = 0; i < instanceCount; ++i)
    {
        D3D12_RAYTRACING_INSTANCE_DESC& inst = mapped[i];
        // Identity 3x4 transform (row-major)
        memset(&inst, 0, sizeof(inst));
        inst.Transform[0][0] = 1.0f;
        inst.Transform[1][1] = 1.0f;
        inst.Transform[2][2] = 1.0f;
        inst.InstanceID                          = i;
        inst.InstanceMask                        = 0xFF;
        inst.InstanceContributionToHitGroupIndex = 0;
        inst.Flags                               = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
        inst.AccelerationStructure               = _blases[i].blas->GetGPUVirtualAddress();
    }
    _instanceDescs->Unmap(0, nullptr);

    // Query TLAS pre-build info
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasInputs = {};
    tlasInputs.Type           = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    tlasInputs.DescsLayout    = D3D12_ELEMENTS_LAYOUT_ARRAY;
    tlasInputs.NumDescs       = instanceCount;
    tlasInputs.InstanceDescs  = _instanceDescs->GetGPUVirtualAddress();
    tlasInputs.Flags          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlasInfo = {};
    device->GetRaytracingAccelerationStructurePrebuildInfo(&tlasInputs, &tlasInfo);

    _tlas        = AllocASBuffer(device, tlasInfo.ResultDataMaxSizeInBytes,
                                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    _tlasScratch = AllocScratchBuffer(device, tlasInfo.ScratchDataSizeInBytes);

    if (!_tlas || !_tlasScratch) return false;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC tlasBuildDesc = {};
    tlasBuildDesc.Inputs                           = tlasInputs;
    tlasBuildDesc.DestAccelerationStructureData    = _tlas->GetGPUVirtualAddress();
    tlasBuildDesc.ScratchAccelerationStructureData = _tlasScratch->GetGPUVirtualAddress();

    cmdList->BuildRaytracingAccelerationStructure(&tlasBuildDesc, 0, nullptr);

    D3D12_RESOURCE_BARRIER uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(_tlas.Get());
    cmdList->ResourceBarrier(1, &uavBarrier);

    LUNA_LOG_INFO("TLAS built: %u instances", instanceCount);
    return true;
}

} // namespace Luna
