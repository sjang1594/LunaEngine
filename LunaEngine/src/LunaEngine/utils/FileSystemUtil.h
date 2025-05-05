#pragma once
#include <filesystem>
#include <windows.h>
namespace Luna
{
inline std::filesystem::path GetShaderFullPath(const std::wstring& filename)
{
    std::filesystem::path shaderRoot = std::filesystem::u8path(SHADER_ROOT_PATH);
    std::filesystem::path fullPath = shaderRoot / filename;

    fullPath = std::filesystem::weakly_canonical(fullPath);
    if (!std::filesystem::exists(fullPath)) {
        std::wcerr << L"[Shader Loader] Shader not found at: " << fullPath << std::endl;
    }
    return fullPath.wstring();
}
}