#pragma once
#include <DirectXMath.h>
#include <imgui.h>

namespace Luna
{
using namespace DirectX;

// ── World ↔ Screen projection ────────────────────────────────────────────

inline ImVec2 WorldToScreen(XMVECTOR worldPos, XMMATRIX viewProj,
                            ImVec2 vpPos, ImVec2 vpSize)
{
    XMVECTOR clip = XMVector4Transform(
        XMVectorSetW(worldPos, 1.0f), viewProj);
    float w = XMVectorGetW(clip);
    if (w < 1e-5f) return ImVec2(-1e4f, -1e4f); // behind camera

    float ndcX = XMVectorGetX(clip) / w;
    float ndcY = XMVectorGetY(clip) / w;

    // NDC [-1,1] → screen coords (Y flipped for screen space)
    float sx = vpPos.x + (ndcX * 0.5f + 0.5f) * vpSize.x;
    float sy = vpPos.y + (1.0f - (ndcY * 0.5f + 0.5f)) * vpSize.y;
    return ImVec2(sx, sy);
}

inline float WorldToScreenW(XMVECTOR worldPos, XMMATRIX viewProj)
{
    XMVECTOR clip = XMVector4Transform(
        XMVectorSetW(worldPos, 1.0f), viewProj);
    return XMVectorGetW(clip);
}

// ── Screen → Ray (unproject) ─────────────────────────────────────────────

struct Ray { XMVECTOR origin; XMVECTOR dir; };

inline Ray ScreenToRay(ImVec2 screenPos, XMMATRIX invViewProj,
                       ImVec2 vpPos, ImVec2 vpSize)
{
    float ndcX = ((screenPos.x - vpPos.x) / vpSize.x) * 2.0f - 1.0f;
    float ndcY = 1.0f - ((screenPos.y - vpPos.y) / vpSize.y) * 2.0f;

    XMVECTOR nearPt = XMVector4Transform(
        XMVectorSet(ndcX, ndcY, 0.0f, 1.0f), invViewProj);
    XMVECTOR farPt  = XMVector4Transform(
        XMVectorSet(ndcX, ndcY, 1.0f, 1.0f), invViewProj);

    nearPt = XMVectorDivide(nearPt, XMVectorSplatW(nearPt));
    farPt  = XMVectorDivide(farPt,  XMVectorSplatW(farPt));

    XMVECTOR dir = XMVector3Normalize(XMVectorSubtract(farPt, nearPt));
    return { nearPt, dir };
}

// ── Ray–Plane intersection ───────────────────────────────────────────────

struct RayPlaneHit { bool hit; XMVECTOR point; float t; };

inline RayPlaneHit RayPlaneIntersect(XMVECTOR rayO, XMVECTOR rayD,
                                     XMVECTOR planeN, XMVECTOR planeP)
{
    float denom = XMVectorGetX(XMVector3Dot(rayD, planeN));
    if (fabsf(denom) < 1e-7f) return { false, XMVectorZero(), 0.0f };
    float t = XMVectorGetX(XMVector3Dot(XMVectorSubtract(planeP, rayO), planeN)) / denom;
    if (t < 0.0f) return { false, XMVectorZero(), t };
    return { true, XMVectorAdd(rayO, XMVectorScale(rayD, t)), t };
}

// ── Screen-space distance: point to line segment ─────────────────────────

inline float DistPointToSegmentSS(ImVec2 p, ImVec2 a, ImVec2 b)
{
    ImVec2 ab(b.x - a.x, b.y - a.y);
    ImVec2 ap(p.x - a.x, p.y - a.y);
    float lenSq = ab.x * ab.x + ab.y * ab.y;
    if (lenSq < 1e-6f) return sqrtf(ap.x * ap.x + ap.y * ap.y);
    float t = (ap.x * ab.x + ap.y * ab.y) / lenSq;
    t = (t < 0.0f) ? 0.0f : (t > 1.0f) ? 1.0f : t;
    float dx = p.x - (a.x + ab.x * t);
    float dy = p.y - (a.y + ab.y * t);
    return sqrtf(dx * dx + dy * dy);
}

// ── Screen-space distance: point to point ────────────────────────────────

inline float DistSS(ImVec2 a, ImVec2 b)
{
    float dx = a.x - b.x, dy = a.y - b.y;
    return sqrtf(dx * dx + dy * dy);
}

// ── Screen-space constant-size scale factor ──────────────────────────────
// Returns a world-space length that maps to `desiredPixels` on screen.
// Requires the projection matrix to extract the focal length.

inline float ScreenSpaceScale(XMVECTOR worldPos, XMMATRIX viewProj, XMMATRIX proj,
                              ImVec2 vpSize, float desiredPixels)
{
    float w = WorldToScreenW(worldPos, viewProj);
    if (w < 1e-5f) return 1.0f;
    // proj._22 = 1/tan(fov/2) for a perspective projection.
    // Pixels per world unit at distance w = vpSize.y * proj._22 / (2 * w).
    // Invert to get world units per pixel = 2 * w / (vpSize.y * proj._22).
    XMFLOAT4X4 projF;
    XMStoreFloat4x4(&projF, proj);
    float focalY = (projF._22 > 1e-5f) ? projF._22 : 1.0f;
    float worldPerPixel = (2.0f * w) / (vpSize.y * focalY);
    return worldPerPixel * desiredPixels;
}

} // namespace Luna

