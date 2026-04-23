#pragma once

namespace  Luna
{
enum class ShaderType
{
    Vertex,
    Fragment,
    Geometry,
    Compute,
    Tessellation
};

/** This is the interface for Compile for the shader program **/
class IShaderProgram
{
public:
    virtual ~IShaderProgram() = default;
    virtual bool Compile(const std::string& src, ShaderType type) = 0;
    virtual void Bind() = 0;
    virtual void UnBind() = 0;
    virtual void Reload(const std::string& filePath) = 0;
    virtual void Destroy() = 0;

    // P0-05: Factory stub — shader compilation is handled by DX12Pipeline/DXC directly.
    // Returns nullptr on all backends; here for API symmetry only.
    static std::shared_ptr<IShaderProgram> Create(const std::string& path);
};
}