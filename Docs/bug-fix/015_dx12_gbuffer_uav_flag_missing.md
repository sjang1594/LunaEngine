# Bug #015: DX12 G-Buffer Missing ALLOW_UNORDERED_ACCESS — Device Removed on Phase 32 Init

**Date**: 2026-05-27
**Severity**: Critical (device lost, app crash)
**Backend**: DX12
**Status**: Fixed

---

## Symptoms

On startup after Phase 32 (Visibility Buffer) was added:

```
[ERROR] IBL: failed to create equirect GPU texture (hr=0x887A0005)
Assertion failed: 0 && "ImGui_ImplDX12_CreateDeviceObjects() failed!"
```

Both errors appear despite `environment.hdr` existing on disk and stb_image loading it
successfully. The app crashes before the first frame.

---

## Root Cause

`CreateGBuffer()` created G-buffer textures with `D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET`
only (no `ALLOW_UNORDERED_ACCESS`):

```cpp
// DX12Backend.cpp — CreateGBuffer() — before fix
texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
```

Phase 32's `CreateVisibilityResources()` then calls `CreateUnorderedAccessView` on these
G-buffer textures so the visibility shade compute can write reconstructed G-buffer data:

```cpp
dev->CreateUnorderedAccessView(_gbuffer[i].Get(), nullptr, &uavd, cpuH);
```

This is illegal in DX12 — creating a UAV descriptor for a resource that lacks
`ALLOW_UNORDERED_ACCESS`. The D3D12 Debug Layer fires
`D3D12_MESSAGE_ID_CREATEUNORDEREDACCESSVIEW_INVALIDRESOURCE` and removes the device.

Subsequent calls on the removed device fail:
- `LoadHDREnvironment()` → `D3D12MA::CreateResource()` → `DXGI_ERROR_DEVICE_REMOVED`
- `ImGui_ImplDX12_CreateDeviceObjects()` → assertion failure

---

## Call Chain

```
LoadTestScene()
  └─ backend->LoadMeshes()
       └─ DX12Backend::LoadMeshes()
            └─ CreateVisibilityResources()           // Phase 32 init
                 └─ CreateUnorderedAccessView(_gbuffer[i])  // ← ILLEGAL: no UAV flag
                      └─ D3D12 Debug Layer: DEVICE REMOVED
  └─ backend->LoadHDREnvironment()
       └─ D3D12MA::CreateResource()  → 0x887A0005  ← secondary failure
  └─ (first frame) ImGui_ImplDX12_CreateDeviceObjects()  ← secondary failure
```

---

## Fix

Add `D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS` to G-buffer resource creation.

```cpp
// DX12Backend.cpp — CreateGBuffer() — after fix
// Phase 32: ALLOW_UNORDERED_ACCESS required — visibility shade compute writes G-buffer as UAVs.
texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
              | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
```

`ALLOW_RENDER_TARGET | ALLOW_UNORDERED_ACCESS` is valid on all DX12 hardware for
standard color formats (RGBA8_UNORM, RGBA16F). The optimized clear value is unaffected.

---

## Files Modified

| File | Change |
|------|--------|
| `DX12/Private/DX12Backend.cpp` | Added `ALLOW_UNORDERED_ACCESS` to G-buffer texture flags in `CreateGBuffer()` |
| `DX12/Private/DX12Backend.cpp` | Added HRESULT logging to IBL equirect GPU texture error |

---

## Note on R32_UINT Visibility RT

The visibility RT itself (`_visRT`, R32_UINT) was intentionally created with
`ALLOW_RENDER_TARGET` only — combining both flags on R32_UINT causes D3D12 debug validation
failures on some drivers. The vis RT is read by shade compute via SRV, not UAV, so no
UAV flag is needed there.
