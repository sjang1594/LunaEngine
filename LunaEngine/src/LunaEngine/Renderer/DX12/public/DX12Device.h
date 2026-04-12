#pragma once
#include "Renderer/HAL/Public/IRenderDevice.h"
#include <string>

namespace Luna
{
class DX12Device : IRenderDevice
{
public:
    DX12Device();
    ~DX12Device() override = default;

    // P3-04: returns the real GPU name from DXGI_ADAPTER_DESC1::Description
    const char*           GetDeviceName()  const override { return _adapterNameNarrow.c_str(); }
    const std::wstring&   GetDeviceNameW() const          { return _adapterName; }

    ComPtr<ID3D12Device>  GetDeviceComPtr()  const { return _device; }
    ComPtr<IDXGIFactory6> GetDXGIFactory()   const { return _mdxgiFactory; }
    ComPtr<IDXGIAdapter1> GetAdapterComPtr() const { return _adapter; }
    IDXGIAdapter*         GetAdapter()       const { return _adapter.Get(); }
    UINT                  GetMSAAQuality()   const { return m4xMsaaQuality; }

    // Phase 4A: DXR capability check
    bool SupportsDXR() const;

private:
    void CreateDebugLayer();
    void CreateDXGIFactory();
    void CreateDevice();
    void SetMultiSampleQualityLevels();

private:
    UINT m4xMsaaQuality = 0;
#if defined(_DEBUG)
    ComPtr<ID3D12Debug>     _debugController;
#endif
    ComPtr<ID3D12Device>    _device;
    ComPtr<IDXGIAdapter1>   _adapter;
    ComPtr<IDXGIFactory6>   _mdxgiFactory;

    // P3-04: GPU name captured at adapter-enumeration time
    std::wstring _adapterName;
    std::string  _adapterNameNarrow;
};
}
