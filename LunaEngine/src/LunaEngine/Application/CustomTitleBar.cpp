#include "LunaPCH.h"
#include "CustomTitleBar.h"
#include <GLFW/glfw3.h>

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <dwmapi.h>
#include <windowsx.h>   // GET_X_LPARAM, GET_Y_LPARAM
#pragma comment(lib, "dwmapi.lib")

// Subclass proc to handle custom hit-testing so resize works correctly
static HWND       s_hwnd       = nullptr;
static float      s_titleBarH  = 30.0f;
static WNDPROC    s_origProc   = nullptr;

static LRESULT CALLBACK CustomWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_NCHITTEST)
    {
        // Let DWM handle the border resize areas first
        LRESULT hit = DefWindowProcW(hwnd, msg, wParam, lParam);
        if (hit == HTCLIENT)
        {
            // Convert screen coords to client coords
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hwnd, &pt);

            RECT rc;
            GetClientRect(hwnd, &rc);

            // Resize border thickness
            const int border = 6;

            // Corners (resize priority over title bar)
            if (pt.x < border && pt.y < border)            return HTTOPLEFT;
            if (pt.x > rc.right - border && pt.y < border)  return HTTOPRIGHT;
            if (pt.x < border && pt.y > rc.bottom - border) return HTBOTTOMLEFT;
            if (pt.x > rc.right - border && pt.y > rc.bottom - border) return HTBOTTOMRIGHT;

            // Edges
            if (pt.y < border)                              return HTTOP;
            if (pt.y > rc.bottom - border)                  return HTBOTTOM;
            if (pt.x < border)                              return HTLEFT;
            if (pt.x > rc.right - border)                   return HTRIGHT;

            // Title bar drag area (top N pixels, but not the button zone on the right)
            int titleBarPx = (int)s_titleBarH;
            int buttonsZone = 46 * 3;  // 3 buttons × 46px each
            if (pt.y < titleBarPx && pt.x < rc.right - buttonsZone)
                return HTCAPTION;
        }
        return hit;
    }

    // Windows 11: Remove the 1px top white line by absorbing WM_NCCALCSIZE
    if (msg == WM_NCCALCSIZE && wParam == TRUE)
    {
        auto& params = *reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
        auto& requested = params.rgrc[0];
        // Shrink top by 1 to hide DWM's 1px border
        requested.top += 1;
        return 0;
    }

    // Prevent the non-client area from drawing (we handle it ourselves)
    if (msg == WM_NCPAINT) return 0;

    return CallWindowProcW(s_origProc, hwnd, msg, wParam, lParam);
}
#endif

namespace Luna
{

void CustomTitleBar::Init(GLFWwindow* window)
{
    _window = window;

#ifdef _WIN32
    HWND hwnd = glfwGetWin32Window(window);
    s_hwnd      = hwnd;
    s_titleBarH = _titleBarHeight;

    // Keep WS_THICKFRAME (resize) but remove WS_CAPTION (title bar chrome)
    // WS_OVERLAPPEDWINDOW without WS_CAPTION gives us a window with
    // a resize border but no system title bar drawn by DWM
    LONG style = GetWindowLong(hwnd, GWL_STYLE);
    style &= ~WS_CAPTION;
    style |= WS_THICKFRAME;
    SetWindowLong(hwnd, GWL_STYLE, style);

    // Extend DWM shadow/frame — { left, right, top, bottom }
    // All 0 = no DWM frame visible (shadow still drawn by DWM)
    MARGINS margins = { 0, 0, 0, 0 };
    DwmExtendFrameIntoClientArea(hwnd, &margins);

    // Subclass the window to intercept WM_NCHITTEST
    s_origProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(CustomWndProc)));

    // Force re-layout
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
        SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
#endif

    glfwGetWindowPos(_window, &_restoreX, &_restoreY);
    glfwGetWindowSize(_window, &_restoreW, &_restoreH);
}

