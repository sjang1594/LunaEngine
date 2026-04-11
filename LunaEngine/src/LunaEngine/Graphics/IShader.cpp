#include "LunaPCH.h"
#include "IShader.h"
#include "Renderer/HAL/Public/IRenderContext.h"

// Platform-specific shader implementations
#ifdef _WIN32
#include "Renderer/DX12/Public/DX12Shader.h"
#endif

namespace Luna
{
std::shared_ptr<IShaderProgram> IShaderProgram::Create(const std::string& path)
{
    switch (IRenderContext::GetCurrentBackendType())
    {
#ifdef _WIN32
    case RenderBackendType::DirectX12:
    {
        const std::wstring wpath(path.begin(), path.end());
        return std::make_shared<DX12ShaderProgram>(wpath + L".vs.hlsl",
                                                   wpath + L".ps.hlsl");
    }
#endif
    default:
        return nullptr;
    }
}
} // namespace Luna