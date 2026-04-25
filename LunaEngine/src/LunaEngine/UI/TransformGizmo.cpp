#include "LunaPCH.h"
#include "LunaEngine/UI/TransformGizmo.h"
#include "LunaEngine/UI/GizmoMath.h"
#include "LunaEngine/Utils/MathUtils.h"
#include "Renderer/Camera.h"
#include "Components/CameraComponent.h"
#include "Components/Transform.h"
#include <imgui.h>
#include <algorithm>

namespace Luna
{

// ─── Manipulator IDs ─────────────────────────────────────────────────────

enum class ManipID : int
{
    None = 0,
    TranslateX, TranslateY, TranslateZ,
    TranslatePlaneXY, TranslatePlaneXZ, TranslatePlaneYZ,
    RotateX, RotateY, RotateZ, RotateScreen,
    ScaleX, ScaleY, ScaleZ,
};

// ─── Colors ──────────────────────────────────────────────────────────────

static const ImU32 kColorX       = IM_COL32(220,  60,  60, 255);
static const ImU32 kColorY       = IM_COL32( 60, 200,  60, 255);
static const ImU32 kColorZ       = IM_COL32( 60, 100, 220, 255);
static const ImU32 kColorScreen  = IM_COL32(200, 200, 200, 180);
static const ImU32 kColorPlane   = IM_COL32(255, 255,  80, 100);

static ImU32 Brighten(ImU32 col, float factor = 1.5f)
{
    int r = (int)((col >>  0) & 0xFF);
    int g = (int)((col >>  8) & 0xFF);
    int b = (int)((col >> 16) & 0xFF);
    int a = (int)((col >> 24) & 0xFF);
    r = std::min(255, (int)(r * factor));
    g = std::min(255, (int)(g * factor));
    b = std::min(255, (int)(b * factor));
    return IM_COL32(r, g, b, a);
}

static ImU32 GetManipColor(ManipID id, ManipID hovered)
{
    ImU32 base;
    switch (id)
    {
    case ManipID::TranslateX: case ManipID::RotateX: case ManipID::ScaleX:
        base = kColorX; break;
    case ManipID::TranslateY: case ManipID::RotateY: case ManipID::ScaleY:
        base = kColorY; break;
    case ManipID::TranslateZ: case ManipID::RotateZ: case ManipID::ScaleZ:
        base = kColorZ; break;
    case ManipID::RotateScreen:
        base = kColorScreen; break;
    case ManipID::TranslatePlaneXY: case ManipID::TranslatePlaneXZ: case ManipID::TranslatePlaneYZ:
        base = kColorPlane; break;
    default: base = IM_COL32(255, 255, 255, 255); break;
    }
    return (id == hovered) ? Brighten(base) : base;
}

// ─── Gizmo State ─────────────────────────────────────────────────────────

struct GizmoState
{
    bool      isDragging       = false;
    ManipID   activeManip      = ManipID::None;
    ManipID   hoveredManip     = ManipID::None;

    // Drag start state
    XMFLOAT4  dragPlaneNormal  = {0,0,0,0};
    XMFLOAT4  dragPlanePoint   = {0,0,0,0};
    XMFLOAT4  dragStartHit     = {0,0,0,0};
    XMFLOAT3  origPosition     = {};
    XMFLOAT3  origScale        = {};
    ImVec2    dragStartMouse   = {};

