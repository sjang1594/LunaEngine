#pragma once
#include <string>

struct GLFWwindow;
namespace Luna
{
void SetWindowsIcon(GLFWwindow* window, const std::string& filePath);
}