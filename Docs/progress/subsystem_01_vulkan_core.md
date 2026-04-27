# VulkanCore — Implementation Document

**Date:** 2026-04-25  
**Status:** ✅ Complete  
**Lines:** Header 100, Impl 484  

---

## Overview

VulkanCore는 VulkanBackend의 god-object를 분해하는 첫 번째 subsystem이다. Vulkan infrastructure 최하위 레이어로, 다른 모든 subsystem이 의존한다.

---

## Responsibilities

| Responsibility | API |
|----------------|-----|
| Instance lifecycle | `CreateInstance()`, `Shutdown()` |
| Surface creation | `CreateSurface(windowHandle)` |
| Debug messenger | `SetupDebugMessenger()` |
| Device ownership | `GetDevice()`, `GetVulkanDevice()` |
| Queue access | `GetGraphicsQueue()`, `GetComputeQueue()` |
| Single-time commands | `BeginSingleTimeCommands()`, `EndSingleTimeCommands()` |
| Memory helpers | `CreateBuffer()`, `CreateImage()`, `CreateImageView()` |
| Device lost tracking | `IsDeviceLost()`, `SetDeviceLost()` |

---

## Design Decisions

### 1. Transfer Command Pool Isolation

**Problem**: VulkanBackend의 `BeginSingleTimeCommands()`가 `_frames[0].cmdPool`을 사용하면, async resource loading 중 frame command pool과 race condition 발생.

**Solution**: Dedicated `_transferCmdPool` 생성 (`VK_COMMAND_POOL_CREATE_TRANSIENT_BIT`).

```cpp
// 각 single-time command는 dedicated pool에서 할당
ai.commandPool = _transferCmdPool;  // NOT _frames[0].cmdPool
```

### 2. Device Lost State Machine

**Problem**: `VK_ERROR_DEVICE_LOST` 후 연속적인 Vulkan 호출은 추가 에러를 cascade.

**Solution**: `_deviceLost` flag로 early-exit.

```cpp
VkCommandBuffer VulkanCore::BeginSingleTimeCommands() {
    if (_deviceLost) return VK_NULL_HANDLE;  // bail out immediately
    // ...
}
```

### 3. Buffer Device Address Support

**Problem**: RT (ray tracing)에서 BLAS/TLAS buffer는 `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT` + `VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT` 필요.

**Solution**: `CreateBuffer()`에서 usage flag 검사 후 자동 적용.

```cpp
if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)
    allocInfo.pNext = &flagsInfo;  // VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT
```

---

## Memory Layout

```
VulkanCore (168 bytes estimated)
├── VkInstance               _instance          (8B pointer)
├── VkSurfaceKHR             _surface           (8B handle)
├── VkDebugUtilsMessengerEXT _debugMessenger    (8B handle)
├── unique_ptr<VulkanDevice> _device            (8B pointer)
├── VkCommandPool            _transferCmdPool   (8B handle)
└── bool                     _deviceLost        (1B + padding)
```

---

## Thread Safety

| Method | Thread Safety |
|--------|--------------|
| `GetDevice()`, `GetQueue()` | ✅ Thread-safe (read-only after init) |
| `BeginSingleTimeCommands()` | ⚠️ NOT thread-safe (single pool) |
| `CreateBuffer()`, `CreateImage()` | ⚠️ NOT thread-safe (single pool for copies) |

**Recommendation**: 다중 스레드 resource upload 필요시  `_transferCmdPool`을 per-thread pool로 확장하거나, staging ring buffer 사용.

---

## Validation Layers

Debug build에서만 활성화:
- `VK_LAYER_KHRONOS_validation`
- Severity filter: `WARNING | ERROR`
- Type filter: `VALIDATION | PERFORMANCE`

---

## API Version

`VK_API_VERSION_1_3` 사용:
- Vulkan 1.3 기능 (dynamic rendering 등) 지원
- RT extensions (`VK_KHR_ray_tracing_pipeline`) 호환

---

## Integration Path

### Before (VulkanBackend monolith)
```cpp
class VulkanBackend {
    VkInstance _instance;
    VkSurfaceKHR _surface;
    std::unique_ptr<VulkanDevice> _device;
    VkCommandPool _transferCmdPool;
    // ... 100+ more members
};
```

### After (VulkanCore extracted)
```cpp
class VulkanBackend {
    std::unique_ptr<VulkanCore> _core;  // delegates to VulkanCore
    // ... remaining rendering-specific members
};
```

### Migration API
```cpp
// Before
_device->GetDevice()
CreateBuffer(...)
BeginSingleTimeCommands()

// After
_core->GetDevice()
_core->CreateBuffer(...)
_core->BeginSingleTimeCommands()
```

---

## Files

| File | Path |
|------|------|
| Header | `LunaEngine/src/LunaEngine/Renderer/Vulkan/Public/VulkanCore.h` |
| Implementation | `LunaEngine/src/LunaEngine/Renderer/Vulkan/Private/VulkanCore.cpp` |

---

## Build Verification

```
Build succeeded.
0 Warning(s)
0 Error(s)
```

---

## Next Steps

1. **VulkanSwapchain** extraction
2. **VulkanBackend integration** — replace direct members with `_core->` calls

