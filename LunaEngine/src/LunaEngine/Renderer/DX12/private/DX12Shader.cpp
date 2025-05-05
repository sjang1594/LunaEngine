#include "LunaPCH.h"
#include "Renderer/DX12/public/DX12Shader.h"

namespace Luna
{

 DX12Shader::DX12Shader(const std::wstring &vsPath, const std::wstring &psPath)
 {
     if (!CompileShader(vsPath, "vs_5_0", _vsBlob))
     {
         std::cerr << "[DX12Shader] Compile shader failed!" << std::string(vsPath.begin(), vsPath.end()) << "\n";
     }

     if (!CompileShader(psPath, "ps_5_0", _psBlob))
     {
         std::cerr << "[DX12Shader] Pixel shader compilation failed: " << std::string(psPath.begin(), psPath.end()) << "\n";
     }
 }

bool DX12Shader::CompileShader(const std::wstring& path, const std::string& target, ComPtr<ID3DBlob>& outBlob) {
     ComPtr<ID3DBlob> errorBlob;
     HRESULT hr = D3DCompileFromFile(
         path.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
         "main", target.c_str(), D3DCOMPILE_ENABLE_STRICTNESS, 0,
         &outBlob, &errorBlob
     );

     if (FAILED(hr)) {
         if (errorBlob) {
             std::cerr << "[Shader Compilation] Error: "
                       << static_cast<const char*>(errorBlob->GetBufferPointer()) << std::endl;
         }
         return false;
     }

     return true;
 }
}