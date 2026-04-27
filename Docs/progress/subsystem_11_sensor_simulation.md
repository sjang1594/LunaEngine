# S1 — Sensor Simulation Foundation

**Date:** 2026-04-27  
**Track:** Simulation (S-series) — separate from rendering phases  
**Backend:** Backend-agnostic (DX12/Vulkan)  
**Status:** ✅ Complete (build verified)

---

## Overview

Foundation layer for sensor simulation and visualization. Adds a sensor placement system, three sensor types (Camera, LiDAR, FMCW Radar), ImGui visualization panels, and a fusion bird's-eye view. This phase establishes the data structures, component model, and UI — GPU-accelerated simulation is deferred to subsequent phases.

---

## Architecture

```
GameObject
  └─ SensorComponent (ComponentType::SENSOR)
       └─ vector<ISensor>
            ├─ CameraSensor   (config + view/proj computation)
            ├─ LiDARSensor    (config + ray direction generation)
            └─ RadarSensor    (config + range-Doppler test pattern)

SensorManager (singleton)
  └─ Update(dt): iterates all SensorComponents, ticks each ISensor::Simulate()

SensorLayer (ImGui Layer)
  ├─ Sensor Placement Panel   — add/remove/configure sensors per GameObject
  ├─ Camera Sensor Output     — RGB + depth preview (placeholder for Phase 2)
  ├─ LiDAR Sensor Output      — BEV point cloud canvas with range rings
  ├─ Radar Sensor Output      — range-Doppler heatmap visualization
  └─ Fusion BEV Panel         — combined FOV wedges, points, detections
```

---

## Sensor Types

### Camera Sensor
- Resolution, FOV, near/far, Brown-Conrady distortion (k1/k2/p1/p2), noise sigma
- Computes view/projection matrices from owning object's world transform × extrinsic offset
- S2: offscreen rendering via `IRenderBackend::RenderOffscreen()`

### LiDAR Sensor
- Scan pattern (Spinning/Solid State), H/V FOV, angular resolution, max range, noise
- Generates ray direction buffer in sensor-local space at config time
- S3: GPU raycasting via DXR `RayQuery` compute shader

### FMCW Radar Sensor
- Center frequency, bandwidth, chirp duration, num chirps, range/doppler bins
- S4: scene object query, IF signal synthesis, 2D FFT → range-Doppler map
- Currently emits noise-floor test pattern

---

## Visualization Panels

| Panel | Content |
|-------|---------|
| **Sensor Placement** | Per-object sensor list with add/remove, type-specific config editors, extrinsic offset |
| **Camera Output** | Side-by-side RGB + depth preview (placeholder until S2 offscreen rendering) |
| **LiDAR Output** | BEV canvas with range rings, point cloud overlay (populated in S3) |
| **Radar Output** | Range-Doppler heatmap with viridis colormap, axis labels |
| **Fusion BEV** | Combined bird's-eye view: camera FOV (blue), radar FOV (orange), LiDAR points (green), radar detections (red diamonds), ego marker, range grid |

---

## Files

| File | Type | Description |
|------|------|-------------|
| `Sensor/SensorTypes.h` | New | Enums, configs, output structs for all sensor types |
| `Sensor/ISensor.h` | New | Base sensor interface with extrinsic, tick rate, Simulate() |
| `Sensor/CameraSensor.h` | New | Camera sensor: view/proj computation from config |
| `Sensor/LiDARSensor.h` | New | LiDAR sensor: ray direction generation from scan config |
| `Sensor/RadarSensor.h` | New | Radar sensor: FMCW config + test pattern generator |
| `Sensor/SensorComponent.h` | New | ECS component holding vector of ISensor instances |
| `Sensor/SensorComponent.cpp` | New | Component update + factory methods |
| `Sensor/SensorManager.h` | New | Singleton that ticks all sensors in active scene |
| `Sensor/SensorManager.cpp` | New | Scene iteration + per-sensor Simulate() dispatch |
| `Components/Component.h` | Modified | Added `SENSOR` to `ComponentType` enum |
| `LunaApp/src/SensorLayer.h` | New | Layer declaration for sensor visualization panels |
| `LunaApp/src/SensorLayer.cpp` | New | Five ImGui panels: placement, camera, LiDAR, radar, fusion BEV |
| `LunaApp/src/LunaApp.cpp` | Modified | Push SensorLayer, sensor icon in Scene Inspector |

---

## Next Phases (Simulation Track)

| Phase | Goal |
|-------|------|
| **S2** | Camera sensor offscreen rendering (reuse G-buffer pipeline for alternate viewpoint) |
| **S3** | LiDAR GPU raycasting (DXR RayQuery compute shader against scene BLAS/TLAS) |
| **S4** | Radar scene query + CPU FFT (pocketfft) → real range-Doppler map |
| **S5** | Sensor data export (PNG, PLY/PCD, CSV) via stb_image_write |

> **Note:** Simulation phases are deferred until the rendering track is complete. S2–S3 depend on rendering infrastructure (offscreen render targets, RayQuery compute) that may be added during rendering phases.

