#include "Application.h"

#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_vulkan.h"

#include <stdio.h>
#include <stdlib.h>
#include <iostream>

#define GLFW_INCLUDE_NONE;
#define GLFW_INCLUDE_VULKAN;

#include <GLFW/glfw3.h>
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

#include "ImGui/Roboto-Regular.embed"

extern bool g_ApplicationRunning;

//#define IMGUI_UNLIMITED_FRAME_RATE
#ifdef _DEBUG
#define IMGUI_VULKAN_DEBUG_REPORT
#endif

static VkAllocationCallbacks*	g_Allocator = NULL;
static VkInstance				g_Instance = VK_NULL_HANDLE;
static VkDevice					g_Device = VK_NULL_HANDLE;
static uint32_t					g_QueueFamily = (uint32_t)-1;
static VkQueue					q_Queue = VK_NULL_HANDLE;
static VkDebugReportCallbackEXT	g_Debug

Luna::Application::Application(const ApplicationSpec& applicationSpec)
{
}

Luna::Application::~Application()
{
}

Application& Luna::Application::Get()
{
	// TODO: insert return statement here
}

void Luna::Application::Close()
{
}
