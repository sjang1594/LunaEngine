#pragma once
#include "Graphics/IShader.h"

namespace Luna
{

struct ShaderConstantData
{
    
};

class DX12Shader: public IShader
{
public:
    DX12Shader(const std::wstring& vsPath, const std::wstring& psPath);
    ~DX12Shader() override = default;

    void Bind() override {}
    void UnBind() override {}

    ComPtr<ID3DBlob> GetVSBlob() const { return  _vsBlob; }
    ComPtr<ID3DBlob> GetPSBlob() const { return  _psBlob; }

private:
    ComPtr<ID3DBlob> _vsBlob;
    ComPtr<ID3DBlob> _psBlob;
    bool CompileShader(const std::wstring& path, const std::string& target, Microsoft::WRL::ComPtr<ID3DBlob>& outBlob);
};
}
