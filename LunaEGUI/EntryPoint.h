#pragma once

#ifdef LUNA_PLATFORM_WINDOWS
extern Luna::Application* Luna::CreateApplication(int argc, char** argv);
bool g_ApplicationRunning = true;

namespace Luna
{
	int Main(int argc, char** argv)
	{
		while (g_ApplicationRunning)
		{
			Luna::Application
		}
	}
}
