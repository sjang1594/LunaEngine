#include "LunaPCH.h"
#include "EntryPoint.h"

#if defined(WL_PLATFORM_WINDOWS)

namespace Luna
{
// Implementation of Main function
int Main(int argc, char **argv)
{
    Application *app = CreateApplication(argc, argv);
    app->Run();
    delete app;
    return 0;
}
} // namespace Luna

// Implementation of entry points
#ifdef WL_DIST
int APIENTRY WinMain(HINSTANCE hInst, HINSTANCE hPrevInst, PSTR cmdLine, int cmdShow)
{
    return Luna::Main(__argc, __argv);
}
#else
int main(int argc, char **argv)
{
    return Luna::Main(argc, argv);
}
#endif

#endif // WL_PLATFORM_WINDOWS