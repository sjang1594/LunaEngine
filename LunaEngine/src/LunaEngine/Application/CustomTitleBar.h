#pragma once

#include <string>
#include <functional>

struct GLFWwindow;

namespace Luna
{

// Custom title bar that replaces Windows default title bar
// Similar to Unreal Engine / Visual Studio style
class CustomTitleBar
{
public:
    CustomTitleBar() = default;
    ~CustomTitleBar() = default;

    // Initialize with GLFW window (must call after window creation)
    void Init(GLFWwindow* window);

    // Call this every frame in ImGui rendering
    // Returns true if the title bar was rendered
    bool Render();

    // Setters
    void SetTitle(const std::string& title) { _title = title; }
    void SetShowLogo(bool show) { _showLogo = show; }
    void SetLogoText(const std::string& text) { _logoText = text; }
    
    // Menu callback - called to render menu items in title bar
    void SetMenuCallback(std::function<void()> callback) { _menuCallback = callback; }

    // Check if we're currently dragging the title bar
    bool IsDragging() const { return _isDragging; }

    // Get title bar height
    float GetHeight() const { return _titleBarHeight; }

private:
    void HandleWindowDrag();
    void DrawMinimizeButton();
    void DrawMaximizeButton();
    void DrawCloseButton();

    GLFWwindow* _window = nullptr;
    std::string _title = "LunaEngine";
    std::string _logoText = "LUNA";
    bool _showLogo = true;
    
    std::function<void()> _menuCallback;

    float _titleBarHeight = 30.0f;
    bool _isDragging = false;
    double _dragStartX = 0;
    double _dragStartY = 0;
    int _windowStartX = 0;
    int _windowStartY = 0;
    
    bool _isMaximized = false;
    int _restoreX = 0, _restoreY = 0;
    int _restoreW = 0, _restoreH = 0;
};

} // namespace Luna

