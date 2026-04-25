#pragma once

#include <string>
#include <functional>

struct GLFWwindow;

namespace Luna
{

class CustomTitleBar
{
public:
    CustomTitleBar() = default;
    ~CustomTitleBar() = default;

    void Init(GLFWwindow* window);
    bool Render();

    void SetTitle(const std::string& title) { _title = title; }
    void SetShowLogo(bool show) { _showLogo = show; }
    void SetLogoText(const std::string& text) { _logoText = text; }
    void SetBackendLabel(const std::string& label) { _backendLabel = label; }
    void SetFrameStats(float fps, float frameTimeMs) { _fps = fps; _frameTimeMs = frameTimeMs; }
    
    void SetMenuCallback(std::function<void()> callback) { _menuCallback = callback; }
    bool IsDragging() const { return _isDragging; }
    float GetHeight() const { return _titleBarHeight; }

private:
    void HandleWindowDrag();
    void DrawMinimizeButton();
    void DrawMaximizeButton();
    void DrawCloseButton();

    GLFWwindow* _window = nullptr;
    std::string _title = "LunaEngine";
    std::string _logoText = "LUNA";
    std::string _backendLabel;
    float _fps = 0.0f;
    float _frameTimeMs = 0.0f;
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

