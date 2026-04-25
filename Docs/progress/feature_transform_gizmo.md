# Feature: Isaac Sim-style Universal Transform Gizmo

**Date:** 2026-04-24  
**Status:** Implemented  
**Component:** UI / Editor Tools

---

## Overview

Isaac Sim-style universal transform gizmo that shows **all manipulators simultaneously** (translate, rotate, scale) without mode switching. Includes object selection system and Scene Inspector integration.

Unlike Unreal Engine's mode-based approach (W/E/R hotkeys), the universal gizmo lets users grab any manipulator directly — reducing context switching for scene authoring workflows.

---

## Architecture

### Files Created

| File | Purpose |
|------|---------|
| `LunaEngine/src/LunaEngine/UI/GizmoMath.h` | Header-only math utilities: WorldToScreen, ScreenToRay, RayPlaneIntersect, ScreenSpaceScale |
| `LunaEngine/src/LunaEngine/UI/Selection.h` | Minimal selection state: `selectedIndex` + `selectedObject` with global accessor |
| `LunaEngine/src/LunaEngine/UI/TransformGizmo.h` | Public API: `DrawTransformGizmo()`, `IsGizmoDragging()`, `IsGizmoHovered()` |
| `LunaEngine/src/LunaEngine/UI/TransformGizmo.cpp` | Full gizmo implementation (~660 lines) |

### Files Modified

| File | Changes |
|------|---------|
| `LunaApp/src/LunaApp.cpp` | Selection in Scene Inspector, gizmo drawing + toolbar, auto-expand tree node |
| `LunaEngine/src/LunaEngine/Application/Application.cpp` | Camera orbit suppression via `IsGizmoDragging()` / `IsGizmoHovered()` |

---

## Manipulator Types

| Type | Visual | Color | Interaction |
|------|--------|-------|-------------|
| Translate X/Y/Z | Arrow + line | Red/Green/Blue | Drag along axis via ray-plane intersection |
| Translate Plane XY/XZ/YZ | Filled quad at axis intersection | Yellow | Drag on plane |
| Rotate X/Y/Z | 3D projected ring ⊥ axis | Red/Green/Blue (semi-transparent) | Screen-space angular delta |
| Rotate Screen | Outer 2D circle | White/Gray | Free rotation around view direction |
| Scale X/Y/Z | Cube at axis tip | Red/Green/Blue | Drag ratio from center |

---

## Rendering

- **ImDrawList overlay** — no GPU shaders, drawn on `ImGui::GetForegroundDrawList()`
- **Screen-space constant size** — `ScreenSpaceScale()` computes world-space length that maps to fixed pixel size, accounting for perspective `proj._22` focal length
- **3D projected rotation rings** — each ring is a circle in the plane perpendicular to its axis, projected through viewProj (not flat 2D circles)
- **Axis labels** — "X", "Y", "Z" at arrow tips matching ViewGizmo convention
- **Hover highlight** — brighten color ×1.5 on hovered manipulator
- **Depth sorting** — back-facing elements drawn first (painter's algorithm)

---

## Coordinate Convention

Engine is **Y-up** internally but **displays as Z-up** (Y↔Z swap in gizmo):

| Display Axis | Color | Label | World Direction | Euler Mapping |
|-------------|-------|-------|-----------------|---------------|
| X | Red | "X" | `(1, 0, 0)` | `rotation.x` (pitch) |
| Y | Green | "Y" | `(0, 0, 1)` | `rotation.z` (roll) |
| Z (up) | Blue | "Z" | `(0, 1, 0)` | `rotation.y` (yaw) |

This matches the ViewGizmo's `kAxes[]` definition exactly.

---

## Input Priority

GLFW mouse callbacks fire during `glfwPollEvents()` (before ImGui render). The gizmo detects interaction during `OnUIRender()` (after). This creates a one-frame race:

**Solution:** `IsGizmoHovered()` returns true when the mouse is over any manipulator. Both `OnMouseButton` and `OnMouseMove` check this alongside `IsGizmoDragging()` to suppress camera orbit before the gizmo registers the drag.

```
GLFW callback:  if (WantCaptureMouse || IsGizmoDragging() || IsGizmoHovered()) → skip camera
ImGui render:   DrawTransformGizmo() → HitTest → BeginDrag/UpdateDrag
```

---

## Selection System

Minimal MVP selection via `Selection.h`:

```cpp
struct Selection {
    int selectedIndex = -1;
    shared_ptr<GameObject> selectedObject = nullptr;
    bool HasSelection() const;
    void Clear();
};
Selection& GetSelection(); // global accessor
```

Scene Inspector integration:
- `ImGuiTreeNodeFlags_Selected` highlights selected object
- Click label → sets selection (click arrow → expand/collapse)
- `ImGui::SetNextItemOpen(true)` auto-expands tree node on new selection
- Gizmo toolbar shows "Local Space" toggle + selection status

---

## Hit-Testing Priority

When multiple manipulators overlap, hit-test priority (highest first):

1. **Plane quads** (point-in-convex-quad test)
2. **Axis arrows** (distance to line segment < 7px)
3. **Scale cubes** (distance to tip < 10.5px)
4. **Rotation rings** (|distance_from_center − ring_radius| < 7px, closest axis wins)
5. **Screen rotation** (outer ring ± 7px)

---

## Drag Mechanics

### Translation
- Construct drag plane containing the axis and most facing the camera
- Ray-plane intersection each frame; delta projected onto axis (single-axis) or applied directly (plane mode)
- `newPos = origPos + axisDir * dot(delta, axisDir)`

### Rotation
- Screen-space angular delta: `atan2(mouse - center)` current vs start
- Applied to Euler angle corresponding to the axis (with Y↔Z mapping)

### Scale
- Same ray-plane intersection as translate
- Ratio = `1 + dot(delta, axis) / worldLen`
- Applied to the scale component with Y↔Z mapping

### Transform Write-Back
- On drag start: `DecomposeTransform()` snapshots pos/rot/scl (handles `useRawMatrix`)
- During drag: `ApplyTransform()` sets `useRawMatrix = false` and writes new pos/rot/scl
- Scene Inspector reads the same `Transform` component — values reflect in real-time

---

## Dependencies

- **ImGui** — `ImDrawList`, `GetForegroundDrawList()`, input queries
- **DirectXMath** — XMVECTOR/XMMATRIX for projection, ray math
- **Existing systems** — Camera/CameraComponent (view/proj matrices), Transform component, GameObject/Scene

No GPU resources, no shader changes, no backend-specific code.