    // Quaternion-based rotation state
    XMFLOAT4  origQuat         = {0,0,0,1}; // original rotation as quaternion
    XMFLOAT4  rotAxis          = {0,1,0,0}; // axis of rotation for current drag
    // Stored axes at drag start (for translation/scale constraint)
    XMFLOAT4  dragAxes[3]      = {{1,0,0,0},{0,1,0,0},{0,0,1,0}};
};

static GizmoState s_state;
static GizmoMode  s_mode = GizmoMode::Translate;

bool IsGizmoDragging() { return s_state.isDragging; }
bool IsGizmoHovered()  { return s_state.hoveredManip != ManipID::None; }
void SetGizmoMode(GizmoMode mode) { s_mode = mode; }
GizmoMode GetGizmoMode() { return s_mode; }

// ─── Axis definitions ────────────────────────────────────────────────────
// No axis swap — gizmo X/Y/Z maps directly to world X/Y/Z.
// X = Red, Y = Green (up in Y-up engine), Z = Blue

static XMVECTOR GetWorldAxis(int axisIndex, bool localSpace, XMMATRIX rotMat)
{
    // axisIndex: 0=X(red), 1=Y(green), 2=Z(blue) — direct mapping to world axes
    XMVECTOR worldDirs[3] = {
        XMVectorSet(1, 0, 0, 0),  // X
        XMVectorSet(0, 1, 0, 0),  // Y (up)
        XMVectorSet(0, 0, 1, 0),  // Z (forward)
    };
    if (localSpace)
        return XMVector3Normalize(XMVector3TransformNormal(worldDirs[axisIndex], rotMat));
    return worldDirs[axisIndex];
}

// ─── Decompose Transform helper ─────────────────────────────────────────

static void DecomposeTransform(Transform& t, XMFLOAT3& pos, XMFLOAT3& scl,
                               XMMATRIX& outRotMat, XMVECTOR& outQuat)
{
    if (t.useRawMatrix)
    {
        XMMATRIX world = XMLoadFloat4x4(&t.rawMatrix);
        XMVECTOR sv, rv, tv;
        XMMatrixDecompose(&sv, &rv, &tv, world);
        XMStoreFloat3(&pos, tv);
        XMStoreFloat3(&scl, sv);
        outQuat = rv;
        outRotMat = XMMatrixRotationQuaternion(rv);
    }
    else
    {
        pos = t.position;
        scl = t.scale;
        outRotMat = XMMatrixRotationRollPitchYaw(
            XMConvertToRadians(t.rotation.x),
            XMConvertToRadians(t.rotation.y),
            XMConvertToRadians(t.rotation.z));
        outQuat = XMQuaternionRotationMatrix(outRotMat);
    }
}

static void ApplyTransformQuat(Transform& t, const XMFLOAT3& pos, XMVECTOR quat, const XMFLOAT3& scl)
{
    // Store as Euler for compatibility with renderer's GetWorldMatrix()
    XMFLOAT4 qf; XMStoreFloat4(&qf, XMQuaternionNormalize(quat));
    t.useRawMatrix = false;
    t.position = pos;
    t.rotation = QuatToEulerDegrees(qf);
    t.scale    = scl;
}


// ─── Hit-testing ─────────────────────────────────────────────────────────

static const float kHitThreshold    = 7.0f;   // pixels
static const float kPlaneQuadFrac   = 0.3f;   // fraction of axis length for plane quad
static const float kRotateArcRadius = 0.9f;   // fraction of pixelSize/2 for rotation rings
static const int   kArcSegments     = 64;

// Point-in-convex-quad test (screen space)
static bool PointInQuad(ImVec2 p, ImVec2 a, ImVec2 b, ImVec2 c, ImVec2 d)
{
    auto cross2D = [](ImVec2 o, ImVec2 a2, ImVec2 b2) {
        return (a2.x - o.x) * (b2.y - o.y) - (a2.y - o.y) * (b2.x - o.x);
    };
    bool s1 = cross2D(a, b, p) >= 0;
    bool s2 = cross2D(b, c, p) >= 0;
    bool s3 = cross2D(c, d, p) >= 0;
    bool s4 = cross2D(d, a, p) >= 0;
    return (s1 == s2) && (s2 == s3) && (s3 == s4);
}

struct AxisScreenData
{
    ImVec2 center;
    ImVec2 tipX, tipY, tipZ;
    // Plane quad corners [3 planes][4 corners]
    ImVec2 planeQuad[3][4]; // XY=0, XZ=1, YZ=2
    float  axisLength; // screen pixels of longest axis
};

static AxisScreenData ComputeScreenData(
    XMVECTOR worldCenter, XMVECTOR axes[3], float worldLen,
    XMMATRIX viewProj, ImVec2 vpPos, ImVec2 vpSize)
{
    AxisScreenData d;
    d.center = WorldToScreen(worldCenter, viewProj, vpPos, vpSize);

    // Project world-space tips — perspective foreshortening preserves 3D depth cues
    XMVECTOR tips[3];
    for (int i = 0; i < 3; i++)
        tips[i] = XMVectorAdd(worldCenter, XMVectorScale(axes[i], worldLen));

    d.tipX = WorldToScreen(tips[0], viewProj, vpPos, vpSize);
    d.tipY = WorldToScreen(tips[1], viewProj, vpPos, vpSize);
    d.tipZ = WorldToScreen(tips[2], viewProj, vpPos, vpSize);
    d.axisLength = std::max({DistSS(d.center, d.tipX),
                             DistSS(d.center, d.tipY),
                             DistSS(d.center, d.tipZ)});

    // Plane quads in world space, projected with perspective
    float pf = kPlaneQuadFrac * worldLen;
    // XY plane quad
    {
        XMVECTOR p0 = worldCenter;
        XMVECTOR p1 = XMVectorAdd(worldCenter, XMVectorScale(axes[0], pf));
        XMVECTOR p2 = XMVectorAdd(p1, XMVectorScale(axes[1], pf));
        XMVECTOR p3 = XMVectorAdd(worldCenter, XMVectorScale(axes[1], pf));
        d.planeQuad[0][0] = WorldToScreen(p0, viewProj, vpPos, vpSize);
        d.planeQuad[0][1] = WorldToScreen(p1, viewProj, vpPos, vpSize);
        d.planeQuad[0][2] = WorldToScreen(p2, viewProj, vpPos, vpSize);
        d.planeQuad[0][3] = WorldToScreen(p3, viewProj, vpPos, vpSize);
    }
    // XZ plane quad
    {
        XMVECTOR p0 = worldCenter;
        XMVECTOR p1 = XMVectorAdd(worldCenter, XMVectorScale(axes[0], pf));
        XMVECTOR p2 = XMVectorAdd(p1, XMVectorScale(axes[2], pf));
        XMVECTOR p3 = XMVectorAdd(worldCenter, XMVectorScale(axes[2], pf));
        d.planeQuad[1][0] = WorldToScreen(p0, viewProj, vpPos, vpSize);
        d.planeQuad[1][1] = WorldToScreen(p1, viewProj, vpPos, vpSize);
        d.planeQuad[1][2] = WorldToScreen(p2, viewProj, vpPos, vpSize);
        d.planeQuad[1][3] = WorldToScreen(p3, viewProj, vpPos, vpSize);
    }
    // YZ plane quad
    {
        XMVECTOR p0 = worldCenter;
        XMVECTOR p1 = XMVectorAdd(worldCenter, XMVectorScale(axes[1], pf));
        XMVECTOR p2 = XMVectorAdd(p1, XMVectorScale(axes[2], pf));
        XMVECTOR p3 = XMVectorAdd(worldCenter, XMVectorScale(axes[2], pf));
        d.planeQuad[2][0] = WorldToScreen(p0, viewProj, vpPos, vpSize);
        d.planeQuad[2][1] = WorldToScreen(p1, viewProj, vpPos, vpSize);
        d.planeQuad[2][2] = WorldToScreen(p2, viewProj, vpPos, vpSize);
        d.planeQuad[2][3] = WorldToScreen(p3, viewProj, vpPos, vpSize);
    }

    return d;
}

// Compute min screen-space distance from mouse to a projected 3D ring.
// Samples N points on the ring circle and returns the minimum distance to any of them.
static float DistToProjectedRing(ImVec2 mouse, XMVECTOR worldCenter, XMVECTOR axis,
                                  float worldRadius, XMMATRIX viewProj, ImVec2 vpPos, ImVec2 vpSize)
{
    XMVECTOR up = (fabsf(XMVectorGetY(axis)) < 0.99f)
        ? XMVectorSet(0, 1, 0, 0) : XMVectorSet(1, 0, 0, 0);
    XMVECTOR t1 = XMVector3Normalize(XMVector3Cross(axis, up));
    XMVECTOR t2 = XMVector3Normalize(XMVector3Cross(axis, t1));

    const int N = 32;
    float minDist = 1e9f;
    for (int i = 0; i < N; i++)
    {
        float a = (float)i / (float)N * XM_2PI;
        XMVECTOR pt = XMVectorAdd(worldCenter,
            XMVectorAdd(XMVectorScale(t1, cosf(a) * worldRadius),
                        XMVectorScale(t2, sinf(a) * worldRadius)));
        ImVec2 sp = WorldToScreen(pt, viewProj, vpPos, vpSize);
        float d = DistSS(mouse, sp);
        if (d < minDist) minDist = d;
    }
    return minDist;
}

static ManipID HitTest(ImVec2 mouse, const AxisScreenData& sd,
                       XMVECTOR worldCenter, XMVECTOR axes[3], float worldLen,
                       XMMATRIX viewProj, ImVec2 vpPos, ImVec2 vpSize)
{
    if (s_mode == GizmoMode::Translate)
    {
        // Plane quads first
        static const ManipID planeIDs[3] = {
            ManipID::TranslatePlaneXY, ManipID::TranslatePlaneXZ, ManipID::TranslatePlaneYZ };
        for (int i = 0; i < 3; i++)
            if (PointInQuad(mouse, sd.planeQuad[i][0], sd.planeQuad[i][1],
                            sd.planeQuad[i][2], sd.planeQuad[i][3]))
                return planeIDs[i];

        // Axis arrows
        float axisDists[3] = {
            DistPointToSegmentSS(mouse, sd.center, sd.tipX),
            DistPointToSegmentSS(mouse, sd.center, sd.tipY),
            DistPointToSegmentSS(mouse, sd.center, sd.tipZ)
        };
        int minIdx = 0;
        for (int i = 1; i < 3; i++)
            if (axisDists[i] < axisDists[minIdx]) minIdx = i;
        if (axisDists[minIdx] < kHitThreshold)
        {
            static const ManipID ids[3] = { ManipID::TranslateX, ManipID::TranslateY, ManipID::TranslateZ };
            return ids[minIdx];
        }
    }
    else if (s_mode == GizmoMode::Rotate)
    {
        // Check distance to each projected 3D ring
        float ringWorldRadius = worldLen * kRotateArcRadius;
        float ringDists[3];
        for (int i = 0; i < 3; i++)
            ringDists[i] = DistToProjectedRing(mouse, worldCenter, axes[i],
                                                ringWorldRadius, viewProj, vpPos, vpSize);
        int minRI = 0;
        for (int i = 1; i < 3; i++)
            if (ringDists[i] < ringDists[minRI]) minRI = i;
        if (ringDists[minRI] < kHitThreshold)
        {
            static const ManipID rotIDs[3] = { ManipID::RotateX, ManipID::RotateY, ManipID::RotateZ };
            return rotIDs[minRI];
        }

        // Screen rotate (outer ring)
        float distFromCenter = DistSS(mouse, sd.center);
        float outerRingRadius = sd.axisLength * 1.1f;
        if (fabsf(distFromCenter - outerRingRadius) < kHitThreshold)
            return ManipID::RotateScreen;
    }
    else if (s_mode == GizmoMode::Scale)
    {
        // Scale cubes at tips
        float tipDists[3] = {
            DistSS(mouse, sd.tipX),
            DistSS(mouse, sd.tipY),
            DistSS(mouse, sd.tipZ)
        };
        int minIdx = 0;
        for (int i = 1; i < 3; i++)
            if (tipDists[i] < tipDists[minIdx]) minIdx = i;
        if (tipDists[minIdx] < kHitThreshold * 1.5f)
        {
            static const ManipID ids[3] = { ManipID::ScaleX, ManipID::ScaleY, ManipID::ScaleZ };
            return ids[minIdx];
        }

        // Also allow clicking on axis lines for scale
        float axisDists[3] = {
            DistPointToSegmentSS(mouse, sd.center, sd.tipX),
            DistPointToSegmentSS(mouse, sd.center, sd.tipY),
            DistPointToSegmentSS(mouse, sd.center, sd.tipZ)
        };
        int minAx = 0;
        for (int i = 1; i < 3; i++)
            if (axisDists[i] < axisDists[minAx]) minAx = i;
        if (axisDists[minAx] < kHitThreshold)
        {
            static const ManipID ids[3] = { ManipID::ScaleX, ManipID::ScaleY, ManipID::ScaleZ };
            return ids[minAx];
        }
    }

    return ManipID::None;
}

// ─── Drag Logic ──────────────────────────────────────────────────────────

static XMVECTOR GetDragPlaneNormal(ManipID manip, XMVECTOR axes[3], XMVECTOR camDir)
{
    switch (manip)
    {
    case ManipID::TranslatePlaneXY: return XMVector3Normalize(XMVector3Cross(axes[0], axes[1]));
    case ManipID::TranslatePlaneXZ: return XMVector3Normalize(XMVector3Cross(axes[0], axes[2]));
    case ManipID::TranslatePlaneYZ: return XMVector3Normalize(XMVector3Cross(axes[1], axes[2]));
    case ManipID::TranslateX: case ManipID::ScaleX:
    {
        XMVECTOR perp = XMVector3Cross(axes[0], camDir);
        return XMVector3Normalize(XMVector3Cross(axes[0], perp));
    }
    case ManipID::TranslateY: case ManipID::ScaleY:
    {
        XMVECTOR perp = XMVector3Cross(axes[1], camDir);
        return XMVector3Normalize(XMVector3Cross(axes[1], perp));
    }
    case ManipID::TranslateZ: case ManipID::ScaleZ:
    {
        XMVECTOR perp = XMVector3Cross(axes[2], camDir);
        return XMVector3Normalize(XMVector3Cross(axes[2], perp));
    }
    case ManipID::RotateX:  return axes[0];
    case ManipID::RotateY:  return axes[1];
    case ManipID::RotateZ:  return axes[2];
    case ManipID::RotateScreen: return camDir;
    default: return XMVectorSet(0, 1, 0, 0);
    }
}

static void BeginDrag(ManipID manip, Transform& t, XMVECTOR worldCenter,
                      XMVECTOR axes[3], XMVECTOR camDir, XMVECTOR quat,
                      XMMATRIX invViewProj, ImVec2 vpPos, ImVec2 vpSize)
{
    XMFLOAT3 pos, scl;
    XMMATRIX dummy; XMVECTOR dummyQ;
    DecomposeTransform(t, pos, scl, dummy, dummyQ);

    s_state.isDragging    = true;
    s_state.activeManip   = manip;
    s_state.origPosition  = pos;
    s_state.origScale     = scl;
    s_state.dragStartMouse = ImGui::GetIO().MousePos;
    XMStoreFloat4(&s_state.origQuat, quat);

    // Store axes at drag start (frozen during drag)
    for (int i = 0; i < 3; i++)
        XMStoreFloat4(&s_state.dragAxes[i], axes[i]);

    // Store rotation axis for this drag
    XMVECTOR rAxis = XMVectorSet(0,1,0,0);
    switch (manip)
    {
    case ManipID::RotateX: rAxis = axes[0]; break;
    case ManipID::RotateY: rAxis = axes[1]; break;
    case ManipID::RotateZ: rAxis = axes[2]; break;
    case ManipID::RotateScreen: rAxis = camDir; break;
    default: break;
    }
    XMStoreFloat4(&s_state.rotAxis, rAxis);

    XMVECTOR planeN = GetDragPlaneNormal(manip, axes, camDir);
    XMStoreFloat4(&s_state.dragPlaneNormal, planeN);
    XMStoreFloat4(&s_state.dragPlanePoint, worldCenter);

    // Compute initial ray-plane intersection
    ImVec2 mouse = ImGui::GetIO().MousePos;
    Ray ray = ScreenToRay(mouse, invViewProj, vpPos, vpSize);
    auto hit = RayPlaneIntersect(ray.origin, ray.dir, planeN, worldCenter);
    XMStoreFloat4(&s_state.dragStartHit, hit.hit ? hit.point : worldCenter);
}

static void UpdateDrag(Transform& t, XMVECTOR worldCenter,
                       XMMATRIX invViewProj, XMMATRIX viewProj,
                       ImVec2 vpPos, ImVec2 vpSize, float worldLen)
{
    ImVec2 mouse = ImGui::GetIO().MousePos;
    XMVECTOR dragPlaneN = XMLoadFloat4(&s_state.dragPlaneNormal);
    XMVECTOR dragPlaneP = XMLoadFloat4(&s_state.dragPlanePoint);
    XMVECTOR dragStartH = XMLoadFloat4(&s_state.dragStartHit);
    Ray ray = ScreenToRay(mouse, invViewProj, vpPos, vpSize);
    auto hit = RayPlaneIntersect(ray.origin, ray.dir, dragPlaneN, dragPlaneP);
    if (!hit.hit) return;

    // Load frozen axes from drag start
    XMVECTOR axes[3];
    for (int i = 0; i < 3; i++)
        axes[i] = XMLoadFloat4(&s_state.dragAxes[i]);

    ManipID m = s_state.activeManip;

    // ── Translation ──
    if (m >= ManipID::TranslateX && m <= ManipID::TranslatePlaneYZ)
    {
        XMVECTOR delta = XMVectorSubtract(hit.point, dragStartH);

        XMFLOAT3 newPos = s_state.origPosition;
        if (m == ManipID::TranslateX)
        {
            float d = XMVectorGetX(XMVector3Dot(delta, axes[0]));
            XMFLOAT3 a; XMStoreFloat3(&a, axes[0]);
            newPos.x += a.x * d; newPos.y += a.y * d; newPos.z += a.z * d;
        }
        else if (m == ManipID::TranslateY)
        {
            float d = XMVectorGetX(XMVector3Dot(delta, axes[1]));
            XMFLOAT3 a; XMStoreFloat3(&a, axes[1]);
            newPos.x += a.x * d; newPos.y += a.y * d; newPos.z += a.z * d;
        }
        else if (m == ManipID::TranslateZ)
        {
            float d = XMVectorGetX(XMVector3Dot(delta, axes[2]));
            XMFLOAT3 a; XMStoreFloat3(&a, axes[2]);
            newPos.x += a.x * d; newPos.y += a.y * d; newPos.z += a.z * d;
        }
        else // Plane translations
        {
            XMFLOAT3 df; XMStoreFloat3(&df, delta);
            newPos.x += df.x; newPos.y += df.y; newPos.z += df.z;
        }
        XMVECTOR origQ = XMLoadFloat4(&s_state.origQuat);
        ApplyTransformQuat(t, newPos, origQ, s_state.origScale);
    }

    // ── Rotation (quaternion-based) ──
    else if (m >= ManipID::RotateX && m <= ManipID::RotateScreen)
    {
        // Screen-space angular delta
        ImVec2 center = WorldToScreen(worldCenter, viewProj, vpPos, vpSize);
        float startAngle = atan2f(s_state.dragStartMouse.y - center.y,
                                  s_state.dragStartMouse.x - center.x);
        float curAngle = atan2f(mouse.y - center.y, mouse.x - center.x);
        float angleRad = curAngle - startAngle;

        // Build quaternion delta around the actual 3D rotation axis
        XMVECTOR rAxis = XMLoadFloat4(&s_state.rotAxis);
        XMVECTOR qDelta = XMQuaternionRotationAxis(rAxis, angleRad);

        // Apply: qNew = qDelta * qOrig (world-space rotation applied after original)
        XMVECTOR origQ = XMLoadFloat4(&s_state.origQuat);
        XMVECTOR qNew = XMQuaternionMultiply(origQ, qDelta);

        ApplyTransformQuat(t, s_state.origPosition, qNew, s_state.origScale);
    }

    // ── Scale ──
    else if (m >= ManipID::ScaleX && m <= ManipID::ScaleZ)
    {
        XMVECTOR delta = XMVectorSubtract(hit.point, dragStartH);
        int axisIdx = (int)m - (int)ManipID::ScaleX;
        float d = XMVectorGetX(XMVector3Dot(delta, axes[axisIdx]));
        float ratio = 1.0f + d / worldLen;
        ratio = std::max(0.01f, ratio);

        XMFLOAT3 newScl = s_state.origScale;
        float* sclArr = &newScl.x;
        sclArr[axisIdx] *= ratio;

        XMVECTOR origQ = XMLoadFloat4(&s_state.origQuat);
        ApplyTransformQuat(t, s_state.origPosition, origQ, newScl);
    }
}

// ─── Drawing ─────────────────────────────────────────────────────────────

static void DrawArrowHead(ImDrawList* dl, ImVec2 from, ImVec2 to, ImU32 col, float size)
{
    ImVec2 dir(to.x - from.x, to.y - from.y);
    float len = sqrtf(dir.x * dir.x + dir.y * dir.y);
    if (len < 1.0f) return;
    dir.x /= len; dir.y /= len;
    ImVec2 perp(-dir.y * size, dir.x * size);
    ImVec2 base(to.x - dir.x * size * 2, to.y - dir.y * size * 2);
    dl->AddTriangleFilled(to,
        ImVec2(base.x + perp.x, base.y + perp.y),
        ImVec2(base.x - perp.x, base.y - perp.y), col);
}

static void DrawScaleCube(ImDrawList* dl, ImVec2 pos, ImU32 col, float halfSize)
{
    dl->AddRectFilled(ImVec2(pos.x - halfSize, pos.y - halfSize),
                      ImVec2(pos.x + halfSize, pos.y + halfSize), col);
}

static void DrawRotationRing(ImDrawList* dl, ImVec2 center, float radius, ImU32 col,
                              int segments = kArcSegments)
{
    for (int i = 0; i < segments; i++)
    {
        float a0 = (float)i / (float)segments * XM_2PI;
        float a1 = (float)(i + 1) / (float)segments * XM_2PI;
        ImVec2 p0(center.x + cosf(a0) * radius, center.y + sinf(a0) * radius);
        ImVec2 p1(center.x + cosf(a1) * radius, center.y + sinf(a1) * radius);
        dl->AddLine(p0, p1, col, 1.5f);
    }
}

// Draw a 3D rotation ring: circle in the plane perpendicular to `axis`, projected to screen.
static void Draw3DRotationRing(ImDrawList* dl, XMVECTOR worldCenter, XMVECTOR axis,
                                float worldRadius, XMMATRIX viewProj, ImVec2 vpPos, ImVec2 vpSize,
                                ImU32 col, int segments = kArcSegments)
{
    // Build two tangent vectors perpendicular to axis
    XMVECTOR up = (fabsf(XMVectorGetY(axis)) < 0.99f)
        ? XMVectorSet(0, 1, 0, 0) : XMVectorSet(1, 0, 0, 0);
    XMVECTOR t1 = XMVector3Normalize(XMVector3Cross(axis, up));
    XMVECTOR t2 = XMVector3Normalize(XMVector3Cross(axis, t1));

    ImVec2 prev = {};
    for (int i = 0; i <= segments; i++)
    {
        float a = (float)i / (float)segments * XM_2PI;
        XMVECTOR pt = XMVectorAdd(worldCenter,
            XMVectorAdd(XMVectorScale(t1, cosf(a) * worldRadius),
                        XMVectorScale(t2, sinf(a) * worldRadius)));
        ImVec2 sp = WorldToScreen(pt, viewProj, vpPos, vpSize);
        if (i > 0)
            dl->AddLine(prev, sp, col, 1.5f);
        prev = sp;
    }
}

// ─── Main Implementation (templated) ─────────────────────────────────────

template<typename CamT>
static void DrawTransformGizmoImpl(CamT& camera, Transform& transform,
                                    bool localSpace, float pixelSize)
{
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 vpPos  = vp->Pos;
    ImVec2 vpSize = vp->Size;

    XMMATRIX view     = camera.GetViewMatrix();
    XMMATRIX proj     = camera.GetProjectionMatrix();
    XMMATRIX viewProj = XMMatrixMultiply(view, proj);
    XMMATRIX invViewProj = XMMatrixInverse(nullptr, viewProj);

    // Get object world position, rotation matrix, and quaternion
    XMFLOAT3 pos, scl;
    XMMATRIX rotMat;
    XMVECTOR quat;
    DecomposeTransform(transform, pos, scl, rotMat, quat);
    XMVECTOR worldCenter = XMLoadFloat3(&pos);

    // Camera direction for drag plane computation
    XMFLOAT3 eyePos = camera.GetEyePosition();
    XMVECTOR camDir = XMVector3Normalize(
        XMVectorSubtract(worldCenter, XMLoadFloat3(&eyePos)));

    // Compute axes (rotMat comes directly from quaternion or Euler — no roundtrip mismatch)
    XMVECTOR axes[3];
    for (int i = 0; i < 3; i++)
        axes[i] = GetWorldAxis(i, localSpace, rotMat);

    // Fixed screen-space pixel size for axes
    float axisPixelLen = pixelSize * 0.5f;

    // World-space length that maps to axisPixelLen on screen (constant screen size)
    float worldLen = ScreenSpaceScale(worldCenter, viewProj, proj, vpSize, axisPixelLen);

    // Compute screen-space data via world-space projection (preserves perspective depth cues)
    AxisScreenData sd = ComputeScreenData(worldCenter, axes, worldLen,
                                           viewProj, vpPos, vpSize);

    // ── Input handling ───────────────────────────────────────────────
    ImVec2 mouse = ImGui::GetIO().MousePos;

    if (!s_state.isDragging)
    {
        s_state.hoveredManip = HitTest(mouse, sd, worldCenter, axes, worldLen,
                                       viewProj, vpPos, vpSize);

        if (s_state.hoveredManip != ManipID::None && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            BeginDrag(s_state.hoveredManip, transform, worldCenter, axes, camDir, quat,
                      invViewProj, vpPos, vpSize);
        }
    }
    else
    {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            UpdateDrag(transform, worldCenter, invViewProj, viewProj,
                       vpPos, vpSize, worldLen);
        }
        else
        {
            // End drag
            s_state.isDragging  = false;
            s_state.activeManip = ManipID::None;
        }
    }

