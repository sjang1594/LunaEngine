#pragma once
#include "Application.h"
#ifdef LUNA_PLATFORM_WINDOWS
extern Luna::Application* Luna::CreateApplication(int argc, char** argv);
bool g_ApplicationRunning = true;

namespace Luna
{
	int Main(int argc, char** argv)
	{
		while (g_ApplicationRunning)
		{
			Luna::Application* app = Luna::CreateApplication(argc, argv);
		}
	}
}

#ifdef WL_DIST

#include <Windows.h>

int APIENTRY WinMain(HINSTANCE hInst, HINSTANCE hInstPrev, PSTR cmdline, int cmdshow)
{
	return Luna::Main(__argc, __argv);
}

#else

int main(int argc, char** argv)
{
	return Luna::Main(argc, argv);
}

#endif // WL_DIST

#endif // WL_PLATFORM_WINDOWS