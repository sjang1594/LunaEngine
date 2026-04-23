#pragma once
#include "Graphics/IShader.h"

namespace Luna
{
class DX12ShaderProgram: public IShaderProgram
{
public:
    DX12ShaderProgram(const std::wstring& vsPath, const std::wstring& psPath);
    ~DX12ShaderProgram() override = default;

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
