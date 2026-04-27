# Phase 26 — Vulkan Render Graph: Transient Resource Aliasing

**Date:** 2026-04-27  
**Backend:** Vulkan  
**Status:** ✅ Complete (build verified)

---

## Overview

Ports DX12 Phase 14's placed-resource aliasing to the Vulkan render graph. Graph-owned transient VkImages share the same VkDeviceMemory when their pass lifetimes don't overlap. Uses greedy interval-graph colouring (identical algorithm to DX12) for alias slot assignment.

Currently all images remain imported (persistent). The aliasing infrastructure is ready for migration of short-lived intermediates (G-buffer, SSAO, bloom).

---

## Architecture

```
VulkanRenderGraph::Compile()
  │
  ├─ 1. _CullPasses()              ← DAG flood-fill from SideEffect passes
  ├─ 2. _ComputeTransientLifetimes() ← [firstPass, lastPass] per transient
  ├─ 3. _AssignAliasingSlots()      ← greedy interval-graph colouring
  │     ├─ vkCreateImage (temp)     ← query VkMemoryRequirements
  │     ├─ interval colouring       ← reuse slots where lastPass < firstPass
  │     └─ memTypeBits intersection ← ensure compatible memory types
  └─ 4. _CreateTransientResources()
        ├─ vkAllocateMemory         ← one VkDeviceMemory per alias slot
        ├─ vkCreateImage            ← final VkImage per transient
        └─ vkBindImageMemory        ← bind at offset 0

VulkanRenderGraph::Execute(cmd)
  └─ per live pass:
       ├─ aliasing barrier check    ← if slot resident changed:
       │   └─ vkCmdPipelineBarrier  ← oldLayout=UNDEFINED (discard contents)
       ├─ state transition barriers ← normal read/write barriers
       └─ pass._executeFn(cmd)
```

---

## API

```cpp
// Phase 26 constructor — enables transient image support
VulkanRenderGraph rg(device, physicalDevice);

// Import persistent image (Phase 18C — unchanged)
auto hHDR = rg.ImportImage(hdrImage, layout, stage, access);

// Declare transient image (Phase 26 — new)
VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
ci.imageType   = VK_IMAGE_TYPE_2D;
ci.format      = VK_FORMAT_R8G8B8A8_UNORM;
ci.extent      = { width, height, 1 };
ci.mipLevels   = 1;
ci.arrayLayers = 1;
ci.samples     = VK_SAMPLE_COUNT_1_BIT;
ci.tiling      = VK_IMAGE_TILING_OPTIMAL;
ci.usage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
auto hTemp = rg.CreateTransientImage("TempRT", ci, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

rg.AddPass("Write Temp")
  .Write(hTemp, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT)
  .SideEffect()
  .Execute([&](VkCommandBuffer c) { /* render into hTemp */ });

rg.AddPass("Read Temp")
  .Read(hTemp, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT)
  .Write(hHDR, ...)
  .SideEffect()
  .Execute([&](VkCommandBuffer c) { /* sample hTemp */ });

rg.Compile();   // alias analysis + allocate
rg.Execute(cmd); // barriers + lambdas

// After Compile(), retrieve the VkImage:
VkImage tempImg = rg.GetTransientImage(hTemp);
```

---

## Aliasing Barrier Strategy

When a transient image takes over a memory slot from a different resident:

```
VkImageMemoryBarrier {
  srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT
  dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT
  oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED    ← discard previous contents
  newLayout     = <initialLayout from CreateTransientImage>
  image         = <new resident>
}
```

This matches the Vulkan spec §12.4 requirement that aliased memory transitions must use `oldLayout=UNDEFINED` to invalidate stale data.

---

## Files Modified

| File | Type | Description |
|------|------|-------------|
| `Vulkan/Public/VulkanRenderGraph.h` | Modified | Added ImageNode (replaces ImportedImage), transient fields, AliasSlot, CreateTransientImage(), GetTransientImage(), Shutdown(), private helpers |
| `Vulkan/Private/VulkanRenderGraph.cpp` | Modified | Full Phase 26 implementation: _CullPasses(), _ComputeTransientLifetimes(), _AssignAliasingSlots(), _CreateTransientResources(), aliasing barriers in Execute(), Shutdown() |
| `Vulkan/Private/VulkanBackend.cpp` | Modified | CompositeFrame() passes VkDevice + VkPhysicalDevice to VulkanRenderGraph constructor |

---

## Differences from DX12 Phase 14

| Aspect | DX12 | Vulkan |
|--------|------|--------|
| Memory backing | `ID3D12Heap` (ALLOW_ALL_BUFFERS_AND_TEXTURES) | `VkDeviceMemory` (DEVICE_LOCAL) |
| Image creation | `CreatePlacedResource(heap, offset, desc, state)` | `vkCreateImage` + `vkBindImageMemory(mem, 0)` |
| Aliasing barrier | `D3D12_RESOURCE_BARRIER::Aliasing(old, new)` | `VkImageMemoryBarrier` with `oldLayout=UNDEFINED` |
| Memory query | `GetResourceAllocationInfo(desc)` | `vkGetImageMemoryRequirements(tempImage)` |
| Memory type | Implicit (DEFAULT heap) | Explicit `_FindMemoryType(bits, DEVICE_LOCAL)` |

---

## Known Limitations

1. **Per-frame allocation**: If transient images are declared each frame (graph is a local in CompositeFrame), VkImages and VkDeviceMemory are allocated and freed per frame. This is correct but suboptimal. When images are migrated to transients, the graph should be elevated to a VulkanBackend member with cached allocations.

2. **No VkImageView creation**: The graph creates VkImages but not VkImageViews. Passes that need views for descriptor sets must create them externally (or the graph API should be extended).

3. **No images currently transient**: All existing images are imported. Migration of G-buffer, SSAO, and bloom intermediates to transients is a future step.

