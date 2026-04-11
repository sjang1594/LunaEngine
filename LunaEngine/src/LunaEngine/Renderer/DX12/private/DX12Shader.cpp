#include "LunaPCH.h"
#include "Renderer/DX12/Public/DX12Shader.h"
#include "Logger/Logger.h"

namespace Luna
{

DX12ShaderProgram::DX12ShaderProgram(const std::wstring &vsPath, const std::wstring &psPath)
{
    if (!CompileShader(vsPath, "vs_5_0", _vsBlob))
        LUNA_LOG_ERROR("Vertex shader compilation failed: %ls", vsPath.c_str());

    if (!CompileShader(psPath, "ps_5_0", _psBlob))
        LUNA_LOG_ERROR("Pixel shader compilation failed: %ls", psPath.c_str());
}

bool DX12ShaderProgram::CompileShader(const std::wstring &path, const std::string &target,
                                ComPtr<ID3DBlob> &outBlob)
{
    ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3DCompileFromFile(path.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                    "main", target.c_str(),
                                    D3DCOMPILE_ENABLE_STRICTNESS, 0, &outBlob, &errorBlob);
    if (FAILED(hr))
    {
        if (errorBlob)
            LUNA_LOG_ERROR("Shader compile error in %ls:\n%s",
                           path.c_str(),
                           static_cast<const char *>(errorBlob->GetBufferPointer()));
        else
            LUNA_LOG_ERROR("Shader compile failed: HRESULT 0x%08lX", static_cast<unsigned long>(hr));
        return false;
    }
    return true;
}

} // namespace Luna
