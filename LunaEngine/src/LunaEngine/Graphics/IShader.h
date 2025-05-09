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

    static std::shared_ptr<IShaderProgram> Create(const std::string& path);
};
}