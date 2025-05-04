#pragma once

// Root Signature Creation
// Pipeline State Object (PSO)
// Shader Loading via D3DCompileFromFile

namespace Luna {
class DX12Pipeline {
public:
  DX12Pipeline() = default;
  ~DX12Pipeline() = default;

  bool Initialize(ComPtr<ID3D12Device> device, const std::wstring& vsPath, const std::wstring& psPath);

  ComPtr<ID3D12PipelineState> GetPipelineState() const { return _pipelineState; }
  ComPtr<ID3D12RootSignature> GetRootSignature() const { return _rootSignature; }
  
private:
  bool LoadShader(const std::wstring& path, const std::string& target, ComPtr<ID3DBlob>& blob);
  bool CreateRootSignature(const ComPtr<ID3D12Device> &device);
  bool CreatePipelineState(ComPtr<ID3D12Device> device, ComPtr<ID3DBlob> vs, ComPtr<ID3DBlob> ps);

private:
  ComPtr<ID3D12PipelineState> _pipelineState;
  ComPtr<ID3D12RootSignature> _rootSignature;
};
}