# Bug #013: DX12 IBL Pipeline Creation Failure — Dangling Pointers & Root Param Mismatch

**Date**: 2026-04-26  
**Severity**: Critical (crash)  
**Backend**: DX12  
**Status**: Fixed

---

## Symptoms

1. Application startup error:
   ```
   [ERROR] CreateGraphicsPipelineState failed: 0x80070057
   [ERROR] IBL: deferred_lighting_ibl pipeline FAILED — IBL lighting disabled, falling back to HDR
   ```

2. After partial fix, access violation crash:
   ```
   Exception 0xc0000005 encountered at address 0x7ff887ab9805: Access violation reading location 0x00000000
   ```

---

## Root Causes

### Issue #1: Dangling Pointers in Root Signature Creation

In `DX12Pipeline::CreateRootSignature()`, local arrays were declared inside each if-block:

```cpp
else if (_desc.rootLayout == RootSignatureLayout::DeferredLightingIBL)
{
    CD3DX12_DESCRIPTOR_RANGE ranges[6];   // LOCAL — goes out of scope
    CD3DX12_ROOT_PARAMETER params[8];     // LOCAL — goes out of scope
    D3D12_STATIC_SAMPLER_DESC samplers[3];// LOCAL — goes out of scope
    
    // ... init arrays ...
    
    rootSigDesc.Init(8, params, 3, samplers, ...);
}  // <-- ranges, params, samplers OUT OF SCOPE here

// Called AFTER the if-else chain — reads garbage memory!
hr = D3D12SerializeRootSignature(&rootSigDesc, ...);
```

`CD3DX12_ROOT_SIGNATURE_DESC::Init()` stores **pointers** to the passed arrays. When the if-block ends, those pointers become dangling. `D3D12SerializeRootSignature` then reads garbage, causing undefined behavior.

**Why DeferredLightingIBL failed but simpler layouts worked**: The larger arrays (8 params, 6 ranges, 3 samplers) caused different stack memory reuse patterns, making corruption manifest more reliably. Simpler layouts happened to "work" due to luck with stack memory not being immediately overwritten.

### Issue #2: Root Parameter Count Mismatch

The binding code in `DX12Backend.cpp` called:

```cpp
cmd->SetGraphicsRootDescriptorTable(7, MakeGpu(_clusterLightSRVIndex));
cmd->SetGraphicsRootDescriptorTable(8, MakeGpu(_clusterCountsSRVIndex));   // ERROR!
cmd->SetGraphicsRootDescriptorTable(9, MakeGpu(_clusterIndicesSRVIndex));  // ERROR!
```

But the root signature only had **8 parameters** (indices 0-7). Setting params 8 and 9 caused an access violation.

Additionally, the original design assumed t9-t11 SRVs were **contiguous** in the descriptor heap (one table covering 3 SRVs). They were not — UAVs were interleaved:
- `_clusterLightSRVIndex` (t9)
- `_clusterCountsSRVIndex` (t10)
- `_clusterCountsUAVIndex` ← breaks contiguity
- `_clusterIndicesSRVIndex` (t11)

---

## Fix

### File: `LunaEngine/src/LunaEngine/Renderer/DX12/private/DX12Pipeline.cpp`

#### 1. Move arrays to function scope with zero-initialization

```cpp
bool DX12Pipeline::CreateRootSignature(const ComPtr<ID3D12Device> &device)
{
    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc;
    ComPtr<ID3DBlob>            serializedBlob;
    ComPtr<ID3DBlob>            errorBlob;
    HRESULT                     hr;

    // Declare arrays at function scope to prevent dangling pointers.
    // rootSigDesc.Init() stores pointers to these, so they must persist until
    // D3D12SerializeRootSignature is called after the if-else chain.
    // Zero-initialize to avoid garbage in unused slots.
    CD3DX12_ROOT_PARAMETER      rootParams[10] = {};  // max: DeferredLightingIBL uses 10
    CD3DX12_DESCRIPTOR_RANGE    descRanges[8] = {};   // max: DeferredLightingIBL uses 8
    D3D12_STATIC_SAMPLER_DESC   staticSamplers[3] = {}; // max: 3 samplers

    if (_desc.rootLayout == RootSignatureLayout::PBR)
    // ... each branch now uses rootParams[], descRanges[], staticSamplers[]
```

#### 2. Expand DeferredLightingIBL to 10 root parameters

