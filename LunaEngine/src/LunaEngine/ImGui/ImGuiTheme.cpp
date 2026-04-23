 #include "LunaPCH.h"
#include "ImGuiTheme.h"

namespace Luna
{

void ImGuiTheme::SetCommonStyle()
{
    ImGuiStyle& style = ImGui::GetStyle();
    
    // Rounding
    style.WindowRounding = 5.0f;
    style.FrameRounding = 4.0f;
    style.PopupRounding = 4.0f;
    style.ScrollbarRounding = 9.0f;
    style.GrabRounding = 3.0f;
    style.TabRounding = 4.0f;
    
    // Borders
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.PopupBorderSize = 1.0f;
    style.TabBorderSize = 0.0f;
    
    // Padding / Spacing
    style.WindowPadding = ImVec2(10.0f, 10.0f);
    style.FramePadding = ImVec2(8.0f, 4.0f);
    style.ItemSpacing = ImVec2(8.0f, 4.0f);
    style.ItemInnerSpacing = ImVec2(4.0f, 4.0f);
    style.IndentSpacing = 20.0f;
    style.ScrollbarSize = 14.0f;
    style.GrabMinSize = 10.0f;
    
    // Alignment
    style.WindowTitleAlign = ImVec2(0.5f, 0.5f);  // Center title
    style.ButtonTextAlign = ImVec2(0.5f, 0.5f);
}

void ImGuiTheme::ApplyDarkTheme()
{
    SetCommonStyle();
    ImGui::StyleColorsDark();
}

void ImGuiTheme::ApplyWalnutTheme()
{
    SetCommonStyle();
    
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;
    
    // Background colors
    const ImVec4 bgDark       = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);  // #1A1A1A
    const ImVec4 bgMid        = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);  // #262626
    const ImVec4 bgLight      = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);  // #333333
    
    // Accent colors (purple/blue)
    const ImVec4 accent       = ImVec4(0.45f, 0.35f, 0.80f, 1.00f);  // Purple
    const ImVec4 accentHover  = ImVec4(0.55f, 0.45f, 0.90f, 1.00f);
    const ImVec4 accentActive = ImVec4(0.35f, 0.25f, 0.70f, 1.00f);
    
    // Text colors
    const ImVec4 textBright   = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    const ImVec4 textNormal   = ImVec4(0.85f, 0.85f, 0.85f, 1.00f);
    const ImVec4 textDisabled = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
    
    // Border
    const ImVec4 border       = ImVec4(0.08f, 0.08f, 0.08f, 1.00f);  // Near black
    const ImVec4 borderLight  = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
    
    // Window
    colors[ImGuiCol_WindowBg]             = bgDark;
    colors[ImGuiCol_ChildBg]              = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_PopupBg]              = ImVec4(0.08f, 0.08f, 0.08f, 0.94f);
    colors[ImGuiCol_Border]               = border;
    colors[ImGuiCol_BorderShadow]         = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    
    // Frame (input fields, checkboxes, etc.)
    colors[ImGuiCol_FrameBg]              = bgMid;
    colors[ImGuiCol_FrameBgHovered]       = bgLight;
    colors[ImGuiCol_FrameBgActive]        = bgLight;
    
    // Title bar
    colors[ImGuiCol_TitleBg]              = bgDark;
    colors[ImGuiCol_TitleBgActive]        = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]     = bgDark;
    
    // Menu bar
    colors[ImGuiCol_MenuBarBg]            = bgDark;
    
    // Scrollbar
    colors[ImGuiCol_ScrollbarBg]          = bgDark;
    colors[ImGuiCol_ScrollbarGrab]        = bgLight;
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.40f, 0.40f, 0.40f, 1.00f);
    
    // Checkmark, Slider, etc.
    colors[ImGuiCol_CheckMark]            = accent;
    colors[ImGuiCol_SliderGrab]           = accent;
    colors[ImGuiCol_SliderGrabActive]     = accentActive;
    
    // Buttons
    colors[ImGuiCol_Button]               = bgLight;
    colors[ImGuiCol_ButtonHovered]        = ImVec4(0.28f, 0.28f, 0.28f, 1.00f);
    colors[ImGuiCol_ButtonActive]         = accent;
    
    // Headers (collapsing headers, tree nodes, etc.)
    colors[ImGuiCol_Header]               = bgLight;
    colors[ImGuiCol_HeaderHovered]        = ImVec4(0.28f, 0.28f, 0.28f, 1.00f);
    colors[ImGuiCol_HeaderActive]         = accent;
    
    // Separator
    colors[ImGuiCol_Separator]            = borderLight;
    colors[ImGuiCol_SeparatorHovered]     = accent;
    colors[ImGuiCol_SeparatorActive]      = accentActive;
    
    // Resize grip
    colors[ImGuiCol_ResizeGrip]           = ImVec4(0.26f, 0.26f, 0.26f, 0.25f);
    colors[ImGuiCol_ResizeGripHovered]    = accent;
    colors[ImGuiCol_ResizeGripActive]     = accentActive;
    
    // Tabs
    colors[ImGuiCol_Tab]                  = bgMid;
    colors[ImGuiCol_TabHovered]           = accent;
    colors[ImGuiCol_TabActive]            = ImVec4(0.18f, 0.18f, 0.18f, 1.00f);
    colors[ImGuiCol_TabUnfocused]         = bgDark;
    colors[ImGuiCol_TabUnfocusedActive]   = bgMid;
    
    // Docking
    colors[ImGuiCol_DockingPreview]       = ImVec4(accent.x, accent.y, accent.z, 0.70f);
    colors[ImGuiCol_DockingEmptyBg]       = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);
    
    // Plot
    colors[ImGuiCol_PlotLines]            = ImVec4(0.61f, 0.61f, 0.61f, 1.00f);
    colors[ImGuiCol_PlotLinesHovered]     = accent;
    colors[ImGuiCol_PlotHistogram]        = accent;
    colors[ImGuiCol_PlotHistogramHovered] = accentHover;
    
    // Table
    colors[ImGuiCol_TableHeaderBg]        = bgMid;
    colors[ImGuiCol_TableBorderStrong]    = border;
    colors[ImGuiCol_TableBorderLight]     = borderLight;
    colors[ImGuiCol_TableRowBg]           = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_TableRowBgAlt]        = ImVec4(1.00f, 1.00f, 1.00f, 0.02f);
    
    // Text
    colors[ImGuiCol_Text]                 = textNormal;
    colors[ImGuiCol_TextDisabled]         = textDisabled;
    colors[ImGuiCol_TextSelectedBg]       = ImVec4(accent.x, accent.y, accent.z, 0.35f);
    
    // Drag/Drop
    colors[ImGuiCol_DragDropTarget]       = accent;
    
    // Nav
    colors[ImGuiCol_NavHighlight]         = accent;
    colors[ImGuiCol_NavWindowingHighlight]= ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    colors[ImGuiCol_NavWindowingDimBg]    = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
    
    // Modal
    colors[ImGuiCol_ModalWindowDimBg]     = ImVec4(0.00f, 0.00f, 0.00f, 0.60f);
}

