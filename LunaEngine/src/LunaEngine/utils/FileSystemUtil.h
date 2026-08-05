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

// Walks up from the exe directory looking for the repo root (identified by
// LunaEngine.sln or LunaEngine/LunaEngine.vcxproj). Returns empty on failure.
inline fs::path FindProjectRoot()
{
    fs::path dir = GetExeDir();
    for (int i = 0; i < 16; ++i)
    {
        if (fs::exists(dir / "LunaEngine.sln") ||
            fs::exists(dir / "LunaEngine" / "LunaEngine.vcxproj"))
            return dir;
        if (!dir.has_parent_path() || dir.parent_path() == dir)
            break;
        dir = dir.parent_path();
    }
    return {};
}

inline const fs::path& GetShaderRoot()
{
    static const fs::path cached = []() -> fs::path {
        fs::path root = FindProjectRoot();
        if (!root.empty())
            return root / "LunaEngine" / "src" / "LunaEngine" / "Shaders";
        std::cerr << "[Shader Loader] Could not locate project root from exe dir" << std::endl;
        return {};
    }();
    return cached;
}

inline fs::path GetShaderFullPath(const std::wstring& filename)
{
    fs::path fullPath = GetShaderRoot() / filename;

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