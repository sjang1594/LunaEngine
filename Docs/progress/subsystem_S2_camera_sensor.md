# S2 — Camera Sensor Offscreen Rendering (DX12)

**Date:** 2026-05-28
**Track:** Simulation S-series
**Backend:** DX12 (Vulkan parity: S2b)
**Status:** ✅ Complete (build verified, 0 errors)

---

## Overview

S2 wires real offscreen GPU rendering into the `CameraSensor` stub from S1. Each camera sensor now produces:
- A rendered + lens-distorted + noise-applied RGBA8 output (displayed live in ImGui)
- CPU-readback buffer (`CameraSensor::rgbOutput`) for S5 nuScenes export
- Correct pinhole + Brown-Conrady (k1, k2, k3, p1, p2) distortion matching OpenCV model
- Per-sensor GPU frustum culling (reuses `_gpuCullPipeline` with sensor-specific frustum planes)

---

## Architecture

### Per-camera GPU resources (`DX12CameraResources`)
Each `CameraSensor*` maps to a resource bundle in `DX12Backend::_cameraRTs`:

```
G-buffer[3]   RGBA8/RGBA16F/RGBA8   camera-viewpoint G-buffer MRTs
depth         R32_TYPELESS (D32F)
litRT         RGBA16F               IBL-only deferred lighting output
distortRT     RGBA8_UNORM           post-distortion + noise + sRGB (ImGui + readback source)
readbackRGB   READBACK              CPU-accessible RGBA8 → CameraSensor::rgbOutput
readbackDepth READBACK              CPU-accessible float depth
sensorArgBuffer  DEFAULT UAV        per-sensor GPU cull indirect draw args
sensorDrawCount  DEFAULT UAV        draw count for ExecuteIndirect
sensorCullCB[2]  UPLOAD CBV         CullConstants with sensor frustum planes (double-buffered)
distortCB[2]     UPLOAD CBV         DistortConstants (double-buffered)
```

### Render pipeline (per sensor, per display frame, after CompositeFrame)
1. **Transition**: G-buffer[0-2] PIXEL_SHADER_RESOURCE → RENDER_TARGET, depth DEPTH_READ → DEPTH_WRITE
2. **GPU frustum cull**: dispatch `_gpuCullPipeline` with sensor frustum planes → `sensorArgBuffer`
3. **G-buffer fill**: ExecuteIndirect using `sensorArgBuffer` with sensor view/proj (GPU-driven path) OR per-draw fallback
4. **Transition**: G-buffer → SRV, depth → DEPTH_READ; litRT stays RENDER_TARGET
5. **Sensor lighting** (`sensor_lighting.frag.hlsl`): IBL-only Cook-Torrance (no SSAO/CSM/GI)
6. **litRT** → NON_PIXEL_SHADER_RESOURCE; distortion compute dispatches
7. **Distort compute** (`camera_distort.comp.hlsl`): Brown-Conrady warp + EV100 exposure + shot+read noise + sRGB → distortRT
8. **Copy** distortRT → readbackRGB (async, read next frame)

---

## New Shaders

| Shader | Root Sig | Purpose |
|--------|----------|---------|
| `sensor_lighting.frag.hlsl` | `SensorLighting` | IBL-only deferred (b0=SceneConst; t0-t2=GB; t3=depth; t4-t6=IBL; s0-s2) |
| `camera_distort.comp.hlsl` | `CameraDistort` | Brown-Conrady + exposure + noise + sRGB (b0=DistortConst; t0=litRT; u0=distortRT; s0) |

### Brown-Conrady distortion model
```
r² = x²+y²
x' = x(1+k1r²+k2r⁴+k3r⁶) + 2p1xy + p2(r²+2x²)
y' = y(1+k1r²+k2r⁴+k3r⁶) + p1(r²+2y²) + 2p2xy
```
OpenCV-compatible (fx, fy, cx, cy derived from fovDeg + resolution).

---

## Files Modified/Created

| File | Change |
|------|--------|
| `Sensor/SensorTypes.h` | Added `k3`, `exposureEV100`, `shotNoiseFactor`, `readNoiseSigma`, `captureIntervalFrames`, `CameraIntrinsics` struct |
| `Sensor/CameraSensor.h` | Added `hasNewFrame`, `gpuTextureHandle` |
| `Graphics/IPipeline.h` | Added `RootSignatureLayout::SensorLighting`, `CameraDistort` |
| `Renderer/HAL/Public/IRenderBackend.h` | Added `RenderCameraSensors()` virtual |
| `Renderer/DX12/Public/DX12Backend.h` | Added `DX12CameraResources` struct, `_cameraRTs` map, `_sensorLightingPipeline`, `_cameraDistortPipeline`, `_cachedLightDir/Color` |
| `Renderer/DX12/private/DX12Backend.cpp` | `InitCameraResources()`, `DestroyCameraResources()`, `RenderCameraSensorInternal()`, `RenderCameraSensors()` + pipeline init + shutdown cleanup + light caching |
| `Renderer/DX12/private/DX12Pipeline.cpp` | Root sig builders for `SensorLighting` (6 params, 3 samplers) and `CameraDistort` (3 params, 1 sampler) |
| `Application/Application.cpp` | Hook `RenderCameraSensors()` after `CompositeFrame()` |
| `Shaders/sensor_lighting.frag.hlsl` | New |
| `Shaders/camera_distort.comp.hlsl` | New |
| `LunaApp/src/SensorLayer.cpp` | Real `ImGui::Image()` display + k1/k2/k3/exposure sliders + nuScenes-6 preset button |

---

## Known Limitations (deferred to S2b/S3)

- **Vulkan parity**: DX12 only. Vulkan sensor rendering is S2b.
- **Hi-Z disabled for sensor cameras**: Hi-Z was built with display camera matrices — not valid for sensor view. Sensor GPU cull uses frustum-only (no Hi-Z occlusion). Proper per-sensor Hi-Z is a future optimization.
- **Readback latency**: CPU reads `rgbOutput` 1 frame after GPU write. Acceptable for 12 Hz capture rate.
- **Per-sensor mesh shader path not implemented**: Falls back to `_indirectGBufPipeline` (ExecuteIndirect). Mesh shader variant would require separate CullConstants upload to mesh shader CB.

---

## nuScenes-6 Preset (in SensorLayer)

Click **"Add nuScenes-6 Preset"** in Camera Sensor Output panel to spawn 6 cameras on the first scene object:
- CAM_FRONT (70°), CAM_FRONT_LEFT/RIGHT (70°, ±55°), CAM_BACK (110°), CAM_BACK_LEFT/RIGHT (70°, ±110°)
- Resolution: 1600×900, nearZ=0.1m, farZ=150m

---

## Next

- **S2b**: Vulkan parity for camera sensor rendering
- **S3**: LiDAR DXR RayQuery compute (reuses existing BLAS/TLAS from shadow pass)
- **S5**: nuScenes export (PNG + camera.json + .pcd binary) — `CameraSensor::rgbOutput` already populated
