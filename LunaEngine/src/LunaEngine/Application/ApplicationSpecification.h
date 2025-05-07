#pragma once
#include <string>
#include <filesystem>

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
    std::filesystem::path iconPath;
};
} // namespace Luna