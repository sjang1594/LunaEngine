#pragma once

namespace  Luna
{
enum class ShaderStage
{
    Vertex, Fragment, Geometry, Compute
};

class IShader
{
public:
    virtual ~IShader() = default;
    virtual void Bind() = 0;
    virtual void UnBind() = 0;

    virtual void SetUniform(const std::string& name, const XMMATRIX& value) {}
    virtual void SetUniform(const std::string& name, const XMVECTOR& value) {}
    virtual void SetUniform(const std::string& name, float value) {}
    virtual void SetUniform(const std::string& name, int value) {}

    static std::shared_ptr<IShader> Create(const std::string& path);
};
}