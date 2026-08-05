#pragma once
#include <filesystem>
#ifdef _WIN32
#include <windows.h>
#endif

namespace Luna
{

namespace fs = std::filesystem;

// Returns the directory containing the running executable (e.g. .../LunaApp/bin/Debug-windows-x86_64/LunaApp/)
inline fs::path GetExeDir()
{
#ifdef _WIN32
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path();
#else
    return fs::current_path();
#endif
}

// Resolves an asset path relative to the executable directory first; falls back to CWD.
// Usage: GetAssetPath("Assets/DamagedHelmet.glb")
inline fs::path GetAssetPath(const std::string& relativePath)
{
    fs::path exeRelative = GetExeDir() / relativePath;
    if (fs::exists(exeRelative))
        return exeRelative;
    fs::path cwdRelative = fs::current_path() / relativePath;
    if (fs::exists(cwdRelative))
        return cwdRelative;
    return exeRelative; // return exe-relative even if missing (so error messages make sense)
}

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