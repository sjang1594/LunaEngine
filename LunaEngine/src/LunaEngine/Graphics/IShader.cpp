#include "LunaPCH.h"
#include "IShader.h"
// IShader::Create() is a stub — shader compilation is handled by DX12Pipeline/DXC directly.
namespace Luna
{
std::shared_ptr<IShader> IShader::Create(const std::string& /*path*/)
{
    return nullptr;
}
}