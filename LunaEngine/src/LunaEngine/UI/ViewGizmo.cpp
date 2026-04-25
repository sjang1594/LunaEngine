#include "LunaPCH.h"
#include "LunaEngine/UI/ViewGizmo.h"
#include "Renderer/Camera.h"
#include "Components/CameraComponent.h"
#include <imgui.h>
#include <cmath>

namespace Luna
{

// Axis definitions: world direction (Y-up), display color, display label.
// No axis swap — gizmo X/Y/Z maps directly to world X/Y/Z.
struct AxisDef {
    float dir[3];      // world-space direction
    ImU32 color;
    const char* label;
};

static const AxisDef kAxes[] = {
    // Positive axes
    {{ 1, 0, 0}, IM_COL32(220,  60,  60, 255), "X"},   // +X (red)
    {{ 0, 1, 0}, IM_COL32( 60, 200,  60, 255), "Y"},   // +Y (green, up)
    {{ 0, 0, 1}, IM_COL32( 60, 100, 220, 255), "Z"},   // +Z (blue, forward)
    // Negative axes
    {{-1, 0, 0}, IM_COL32(140,  50,  50, 180), nullptr}, // -X
    {{ 0,-1, 0}, IM_COL32( 50, 120,  50, 180), nullptr}, // -Y
    {{ 0, 0,-1}, IM_COL32( 50,  70, 140, 180), nullptr}, // -Z
};

// Snap-to-view quaternions (Y-up).
// ComputeEye() rotates (0, 0, -radius) by q to get eye offset from target.
// Identity → eye at (0, 0, -radius) → looking along +Z (front view).
static XMVECTOR GetSnapQuat(int axisIdx)
{
    switch (axisIdx)
    {
    case 0: // +X: eye at world +X → yaw 90°
        return XMQuaternionRotationAxis(XMVectorSet(0, 1, 0, 0), XMConvertToRadians(90.0f));
    case 1: // +Y (top): eye at world +Y → pitch 90°
        return XMQuaternionRotationAxis(XMVectorSet(1, 0, 0, 0), XMConvertToRadians(90.0f));
    case 2: // +Z (front): eye at world -Z → identity
        return XMQuaternionIdentity();
    case 3: // -X: eye at world -X → yaw -90°
        return XMQuaternionRotationAxis(XMVectorSet(0, 1, 0, 0), XMConvertToRadians(-90.0f));
    case 4: // -Y (bottom): eye at world -Y → pitch -90°
        return XMQuaternionRotationAxis(XMVectorSet(1, 0, 0, 0), XMConvertToRadians(-90.0f));
    case 5: // -Z (back): eye at world +Z → yaw 180°
        return XMQuaternionRotationAxis(XMVectorSet(0, 1, 0, 0), XMConvertToRadians(180.0f));
    default:
        return XMQuaternionIdentity();
    }
}

// Template implementation — works with both Camera and CameraComponent
// (both provide GetViewMatrix() and SetOrientation())
template<typename CamT>
static void DrawViewGizmoImpl(CamT& camera, float size)
{
    XMMATRIX view = camera.GetViewMatrix();
    XMFLOAT4X4 viewF;
    XMStoreFloat4x4(&viewF, view);

    ImGuiViewport* vp = ImGui::GetMainViewport();
    float margin = 16.0f;
    float cx = vp->Pos.x + vp->Size.x - size * 0.5f - margin;
    float cy = vp->Pos.y + size * 0.5f + margin + 40.0f;

    ImGui::SetNextWindowPos(ImVec2(cx - size * 0.5f - 4, cy - size * 0.5f - 4));
    ImGui::SetNextWindowSize(ImVec2(size + 8, size + 8));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.05f, 0.05f, 0.05f, 0.6f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.2f, 0.2f, 0.2f, 0.5f));

    ImGui::Begin("##ViewGizmo", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar(2);

    ImDrawList* dl = ImGui::GetWindowDrawList();

    dl->AddCircleFilled(ImVec2(cx, cy), size * 0.45f, IM_COL32(20, 20, 20, 120), 32);
    dl->AddCircle(ImVec2(cx, cy), size * 0.45f, IM_COL32(60, 60, 60, 150), 32, 1.0f);

    float halfLen = size * 0.35f;
    float tipRadius = 8.0f;

    struct Projected { float sx, sy, depth; int idx; };
    Projected proj[6];

    for (int i = 0; i < 6; ++i)
    {
        float dx = kAxes[i].dir[0];
        float dy = kAxes[i].dir[1];
        float dz = kAxes[i].dir[2];

        float vx = dx * viewF._11 + dy * viewF._12 + dz * viewF._13;
        float vy = dx * viewF._21 + dy * viewF._22 + dz * viewF._23;
        float vz = dx * viewF._31 + dy * viewF._32 + dz * viewF._33;

        proj[i].sx    = cx + vx * halfLen;
        proj[i].sy    = cy - vy * halfLen;
        proj[i].depth = vz;
        proj[i].idx   = i;
    }

    for (int i = 0; i < 5; ++i)
        for (int j = i + 1; j < 6; ++j)
            if (proj[i].depth > proj[j].depth)
                std::swap(proj[i], proj[j]);

    ImVec2 mouse = ImGui::GetIO().MousePos;
    bool clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    int clickedAxis = -1;

    for (int p = 0; p < 6; ++p)
    {
        int i = proj[p].idx;
        float sx = proj[p].sx;
        float sy = proj[p].sy;
        ImU32 col = kAxes[i].color;

        dl->AddLine(ImVec2(cx, cy), ImVec2(sx, sy), col, 2.0f);

        float r = (i < 3) ? tipRadius : tipRadius * 0.6f;
        dl->AddCircleFilled(ImVec2(sx, sy), r, col, 16);

        if (kAxes[i].label)
        {
            ImVec2 labelSz = ImGui::CalcTextSize(kAxes[i].label);
            dl->AddText(ImVec2(sx - labelSz.x * 0.5f, sy - labelSz.y * 0.5f),
                        IM_COL32(255, 255, 255, 230), kAxes[i].label);
        }

        if (clicked)
        {
            float dx2 = mouse.x - sx;
            float dy2 = mouse.y - sy;
            if (dx2 * dx2 + dy2 * dy2 < r * r * 2.5f)
                clickedAxis = i;
        }
    }

    if (clickedAxis >= 0)
        camera.SetOrientation(GetSnapQuat(clickedAxis));

    ImGui::End();
    ImGui::PopStyleColor(2);
}

void DrawViewGizmo(Camera& camera, float size) { DrawViewGizmoImpl(camera, size); }
void DrawViewGizmo(CameraComponent& camera, float size) { DrawViewGizmoImpl(camera, size); }

} // namespace Luna

