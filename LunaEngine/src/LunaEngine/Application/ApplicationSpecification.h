#pragma once
#include <string>

namespace Luna
{
enum class RenderBackendType
{
    Vulkan,
    DirectX12
};

struct ApplicationSpecification
{
    std::string Name;
    uint32_t width, height;
    RenderBackendType backend;
};
} // namespace Luna