    // ── Rendering (mode-based: only draw active mode) ──────────────────
    ImDrawList* dl = ImGui::GetForegroundDrawList(ImGui::GetMainViewport());

    ManipID hovered = s_state.isDragging ? s_state.activeManip : s_state.hoveredManip;
    ImVec2 tips[3] = { sd.tipX, sd.tipY, sd.tipZ };
    static const char* axisLabels[3] = { "X", "Y", "Z" };

    if (s_mode == GizmoMode::Translate)
    {
        // Axis lines + arrow heads + labels
        ManipID transIDs[3] = { ManipID::TranslateX, ManipID::TranslateY, ManipID::TranslateZ };
        for (int i = 0; i < 3; i++)
        {
            ImU32 col = GetManipColor(transIDs[i], hovered);
            dl->AddLine(sd.center, tips[i], col, 2.5f);
            DrawArrowHead(dl, sd.center, tips[i], col, 5.0f);
            ImVec2 dir(tips[i].x - sd.center.x, tips[i].y - sd.center.y);
            float dirLen = sqrtf(dir.x * dir.x + dir.y * dir.y);
            if (dirLen > 1.0f)
            {
                dir.x /= dirLen; dir.y /= dirLen;
                ImVec2 lp(tips[i].x + dir.x * 12.0f, tips[i].y + dir.y * 12.0f);
                ImVec2 ls = ImGui::CalcTextSize(axisLabels[i]);
                dl->AddText(ImVec2(lp.x - ls.x * 0.5f, lp.y - ls.y * 0.5f),
                            IM_COL32(255, 255, 255, 230), axisLabels[i]);
            }
        }
        // Plane quads
        ManipID planeIDs[3] = { ManipID::TranslatePlaneXY, ManipID::TranslatePlaneXZ, ManipID::TranslatePlaneYZ };
        for (int i = 0; i < 3; i++)
        {
            ImU32 col = (hovered == planeIDs[i]) ? IM_COL32(255, 255, 80, 160) : kColorPlane;
            dl->AddQuadFilled(sd.planeQuad[i][0], sd.planeQuad[i][1],
                              sd.planeQuad[i][2], sd.planeQuad[i][3], col);
            dl->AddQuad(sd.planeQuad[i][0], sd.planeQuad[i][1],
                        sd.planeQuad[i][2], sd.planeQuad[i][3], IM_COL32(255, 255, 80, 200), 1.0f);
        }
    }
    else if (s_mode == GizmoMode::Rotate)
    {
        // 3D rotation rings only
        float ringWorldRadius = worldLen * kRotateArcRadius;
        ManipID rotIDs[3] = { ManipID::RotateX, ManipID::RotateY, ManipID::RotateZ };
        ImU32 rotColors[3] = { kColorX, kColorY, kColorZ };
        for (int i = 0; i < 3; i++)
        {
            ImU32 col = (hovered == rotIDs[i]) ? Brighten(rotColors[i]) : rotColors[i];
            Draw3DRotationRing(dl, worldCenter, axes[i], ringWorldRadius,
                               viewProj, vpPos, vpSize, col, kArcSegments);
        }
        // Screen rotation (outer ring)
        float outerR = sd.axisLength * 1.1f;
        ImU32 col = (hovered == ManipID::RotateScreen)
            ? Brighten(kColorScreen) : kColorScreen;
        DrawRotationRing(dl, sd.center, outerR, col, kArcSegments);
    }
    else if (s_mode == GizmoMode::Scale)
    {
        // Axis lines + scale cubes at tips + labels
        ManipID scaleIDs[3] = { ManipID::ScaleX, ManipID::ScaleY, ManipID::ScaleZ };
        for (int i = 0; i < 3; i++)
        {
            ImU32 col = GetManipColor(scaleIDs[i], hovered);
            dl->AddLine(sd.center, tips[i], col, 2.5f);
            DrawScaleCube(dl, tips[i], col, 4.0f);
            ImVec2 dir(tips[i].x - sd.center.x, tips[i].y - sd.center.y);
            float dirLen = sqrtf(dir.x * dir.x + dir.y * dir.y);
            if (dirLen > 1.0f)
            {
                dir.x /= dirLen; dir.y /= dirLen;
                ImVec2 lp(tips[i].x + dir.x * 12.0f, tips[i].y + dir.y * 12.0f);
                ImVec2 ls = ImGui::CalcTextSize(axisLabels[i]);
                dl->AddText(ImVec2(lp.x - ls.x * 0.5f, lp.y - ls.y * 0.5f),
                            IM_COL32(255, 255, 255, 230), axisLabels[i]);
            }
        }
    }

    // Center dot (always)
    dl->AddCircleFilled(sd.center, 3.0f, IM_COL32(255, 255, 255, 220), 12);

    // No window cleanup needed — using foreground draw list directly
}

// ─── Public API ──────────────────────────────────────────────────────────

void DrawTransformGizmo(Camera& camera, Transform& transform,
                        bool localSpace, float pixelSize)
{
    DrawTransformGizmoImpl(camera, transform, localSpace, pixelSize);
}

void DrawTransformGizmo(CameraComponent& camera, Transform& transform,
                        bool localSpace, float pixelSize)
{
    DrawTransformGizmoImpl(camera, transform, localSpace, pixelSize);
}

} // namespace Luna

