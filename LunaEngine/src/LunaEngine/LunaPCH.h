#pragma once

// STL
#include <windows.h>
#include <tchar.h>
#include <memory>
#include <string>
#include <vector>
#include <array>
#include <list>
#include <map>
#include <iostream>
#include <cstdint>
#include <functional>
using namespace std;

#include <vulkan/vulkan.h>

// DX12
#include <directx/d3d12.h>
#include <directx/d3d12sdklayers.h>
#include <directx/d3dx12_core.h>
#include <directx/d3dx12_barriers.h>
#include <directx/d3dcommon.h>

// ImGUI
#include <imgui.h>
// #include "backends/imgui_impl_vulkan.cpp"
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_dx12.h>

#include <GLFW/glfw3.h>
#include <glm/glm.hpp>

// Math
#include <DirectXMath.h>
#include <DirectXPackedVector.h>
#include <DirectXColors.h>

// Window
#include <wrl.h>
#include <windows.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>

using namespace DirectX;
using namespace DirectX::PackedVector;
using namespace Microsoft::WRL;
using Microsoft::WRL::ComPtr;
using std::vector;

// All Type Def
using int8		= __int8;
using int16		= __int16;
using int32		= __int32;
using int64		= __int64;
using uint8		= unsigned __int8;
using uint16	= unsigned __int16;
using uint32	= unsigned __int32;
using uint64	= unsigned __int64;
using Vec2		= XMFLOAT2;
using Vec3		= XMFLOAT3;
using Vec4		= XMFLOAT4;
using Matrix	= XMMATRIX;


enum
{
	SWAP_CHAIN_BUFFER_COUNT = 2
};

struct WindowInfo
{
	HWND	hwnd;       // Output Window
	int32	width;      // Width
	int32	height;     // Height
	bool	windowed;   //
};