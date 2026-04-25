#include "LunaPCH.h"
#include "LunaEngine/Utils/FileDialog.h"

#include <atomic>
#include <mutex>
#include <thread>
#include <string>

#ifdef _WIN32
#include <Windows.h>
#include <commdlg.h>
#pragma comment(lib, "comdlg32.lib")
#endif

namespace Luna
{

// ── Synchronous (legacy) ────────────────────────────────────────────────────
std::string OpenFileDialog(const char* title, const char* filter)
{
#ifdef _WIN32
    char filePath[MAX_PATH] = {};

    OPENFILENAMEA ofn = {};
    ofn.lStructSize  = sizeof(ofn);
    ofn.hwndOwner    = nullptr;
    ofn.lpstrFilter  = filter;
    ofn.lpstrFile    = filePath;
    ofn.nMaxFile     = MAX_PATH;
    ofn.lpstrTitle   = title;
    ofn.Flags        = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameA(&ofn))
        return std::string(filePath);
#endif
    return {};
}

// ── Async (non-blocking) ────────────────────────────────────────────────────
static std::atomic<bool> s_dialogOpen{false};
static std::atomic<bool> s_resultReady{false};
static std::mutex        s_resultMutex;
static std::string       s_resultPath;

void OpenFileDialogAsync(const char* title, const char* filter)
{
    if (s_dialogOpen.load()) return;  // already open

    // Copy filter/title — the caller's pointers may be stack-local
    // (filter contains embedded \0, so we need to handle it carefully)
    std::string titleCopy(title);

    // Filter is double-null-terminated.  Measure its full length.
    const char* p = filter;
    while (*p || *(p + 1)) ++p;
    std::string filterCopy(filter, p + 2);  // include final double-null

    s_dialogOpen.store(true);
    s_resultReady.store(false);

    std::thread([titleCopy = std::move(titleCopy),
                 filterCopy = std::move(filterCopy)]()
    {
        std::string result = OpenFileDialog(titleCopy.c_str(), filterCopy.c_str());

        {
            std::lock_guard<std::mutex> lock(s_resultMutex);
            s_resultPath = std::move(result);
        }
        s_resultReady.store(true);
        s_dialogOpen.store(false);
    }).detach();
}

bool IsFileDialogOpen()
{
    return s_dialogOpen.load();
}

bool PollFileDialogResult(std::string& outPath)
{
    if (!s_resultReady.load()) return false;

    {
        std::lock_guard<std::mutex> lock(s_resultMutex);
        outPath = std::move(s_resultPath);
        s_resultPath.clear();
    }
    s_resultReady.store(false);
    return true;
}

} // namespace Luna

