#pragma once
#if defined(WL_PLATFORM_WINDOWS)
#include <LunaEngine/Application.h>
bool g_ApplicationRunning = true;

namespace Luna {
    // Forward declaration
    class Application;
    extern Application* CreateApplication(int argc, char** argv);
    int Main(int argc, char** argv);
}

#ifdef WL_DIST
#include <Windows.h>
int APIENTRY WinMain(HINSTANCE hInst, HINSTANCE hPrevInst, PSTR cmdLine, int cmdShow);
#else
int main(int argc, char** argv);
#endif
#endif // WL_PLATFORM_WINDOWS