#include "LunaPCH.h"
#include "LunaEngine/Renderer/DX12/Public/DX12RTPipeline.h"
#include "Renderer/DX12/Public/DX12Pipeline.h"   // for EnsureDXCInitialized / s_DxcCompiler
#include "LunaEngine/Utils/FileSystemUtil.h"
#include "Logger/Logger.h"
#include <dxcapi.h>

namespace Luna
{

// ---------------------------------------------------------------------------
// Helper: create a root signature from a serialized blob
// ---------------------------------------------------------------------------
static ComPtr<ID3D12RootSignature> CreateRootSignatureFromBlob(
    ID3D12Device* device, const void* blob, SIZE_T blobSize)
{
    ComPtr<ID3D12RootSignature> rs;
    device->CreateRootSignature(0, blob, blobSize, IID_PPV_ARGS(&rs));
    return rs;
}

// ---------------------------------------------------------------------------
// Initialize — compile shadows.hlsl + create RTPSO + shader table
// ---------------------------------------------------------------------------
bool DX12RTPipeline::Initialize(ID3D12Device5* device, const std::wstring& shaderPath)
{
    if (!CompileShaderLibrary(reinterpret_cast<ID3D12Device5*>(device), shaderPath))
        return false;

    // ---- Global root signature ----
    // Layout: b0=ShadowCB, t0=TLAS, t1=depthBuffer, u0=shadowOutput
    {
        CD3DX12_DESCRIPTOR_RANGE ranges[3];
        ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV,  1, 0); // t0 = TLAS  (AS SRV)
        ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV,  1, 1); // t1 = depth
        ranges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV,  1, 0); // u0 = shadow output

        CD3DX12_ROOT_PARAMETER params[4];
        params[0].InitAsConstantBufferView(0);            // b0 = ShadowCB
        params[1].InitAsShaderResourceView(0, 0, D3D12_SHADER_VISIBILITY_ALL); // t0 = TLAS inline
        params[2].InitAsDescriptorTable(1, &ranges[1]);  // t1 = depth SRV table
        params[3].InitAsDescriptorTable(1, &ranges[2]);  // u0 = UAV table

        CD3DX12_ROOT_SIGNATURE_DESC rsDesc(4, params, 0, nullptr,
                                           D3D12_ROOT_SIGNATURE_FLAG_NONE);
        ComPtr<ID3DBlob> serialized, errors;
        HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                                 &serialized, &errors);
        if (FAILED(hr))
        {
            LUNA_LOG_ERROR("DXR: global root sig serialize failed");
            return false;
        }
        _globalRootSig = CreateRootSignatureFromBlob(
            reinterpret_cast<ID3D12Device*>(device),
            serialized->GetBufferPointer(), serialized->GetBufferSize());
        if (!_globalRootSig) return false;
    }

    // ---- RTPSO ----
    // Subobjects: DXIL library, hit group, shader config, pipeline config, global root sig
    {
        std::vector<D3D12_STATE_SUBOBJECT> subobjects;

        // 1. DXIL library
        D3D12_DXIL_LIBRARY_DESC libDesc = {};
        libDesc.DXILLibrary.pShaderBytecode = _shaderLib->GetBufferPointer();
        libDesc.DXILLibrary.BytecodeLength  = _shaderLib->GetBufferSize();
        // Export all shaders from the library
        libDesc.NumExports = 0; // export all

        D3D12_STATE_SUBOBJECT libSO;
        libSO.Type  = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
        libSO.pDesc = &libDesc;
        subobjects.push_back(libSO);

        // 2. Hit group
        D3D12_HIT_GROUP_DESC hitGroupDesc = {};
        hitGroupDesc.HitGroupExport    = L"ShadowHitGroup";
        hitGroupDesc.Type              = D3D12_HIT_GROUP_TYPE_TRIANGLES;
        hitGroupDesc.ClosestHitShaderImport = L"ClosestHit";

        D3D12_STATE_SUBOBJECT hitGroupSO;
        hitGroupSO.Type  = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
        hitGroupSO.pDesc = &hitGroupDesc;
        subobjects.push_back(hitGroupSO);

        // 3. Shader config (payload + attribute sizes)
        D3D12_RAYTRACING_SHADER_CONFIG shaderConfig = {};
        shaderConfig.MaxPayloadSizeInBytes   = sizeof(bool); // ShadowPayload.shadowed
        shaderConfig.MaxAttributeSizeInBytes = sizeof(float) * 2; // barycentrics

        D3D12_STATE_SUBOBJECT shaderConfigSO;
        shaderConfigSO.Type  = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;
        shaderConfigSO.pDesc = &shaderConfig;
        subobjects.push_back(shaderConfigSO);

        // 4. Pipeline config (max recursion depth = 1 for shadow rays)
        D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig = {};
        pipelineConfig.MaxTraceRecursionDepth = 1;

        D3D12_STATE_SUBOBJECT pipelineConfigSO;
        pipelineConfigSO.Type  = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;
        pipelineConfigSO.pDesc = &pipelineConfig;
        subobjects.push_back(pipelineConfigSO);

        // 5. Global root signature
        D3D12_GLOBAL_ROOT_SIGNATURE globalRSSO = {};
        globalRSSO.pGlobalRootSignature = _globalRootSig.Get();

        D3D12_STATE_SUBOBJECT globalRSSObj;
        globalRSSObj.Type  = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
        globalRSSObj.pDesc = &globalRSSO;
        subobjects.push_back(globalRSSObj);

        D3D12_STATE_OBJECT_DESC soDesc = {};
        soDesc.Type          = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
        soDesc.NumSubobjects = static_cast<UINT>(subobjects.size());
        soDesc.pSubobjects   = subobjects.data();

        HRESULT hr = device->CreateStateObject(&soDesc, IID_PPV_ARGS(&_rtpso));
        if (FAILED(hr))
        {
            LUNA_LOG_ERROR("DXR: CreateStateObject failed: 0x%08lX", (unsigned long)hr);
            return false;
        }
    }

    if (!BuildShaderTable(device)) return false;

    LUNA_LOG_INFO("DXR RT pipeline initialized");
    return true;
}

