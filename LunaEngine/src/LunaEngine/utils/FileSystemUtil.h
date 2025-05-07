#pragma once
#include <filesystem>

namespace Luna
{

namespace fs = std::filesystem;
inline fs::path GetShaderFullPath(const std::wstring& filename)
{
    fs::path shaderRoot = fs::u8path(SHADER_ROOT_PATH);
    fs::path fullPath = shaderRoot / filename;

    fullPath = fs::weakly_canonical(fullPath);
    if (!fs::exists(fullPath)) {
        std::wcerr << L"[Shader Loader] Shader not found at: " << fullPath << std::endl;
    }
    return fullPath.wstring();
}

inline fs::path GetRootDirectoryPath()
{
    try
    {
        fs::path execPath = fs::current_path();
        fs::path rootPath = execPath.parent_path();
        return rootPath.wstring();
    } catch (std::exception& e)
    {
        std::wcerr << e.what() << std::endl;
        return "";
    }
}

// this is for window tiitle
inline fs::path GetImageFullPath(const std::string& filepath)
{
    fs::path rootPath = GetRootDirectoryPath();
    fs::path fullPath = rootPath / filepath;
    if (!fs::exists(fullPath))
    {
        std::cerr << "Image path does not exist: " << fullPath << std::endl;
    }

    return fullPath;
}

}