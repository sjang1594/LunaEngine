#include "LunaPCH.h"
#include "WindowGLFW.h"
#include "utils/FileSystemUtil.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace Luna
{
void SetWindowsIcon(GLFWwindow* window, const std::string& filePath)
{
    int width, height, channels;

    std::string imageFilePath = GetImageFullPath(filePath).generic_string();
    unsigned char* image = stbi_load(imageFilePath.c_str(), &width, &height, &channels, STBI_rgb_alpha);

    if (!image)
    {
        std::cerr << "Failed to load Image - " << imageFilePath << std::endl;
        return;
    }

    GLFWimage icon;
    icon.width = width;
    icon.height = height;
    icon.pixels = image;

    glfwSetWindowIcon(window, 1, &icon);
    stbi_image_free(image);
    std::cout << "Icon Set Successfully: " << filePath << std::endl;
}
}