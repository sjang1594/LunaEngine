#include "LunaPCH.h"
#include "IShader.h"
#include "Renderer/DX12/Public/DX12Shader.h"
#include "Renderer/IRenderContext.h"

namespace Luna
{
std::shared_ptr<IShader> IShader::Create(const std::string& path)
{
    switch (IRenderContext::GetCurrentBackendType())
    {
    case RenderBackendType::DirectX12:
        return std::make_shared<DX12Shader>(std::wstring(path.begin(), path.end()) + L".vs.hlsl",
            std::wstring(path.begin(), path.end()) + L".ps.hlsl");

    default:
        return nullptr;
    }
}
}