bool DX12RTPipeline::CompileShaderLibrary(ID3D12Device5* /*device*/, const std::wstring& path)
{
    // Reuse DXC from DX12Pipeline
    DX12Pipeline::EnsureDXCInitialized();

    ComPtr<IDxcUtils>     utils;
    ComPtr<IDxcCompiler3> compiler;
    DxcCreateInstance(CLSID_DxcUtils,    IID_PPV_ARGS(&utils));
    DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler));

    std::wstring fullPath = GetShaderFullPath(path);

    ComPtr<IDxcBlobEncoding> source;
    HRESULT hr = utils->LoadFile(fullPath.c_str(), nullptr, &source);
    if (FAILED(hr))
    {
        LUNA_LOG_ERROR("DXR: failed to load %ls", fullPath.c_str());
        return false;
    }

    DxcBuffer srcBuf = { source->GetBufferPointer(), source->GetBufferSize(), DXC_CP_ACP };

    LPCWSTR args[] = {
        fullPath.c_str(),
        L"-T", L"lib_6_5",   // shader model 6.5 required for DXR 1.1
        L"-HV", L"2021",
#ifdef _DEBUG
        L"-Zs", L"-Od",
#endif
    };

    ComPtr<IDxcResult> result;
    hr = compiler->Compile(&srcBuf, args, _countof(args), nullptr, IID_PPV_ARGS(&result));
    if (FAILED(hr)) { LUNA_LOG_ERROR("DXR: Compile call failed"); return false; }

    HRESULT status = S_OK;
    result->GetStatus(&status);
    ComPtr<IDxcBlobUtf8> errors;
    result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);

    if (FAILED(status))
    {
        if (errors && errors->GetStringLength() > 0)
            LUNA_LOG_ERROR("DXR shader error:\n%s", errors->GetStringPointer());
        return false;
    }
    if (errors && errors->GetStringLength() > 0)
        LUNA_LOG_WARN("DXR shader warnings:\n%s", errors->GetStringPointer());

    result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&_shaderLib), nullptr);
    return _shaderLib != nullptr;
}