Changed from 8 params with one combined t9-t11 table to 10 params with separate tables:

```cpp
else if (_desc.rootLayout == RootSignatureLayout::DeferredLightingIBL)
{
    // Phase 14+24: Deferred Lighting with IBL + Clustered Lighting
    //   params[0] b0 — SceneConstants CBV
    //   params[1] b1 — ClusterParams CBV (Phase 24)
    //   params[2]    — SRV table t0-t4 (GB0/1/2/depth/shadow)
    //   params[3]    — SRV t5 (SSAO blur)
    //   params[4]    — SRV t6 (irradiance cubemap)
    //   params[5]    — SRV t7 (prefiltered env cubemap)
    //   params[6]    — SRV t8 (BRDF LUT)
    //   params[7]    — SRV t9 (point lights)        ← separate table
    //   params[8]    — SRV t10 (cluster counts)     ← separate table
    //   params[9]    — SRV t11 (cluster indices)    ← separate table
    //   s0 = point-clamp, s1 = bilinear-clamp, s2 = trilinear-clamp

    descRanges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 5, 0);  // t0-t4
    descRanges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 5);  // t5
    descRanges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 6);  // t6
    descRanges[3].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 7);  // t7
    descRanges[4].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 8);  // t8
    descRanges[5].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 9);  // t9
    descRanges[6].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 10); // t10
    descRanges[7].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 11); // t11

    rootParams[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL);
    rootParams[1].InitAsConstantBufferView(1, 0, D3D12_SHADER_VISIBILITY_ALL);
    rootParams[2].InitAsDescriptorTable(1, &descRanges[0], D3D12_SHADER_VISIBILITY_PIXEL);
    rootParams[3].InitAsDescriptorTable(1, &descRanges[1], D3D12_SHADER_VISIBILITY_PIXEL);
    rootParams[4].InitAsDescriptorTable(1, &descRanges[2], D3D12_SHADER_VISIBILITY_PIXEL);
    rootParams[5].InitAsDescriptorTable(1, &descRanges[3], D3D12_SHADER_VISIBILITY_PIXEL);
    rootParams[6].InitAsDescriptorTable(1, &descRanges[4], D3D12_SHADER_VISIBILITY_PIXEL);
    rootParams[7].InitAsDescriptorTable(1, &descRanges[5], D3D12_SHADER_VISIBILITY_PIXEL);
    rootParams[8].InitAsDescriptorTable(1, &descRanges[6], D3D12_SHADER_VISIBILITY_PIXEL);
    rootParams[9].InitAsDescriptorTable(1, &descRanges[7], D3D12_SHADER_VISIBILITY_PIXEL);

    // ... samplers setup ...

    rootSigDesc.Init(10, rootParams, 3, staticSamplers, D3D12_ROOT_SIGNATURE_FLAG_NONE);
}
```

---

## Root Signature Size Calculation

D3D12 limit: 64 DWORDs max.

| Parameter | Type | Size (DWORDs) |
|-----------|------|---------------|
| params[0] | CBV | 2 |
| params[1] | CBV | 2 |
| params[2-9] | Descriptor Table × 8 | 8 |
| **Total** | | **12** |

Static samplers don't count toward the limit. 12 DWORDs is well under 64.

---

## Verification

- Build: `EXIT:0` (success)
- Runtime: IBL pipeline creates successfully, no access violation
- Clustered lighting bindings work correctly with separate descriptor tables

---

## Lessons Learned

1. **D3DX12 helpers store pointers, not copies** — `rootSigDesc.Init()` does NOT copy the arrays; it stores pointers. Arrays must remain in scope until serialization.

2. **Zero-initialize CD3DX12 types** — `CD3DX12_ROOT_PARAMETER` and `CD3DX12_DESCRIPTOR_RANGE` have `= default` constructors that leave memory uninitialized. Use `= {}` for safety.

3. **Descriptor tables require contiguous heap layout** — If SRVs are interleaved with UAVs in the heap, use separate descriptor tables instead of one combined range.

4. **Validate root param indices** — `SetGraphicsRootDescriptorTable(N, ...)` will crash if `N >= NumParameters` in the root signature.

---

## Files Modified

| File | Change |
|------|--------|
| `LunaEngine/.../DX12Pipeline.cpp` | Moved arrays to function scope; expanded DeferredLightingIBL to 10 params |

