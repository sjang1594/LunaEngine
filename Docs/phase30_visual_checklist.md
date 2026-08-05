# Phase 30 — GI Visual Checklist

## Biggest Observable Differences (vs IBL-only)

### 1. Color Bleeding — most obvious
Place a brightly colored surface (red wall, green floor) next to a neutral one. The neutral surface should pick up a tint from SSGI bouncing off the colored surface. In Sponza: the red/blue banners bleeding onto adjacent stone columns.

### 2. Concave Surfaces / Corners Get Brighter
The opposite of SSAO — tight corners and crevices now receive bounce light from nearby surfaces instead of just flat IBL. A corner that was uniformly dark should show a subtle brightening from surfaces bouncing light into it.

### 3. Ground-to-Underside Bounce
Look at the underside of overhanging geometry (arches, ledges). With IBL-only they get a flat ambient. With SSGI the ground beneath them bounces light up onto the underside — especially visible if the ground is lit by direct sun.

### 4. Before/After Toggle Test
The best check: add an ImGui checkbox to toggle between `_lightingPipelineGI` and `_lightingPipelineIBL` live. The difference will be clearest in:
- Enclosed spaces (inside Sponza)
- Frames where colored geometry is visible on-screen

### 5. Camera-Rotation Artifact (confirms SSGI is active)
Spin the camera quickly. The indirect lighting will momentarily lag/change as new geometry enters the screen — telltale sign SSGI is running. IBL-only has no such behavior since it's view-independent.

### 6. First-Frame Buildup (confirms temporal accumulation)
On scene load, hold still. The indirect lighting should visibly brighten over 3–4 frames as the temporal history accumulates (`α = 0.1` per frame → ~10 frames to 65% convergence).

## What Won't Look Different
- Probes contribute at 0.3× and currently sample IBL-only (TODO stub) — effect is almost indistinguishable from existing IBL ambient.
- Smooth/metallic surfaces — dominated by specular IBL; SSGI only adds to the diffuse ambient term.
- Open sky areas with no nearby geometry — SSGI rays miss and contribute zero.

## Best Test Configuration
Sponza interior, camera looking at a colored banner adjacent to stone. Maximum-impact configuration for what Phase 30 implements.