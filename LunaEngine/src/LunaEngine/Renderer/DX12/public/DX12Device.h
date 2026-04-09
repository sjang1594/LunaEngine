#pragma once
#include "Renderer/HAL/Public/IRenderDevice.h"

namespace Luna
{
class DX12Device : IRenderDevice
{
public:
    DX12Device();
    ~DX12Device() override = default;
    const char* GetDeviceName() const override { return "DirectX12"; }

    ComPtr<ID3D12Device>  GetDeviceComPtr() const { return _device; }
    ComPtr<IDXGIFactory6> GetDXGIFactory()  const { return _mdxgiFactory; }
    UINT                  GetMSAAQuality()  const { return m4xMsaaQuality; }

private:
    void CreateDebugLayer();
    void CreateDXGIFactory();
    void CreateDevice();
    void SetMultiSampleQualityLevels();

private:
    UINT m4xMsaaQuality     = 0;
#if defined(_DEBUG)
    ComPtr<ID3D12Debug>     _debugController;
#endif
    ComPtr<ID3D12Device>    _device;
    ComPtr<IDXGIAdapter1>   _adapter;
    ComPtr<IDXGIFactory6>   _mdxgiFactory;
};
}