void ImGuiTheme::ApplyLightTheme()
{
    SetCommonStyle();
    ImGui::StyleColorsLight();
    
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;
    
    // Customize light theme
    colors[ImGuiCol_WindowBg] = ImVec4(0.94f, 0.94f, 0.94f, 1.00f);
    colors[ImGuiCol_Border]   = ImVec4(0.70f, 0.70f, 0.70f, 1.00f);
}

void ImGuiTheme::SetAccentColor(const ImVec4& color)
{
    ImVec4* colors = ImGui::GetStyle().Colors;
    
    ImVec4 hover  = ImVec4(color.x + 0.1f, color.y + 0.1f, color.z + 0.1f, color.w);
    ImVec4 active = ImVec4(color.x - 0.1f, color.y - 0.1f, color.z - 0.1f, color.w);
    
    colors[ImGuiCol_CheckMark]         = color;
    colors[ImGuiCol_SliderGrab]        = color;
    colors[ImGuiCol_SliderGrabActive]  = active;
    colors[ImGuiCol_ButtonActive]      = color;
    colors[ImGuiCol_HeaderActive]      = color;
    colors[ImGuiCol_SeparatorHovered]  = color;
    colors[ImGuiCol_SeparatorActive]   = active;
    colors[ImGuiCol_ResizeGripHovered] = color;
    colors[ImGuiCol_ResizeGripActive]  = active;
    colors[ImGuiCol_TabHovered]        = color;
    colors[ImGuiCol_DockingPreview]    = ImVec4(color.x, color.y, color.z, 0.70f);
    colors[ImGuiCol_PlotLinesHovered]  = color;
    colors[ImGuiCol_PlotHistogram]     = color;
    colors[ImGuiCol_PlotHistogramHovered] = hover;
    colors[ImGuiCol_TextSelectedBg]    = ImVec4(color.x, color.y, color.z, 0.35f);
    colors[ImGuiCol_DragDropTarget]    = color;
    colors[ImGuiCol_NavHighlight]      = color;
}

} // namespace Luna

