#pragma once

// _HAS_STD_BYTE must be defined before any STL / Windows SDK header that references std::byte
#define _HAS_STD_BYTE 0

// STL
#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <tchar.h>
#include <unordered_map>
#include <vector>
#include <windows.h>
#include <filesystem>

#include <vulkan/vulkan.h>

// DX12
#include <directx/d3d12.h>
#include <directx/d3d12sdklayers.h>
#include <directx/d3dcommon.h>
#include <directx/d3dx12_barriers.h>
#include <directx/d3dx12_core.h>
#include <directx/d3dx12_root_signature.h>

// ImGUI
#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>
#include <imgui.h>

#include <GLFW/glfw3.h>
#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#endif
#include <GLFW/glfw3native.h>
#include <glm/glm.hpp>

// Math
#include <DirectXColors.h>
#include <DirectXMath.h>
#include <DirectXPackedVector.h>

// Window
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl.h>

// DirectX math types are used pervasively across all rendering code; bringing
// them in at PCH level avoids thousands of DirectX:: qualifications.
// Microsoft::WRL is intentionally NOT brought in wholesale — use ComPtr<> explicitly.
using namespace DirectX;
using namespace DirectX::PackedVector;
using Microsoft::WRL::ComPtr;

// Explicit std:: aliases — avoids "using namespace std" polluting all TUs
// while still allowing unqualified use of the most common standard types.
using std::array;
using std::enable_shared_from_this;
using std::forward;
using std::function;
using std::list;
using std::make_shared;
using std::make_unique;
using std::map;
using std::move;
using std::pair;
using std::runtime_error;
using std::shared_ptr;
using std::static_pointer_cast;
using std::string;
using std::unique_ptr;
using std::unordered_map;
using std::vector;
using std::weak_ptr;
using std::wstring;

// All Type Def
using int8 = __int8;
using int16 = __int16;
using int32 = __int32;
using int64 = __int64;
using uint8 = unsigned __int8;
using uint16 = unsigned __int16;
using uint32 = unsigned __int32;
using uint64 = unsigned __int64;
using Vec2 = XMFLOAT2;
using Vec3 = XMFLOAT3;
using Vec4 = XMFLOAT4;
using Matrix = XMMATRIX;
using Matrix4x4 = XMFLOAT4X4;

enum
{
    SWAP_CHAIN_BUFFER_COUNT = 2
};

#include <comdef.h>
inline void ThrowIfDXFailed(HRESULT hr, const char* functionName, int lineNumber)
{
    if (FAILED(hr))
    {
        _com_error err(hr);
        std::cerr << "[ERROR] DirectX Error: " << err.ErrorMessage()
                  << " | Function: " << functionName
                  << " | Line: " << lineNumber << std::endl;
        std::terminate();  // Immediate termination with detailed log
    }
}

#define DX_CHECK(x) ThrowIfDXFailed(x, __FUNCTION__, __LINE__)

inline void CheckIfVKFailed(VkResult error)
{
    if (error == 0)
        return;
    fprintf(stderr, "Vulkan Error: %d\n", error);
    if (error < 0)
        abort();
}