bool DX12RTPipeline::BuildShaderTable(ID3D12Device5* device)
{
    // Retrieve shader identifiers from the RTPSO
    ComPtr<ID3D12StateObjectProperties> props;
    _rtpso.As(&props);

    const void* rayGenID   = props->GetShaderIdentifier(L"RayGen");
    const void* missID     = props->GetShaderIdentifier(L"Miss");
    const void* hitGroupID = props->GetShaderIdentifier(L"ShadowHitGroup");

    if (!rayGenID || !missID || !hitGroupID)
    {
        LUNA_LOG_ERROR("DXR: failed to get shader identifiers");
        return false;
    }

    // Each record: identifier (32 B) aligned to D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT
    constexpr UINT idSize   = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
    constexpr UINT align    = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;
    _shaderTableStride      = (idSize + align - 1) & ~(align - 1);
    _shaderTableSize        = static_cast<UINT>(_shaderTableStride * 3); // RayGen + Miss + HitGroup

    D3D12_HEAP_PROPERTIES heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC   desc      = CD3DX12_RESOURCE_DESC::Buffer(_shaderTableSize);

    HRESULT hr = reinterpret_cast<ID3D12Device*>(device)->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&_shaderTable));
    if (FAILED(hr)) { LUNA_LOG_ERROR("DXR: shader table alloc failed"); return false; }

    uint8_t* mapped = nullptr;
    D3D12_RANGE noRead = {0,0};
    _shaderTable->Map(0, &noRead, reinterpret_cast<void**>(&mapped));

    memcpy(mapped,                          rayGenID,   idSize);
    memcpy(mapped + _shaderTableStride,     missID,     idSize);
    memcpy(mapped + _shaderTableStride * 2, hitGroupID, idSize);

    _shaderTable->Unmap(0, nullptr);
    return true;
}

// ---------------------------------------------------------------------------
// DispatchShadows
// ---------------------------------------------------------------------------
void DX12RTPipeline::DispatchShadows(ID3D12GraphicsCommandList4*  cmdList,
                                      UINT64                       tlas,
                                      ID3D12Resource*             /*shadowUAV*/,
                                      ID3D12Resource*             /*depthSRV*/,
                                      UINT                        width,
                                      UINT                        height,
                                      UINT64                       shadowCBGPU)
{
    if (!_rtpso || !_shaderTable) return;

    cmdList->SetComputeRootSignature(_globalRootSig.Get());
    cmdList->SetPipelineState1(_rtpso.Get());

    // Bind resources to global root sig
    cmdList->SetComputeRootConstantBufferView(0, shadowCBGPU);
    cmdList->SetComputeRootShaderResourceView(1, tlas);
    // Params 2 (depth SRV table) and 3 (shadow UAV table) must be bound by caller
    // via SetDescriptorHeaps + SetComputeRootDescriptorTable before calling this.

    D3D12_DISPATCH_RAYS_DESC dispatchDesc = {};
    const UINT64 base = _shaderTable->GetGPUVirtualAddress();

    dispatchDesc.RayGenerationShaderRecord.StartAddress = base;
    dispatchDesc.RayGenerationShaderRecord.SizeInBytes  = _shaderTableStride;

    dispatchDesc.MissShaderTable.StartAddress  = base + _shaderTableStride;
    dispatchDesc.MissShaderTable.SizeInBytes   = _shaderTableStride;
    dispatchDesc.MissShaderTable.StrideInBytes = _shaderTableStride;

    dispatchDesc.HitGroupTable.StartAddress  = base + _shaderTableStride * 2;
    dispatchDesc.HitGroupTable.SizeInBytes   = _shaderTableStride;
    dispatchDesc.HitGroupTable.StrideInBytes = _shaderTableStride;

    dispatchDesc.Width  = width;
    dispatchDesc.Height = height;
    dispatchDesc.Depth  = 1;

    cmdList->DispatchRays(&dispatchDesc);
}

} // namespace Luna
