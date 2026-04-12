project "LunaEngine"
   kind "StaticLib"
   language "C++"
   cppdialect "C++17"
   targetdir "bin/%{cfg.buildcfg}"
   staticruntime "off"

   targetdir ("bin/" .. outputdir .. "/%{prj.name}")
   objdir ("bin-int/" .. outputdir .. "/%{prj.name}")

   pchheader "LunaPCH.h"
   pchsource "src/LunaEngine/LunaPCH.cpp"

   local shader_dir = path.getabsolute("../LunaEngine/src/LunaEngine/Shaders"):gsub("\\", "/")
   defines {
      'SHADER_ROOT_PATH="' .. shader_dir .. '"'
   }

   files {
      "src/**.h",
      "src/**.cpp",
      "src/LunaEngine/Shaders/*.hlsl",
      "../vendor/imgui/backends/imgui_impl_dx12.cpp",
      "../vendor/imgui/backends/imgui_impl_glfw.cpp",
      -- D3D12MA implementation (compiled as a non-PCH translation unit)
      "../vendor/d3d12ma/src/D3D12MemAlloc.cpp",
   }

   -- Vulkan backend is a work-in-progress — exclude its source tree from the build.
   -- When ready, add LUNA_VULKAN_ENABLED to defines and remove these exclusions.
   removefiles {
      "src/LunaEngine/Renderer/Vulkan/**.cpp",
      "src/LunaEngine/Renderer/Vulkan/**.h",
   }

   includedirs
   {
      -- DirectX
      "../vendor/dxheaders/include",
      "../vendor/dxheaders/include/directxs",
      "../vendor/d3d12ma/include",
      "../vendor/dxc/include",
      "../vendor/DirectXTex/DirectXTex",
      "../vendor/DirectXTex/DirectXTex/BC",
      "../vendor/DirectXTex/DirectXTex/Filters",

      -- glTF loader (single-header, implementation compiled in MeshLoader.cpp)
      "../vendor/cgltf",

      -- Vulkan SDK headers (only when SDK is present)
      (VULKAN_ENABLED and "%{IncludeDir.VulkanSDK}" or nil),

      "../vendor/glfw/include",
      "../vendor/glm",
      "../vendor/imgui",
      "../vendor/imgui/backends",
      "../vendor/stb_image",

      "src",
      "src/LunaEngine"
   }

   libdirs
   {
      "../vendor/dxc/lib/x64"
   }

   -- DirectXTex is sourced from the vcpkg installation (no vendor build included).
   -- vcpkg's wildcard lib injection is disabled via Directory.Build.props, so we reference
   -- the appropriate config-specific path explicitly here.
   filter "configurations:Debug"
      libdirs { "C:/Users/skcjf/vcpkg/installed/x64-windows/debug/lib" }
   filter "configurations:Release or configurations:Dist"
      libdirs { "C:/Users/skcjf/vcpkg/installed/x64-windows/lib" }
   filter {}

   links
   {
      "GLFW",
      "ImGui",
      "d3d12",
      "dxgi",
      "dxguid",
      "dxcompiler",      -- DXC for HLSL SM 6.x compiling
      "DirectXTex",
      (VULKAN_ENABLED and "%{Library.Vulkan}" or nil),
   }

   defines
   {
      "D3D12_ENABLE_DEBUG_LAYER",
      "_GLFW_WIN32"
   }

   -- Vendor files: disable PCH (they have their own includes and don't need LunaPCH.h)
   filter { "files:../vendor/d3d12ma/src/D3D12MemAlloc.cpp" }
      flags { "NoPCH" }
   filter { "files:../vendor/imgui/backends/imgui_impl_dx12.cpp" }
      flags { "NoPCH" }
   filter { "files:../vendor/imgui/backends/imgui_impl_glfw.cpp" }
      flags { "NoPCH" }
   -- imgui_impl_vulkan.cpp excluded from build (Vulkan not ready)
   filter {}

   filter { "files:**.hlsl" }
      buildaction "None"

   filter "system:windows"
      systemversion "latest"
      defines { "WL_PLATFORM_WINDOWS" }

      includedirs {
         "../vendor/dxheaders/include",
         "../vendor/dxheaders/include/directxs",
         "../vendor/d3d12ma/include",
         "../vendor/dxc/include",
         "../vendor/DirectXTex/DirectXTex",
         "../vendor/DirectXTex/DirectXTex/BC",
         "../vendor/DirectXTex/DirectXTex/Filters",
         "../vendor/cgltf",
      }

      libdirs {
         "../vendor/dxc/lib/x64"
      }

      links {
         "d3d12", "dxgi", "dxguid", "dxcompiler",
         "GLFW", "ImGui", "DirectXTex",
         (VULKAN_ENABLED and "%{Library.Vulkan}" or nil),
      }

   filter "system:macosx"
      systemversion "latest"
      defines { "WL_PLATFORM_MACOS" }

      files {
         "src/LunaEngine/Platform/MacOS/**.mm",
         "src/LunaEngine/Platform/MacOS/**.h"
      }

      links { "Metal", "Cocoa", "QuartzCore" }

   filter "configurations:Debug"
      defines { "WL_DEBUG" }
      runtime "Debug"
      symbols "On"

   filter "configurations:Release"
      defines { "WL_RELEASE" }
      runtime "Release"
      optimize "On"
      symbols "On"

   filter "configurations:Dist"
      defines { "WL_DIST" }
      runtime "Release"
      optimize "On"
      symbols "Off"
