#include "LunaPCH.h"
#include "LunaEngine/Renderer/DX12/Public/DX12Device.h"
#include "Logger/Logger.h"

namespace Luna
{
DX12Device::DX12Device()
{
    CreateDebugLayer();
    CreateDXGIFactory();
    CreateDevice();
    SetMultiSampleQualityLevels();
    LUNA_LOG_INFO("Device has been initialized successfully");
}

void DX12Device::CreateDebugLayer()
{
#if defined(_DEBUG) || defined(DEBUG)
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&_debugController))))
    {
        _debugController->EnableDebugLayer();
    }
#endif
}

void DX12Device::CreateDXGIFactory()
{
    UINT flags = 0;

#if defined(_DEBUG) || defined(DEBUG)
    flags |= DXGI_CREATE_FACTORY_DEBUG;
#endif
    HRESULT hr = CreateDXGIFactory2(flags, IID_PPV_ARGS(&_mdxgiFactory));
    DX_CHECK(hr);
    LUNA_LOG_INFO("DXGIFactory has been created successfully");
    
    for (UINT i = 0;; ++i)
    {
        ComPtr<IDXGIAdapter1> candidate;
        hr = _mdxgiFactory->EnumAdapters1(i, candidate.GetAddressOf());
        if (hr == DXGI_ERROR_NOT_FOUND)
            break;
        DX_CHECK(hr);

        DXGI_ADAPTER_DESC1 desc;
        candidate->GetDesc1(&desc);

        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            continue;

        if (desc.DedicatedVideoMemory > 0)
        {
            _adapter = candidate;

            // P3-04: persist the real adapter name so GetDeviceName() returns something useful
            _adapterName = desc.Description;
            _adapterNameNarrow.resize(_adapterName.size());
            std::transform(_adapterName.begin(), _adapterName.end(),
                           _adapterNameNarrow.begin(),
                           [](wchar_t c) { return static_cast<char>(c < 128 ? c : '?'); });

            LUNA_LOG_INFO("Selected adapter: %ls", desc.Description);
            return;
        }
    }

    LUNA_LOG_ERROR("No suitable GPU adapter found.");
}

void DX12Device::CreateDevice()
{
    LUNA_LOG_INFO("Creating DX12 device...");
    HRESULT hr = D3D12CreateDevice(_adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&_device));
    DX_CHECK(hr);
    LUNA_LOG_INFO("DX12 device has been created successfully");
}

void DX12Device::SetMultiSampleQualityLevels()
{
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS msQualityLevels;
    msQualityLevels.Format = DXGI_FORMAT_R8G8B8A8_UNORM; // Same As BackBuffer Format
    msQualityLevels.SampleCount = 4;
    msQualityLevels.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
    msQualityLevels.NumQualityLevels = 0;

    HRESULT hr = _device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
        &msQualityLevels, sizeof(msQualityLevels));

    if (FAILED(hr) || msQualityLevels.NumQualityLevels == 0)
    {
        m4xMsaaQuality = 0;
    }
    else
    {
        m4xMsaaQuality = msQualityLevels.NumQualityLevels;
    }
}

// Phase 4A: Query DXR support via ID3D12Device5::CheckFeatureSupport
bool DX12Device::SupportsDXR() const
{
    ComPtr<ID3D12Device5> device5;
    if (FAILED(_device.As(&device5)))
        return false;

    D3D12_FEATURE_DATA_D3D12_OPTIONS5 opts5 = {};
    if (FAILED(device5->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opts5, sizeof(opts5))))
        return false;

    return opts5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_0;
}
}