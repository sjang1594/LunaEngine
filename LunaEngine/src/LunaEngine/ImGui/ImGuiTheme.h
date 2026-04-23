#pragma once

#include <imgui.h>

namespace Luna
{

class ImGuiTheme
{
public:
    // Predefined themes
    static void ApplyDarkTheme();
    static void ApplyWalnutTheme();      // Dark modern style with black borders
    static void ApplyLightTheme();
    
    // Custom accent color (call after ApplyXXXTheme)
    static void SetAccentColor(const ImVec4& color);
    
private:
    static void SetCommonStyle();
};

} // namespace Luna