bool CustomTitleBar::Render()
{
    if (!_window) return false;

    ImGuiViewport* viewport = ImGui::GetMainViewport();

    // ── Colors ───────────────────────────────────────────────────────────────
    const ImVec4 bgColor    = ImVec4(0.08f, 0.08f, 0.08f, 1.0f);
    const ImVec4 logoColor  = ImVec4(0.45f, 0.35f, 0.80f, 1.0f);
    const ImVec4 textColor  = ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
    const ImVec4 btnHover   = ImVec4(0.25f, 0.25f, 0.25f, 1.0f);
    const ImVec4 closeHover = ImVec4(0.80f, 0.20f, 0.20f, 1.0f);

    // ── FramePadding: make menubar height == _titleBarHeight ─────────────────
    float fontSize  = ImGui::GetFontSize();
    float framePadY = floorf((_titleBarHeight - fontSize) * 0.5f);
    if (framePadY < 0.0f) framePadY = 0.0f;

    // ── Window setup ─────────────────────────────────────────────────────────
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, _titleBarHeight));
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,   0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,     ImVec2(10.0f, framePadY));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,      ImVec2(0.0f, 0.0f));

    ImGui::PushStyleColor(ImGuiCol_WindowBg,          bgColor);
    ImGui::PushStyleColor(ImGuiCol_MenuBarBg,         bgColor);
    ImGui::PushStyleColor(ImGuiCol_PopupBg,           ImVec4(0.10f, 0.10f, 0.10f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Header,            ImVec4(0.20f, 0.20f, 0.20f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,     btnHover);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,      btnHover);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar     | ImGuiWindowFlags_NoResize   |
        ImGuiWindowFlags_NoMove         | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings| ImGuiWindowFlags_NoDocking  |
        ImGuiWindowFlags_MenuBar;

    ImGui::Begin("##TitleBar", nullptr, flags);
    ImGui::PopStyleVar(5);

    ImDrawList* dl    = ImGui::GetWindowDrawList();
    ImVec2      wPos  = ImGui::GetWindowPos();
    ImVec2      wSize = ImGui::GetWindowSize();

    // Bottom border
    dl->AddLine(ImVec2(wPos.x,         wPos.y + wSize.y - 1.0f),
                ImVec2(wPos.x + wSize.x, wPos.y + wSize.y - 1.0f),
                IM_COL32(30, 30, 30, 255), 1.0f);

    // ── Everything inside MenuBar so heights always match ─────────────────────
    if (ImGui::BeginMenuBar())
    {
        // 1. Logo
        if (_showLogo && !_logoText.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, logoColor);
            ImGui::TextUnformatted(_logoText.c_str());
            ImGui::PopStyleColor();
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8.0f);
        }

        // 2. Menu items (File, Edit, …) — correct width inside MenuBar context
        if (_menuCallback)
            _menuCallback();

        // 3. Centered title drawn via DrawList (no layout impact)
        {
            ImVec2 sz  = ImGui::CalcTextSize(_title.c_str());
            float  tx  = wPos.x + (wSize.x - sz.x) * 0.5f;
            float  ty  = wPos.y + (_titleBarHeight - sz.y) * 0.5f;
            dl->AddText(ImVec2(tx, ty),
                        ImGui::ColorConvertFloat4ToU32(textColor),
                        _title.c_str());
        }

        // 4. Right-aligned control buttons — jump cursor to right
        const float btnW          = 46.0f;
        const float totalBtnWidth = btnW * 3.0f;
        // SetCursorPosX is relative to window left; fill remaining space
        float rightEdge = wSize.x - totalBtnWidth;
        if (ImGui::GetCursorPosX() < rightEdge)
            ImGui::SetCursorPosX(rightEdge);

        // Helper: transparent button, height = 0 means "use line height" (= menuBar height)
        auto CtrlBtn = [&](const char* id, ImVec4 hov) -> bool
        {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0,0,0,0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hov);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  hov);
            bool hit = ImGui::Button(id, ImVec2(btnW, _titleBarHeight));
            ImGui::PopStyleColor(3);
            return hit;
        };

        // ── Minimize ─────────────────────────────────────────────────────────
        if (CtrlBtn("##Min", btnHover)) glfwIconifyWindow(_window);
        {
            ImVec2 rMin = ImGui::GetItemRectMin();
            ImVec2 rSz  = ImGui::GetItemRectSize();
            float cx = rMin.x + rSz.x * 0.5f, cy = rMin.y + rSz.y * 0.5f;
            dl->AddLine(ImVec2(cx-5, cy), ImVec2(cx+5, cy), IM_COL32(200,200,200,255), 1.5f);
        }
        ImGui::SameLine(0, 0);

        // ── Maximize / Restore ───────────────────────────────────────────────
        if (CtrlBtn("##Max", btnHover))
        {
            if (_isMaximized) {
                glfwSetWindowPos(_window, _restoreX, _restoreY);
                glfwSetWindowSize(_window, _restoreW, _restoreH);
                _isMaximized = false;
            } else {
                glfwGetWindowPos(_window, &_restoreX, &_restoreY);
                glfwGetWindowSize(_window, &_restoreW, &_restoreH);
                GLFWmonitor* mon = glfwGetPrimaryMonitor();
                const GLFWvidmode* m = glfwGetVideoMode(mon);
                int mx, my; glfwGetMonitorPos(mon, &mx, &my);
                glfwSetWindowPos(_window, mx, my);
                glfwSetWindowSize(_window, m->width, m->height - 40);
                _isMaximized = true;
            }
        }
        {
            ImVec2 rMin = ImGui::GetItemRectMin();
            ImVec2 rSz  = ImGui::GetItemRectSize();
            float cx = rMin.x + rSz.x * 0.5f, cy = rMin.y + rSz.y * 0.5f;
            if (_isMaximized) {
                float h = 4.5f, o = 2.0f;
                dl->AddRect(ImVec2(cx-h+o,cy-h-o),ImVec2(cx+h+o,cy+h-o),IM_COL32(200,200,200,255),0,0,1.0f);
                dl->AddRectFilled(ImVec2(cx-h-o,cy-h+o),ImVec2(cx+h-o,cy+h+o),ImGui::ColorConvertFloat4ToU32(bgColor));
                dl->AddRect(ImVec2(cx-h-o,cy-h+o),ImVec2(cx+h-o,cy+h+o),IM_COL32(200,200,200,255),0,0,1.0f);
            } else {
                dl->AddRect(ImVec2(cx-5,cy-5),ImVec2(cx+5,cy+5),IM_COL32(200,200,200,255),0,0,1.0f);
            }
        }
        ImGui::SameLine(0, 0);

        // ── Close ────────────────────────────────────────────────────────────
        if (CtrlBtn("##Close", closeHover))
            glfwSetWindowShouldClose(_window, GLFW_TRUE);
        {
            ImVec2 rMin = ImGui::GetItemRectMin();
            ImVec2 rSz  = ImGui::GetItemRectSize();
            float cx = rMin.x + rSz.x * 0.5f, cy = rMin.y + rSz.y * 0.5f;
            dl->AddLine(ImVec2(cx-5,cy-5),ImVec2(cx+5,cy+5),IM_COL32(200,200,200,255),1.5f);
            dl->AddLine(ImVec2(cx+5,cy-5),ImVec2(cx-5,cy+5),IM_COL32(200,200,200,255),1.5f);
        }

        // ── Double-click drag area to maximize ───────────────────────────────
        {
            ImVec2 mp = ImGui::GetIO().MousePos;
            bool inDrag = mp.x >= wPos.x && mp.x < wPos.x + (wSize.x - totalBtnWidth) &&
                          mp.y >= wPos.y && mp.y < wPos.y + _titleBarHeight;
            if (inDrag && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                if (_isMaximized) {
                    glfwSetWindowPos(_window, _restoreX, _restoreY);
                    glfwSetWindowSize(_window, _restoreW, _restoreH);
                    _isMaximized = false;
                } else {
                    glfwGetWindowPos(_window, &_restoreX, &_restoreY);
                    glfwGetWindowSize(_window, &_restoreW, &_restoreH);
                    GLFWmonitor* mon = glfwGetPrimaryMonitor();
                    const GLFWvidmode* m = glfwGetVideoMode(mon);
                    int mx, my; glfwGetMonitorPos(mon, &mx, &my);
                    glfwSetWindowPos(_window, mx, my);
                    glfwSetWindowSize(_window, m->width, m->height - 40);
                    _isMaximized = true;
                }
            }
        }

        ImGui::EndMenuBar();
    }

    ImGui::End();
    ImGui::PopStyleColor(6);

    return true;
}

void CustomTitleBar::HandleWindowDrag()
{
    // Native drag via WM_NCHITTEST HTCAPTION — no ImGui dragging needed
}

} // namespace Luna

