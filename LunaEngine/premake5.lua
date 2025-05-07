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
      "../vendor/imgui/backends/imgui_impl_glfw.cpp"
   }

   includedirs
   {
      -- DirectX
      "../vendor/dxheaders/include",
      "../vendor/dxheaders/include/directxs",
      "../vendor/d3d12ma/include",
      "../vendor/dxc/include",

      -- Vulkan
      "../vendor/volk",
      "%{IncludeDir.VulkanSDK}",

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

   links
   {
      "GLFW",
      "ImGui",
      "d3d12",
      "dxgi",
      "dxguid",
      "dxcompiler",      -- DXC for HLSL shader compiling
      "%{Library.Vulkan}",
   }

   defines 
   {
      "VOLK_IMPLEMENTATION",
      "VK_NO_PROTOTYPES",
      "D3D12_ENABLE_DEBUG_LAYER",
      "_GLFW_WIN32"
   }

   filter { "files:**.hlsl" }
      buildaction "None"
   
   filter "system:windows"
      systemversion "latest"
      defines { "WL_PLATFORM_WINDOWS" }

      includedirs {
         "../vendor/dxheaders/include",
         "../vendor/dxheaders/include/directxs",
         "../vendor/d3d12ma/include",
         "../vendor/dxc/include"
      }

      libdirs {
         "../vendor/dxc/lib/x64"
      }

      links {
         "d3d12", "dxgi", "dxguid", "dxcompiler",
         "GLFW", "ImGui", "%{Library.Vulkan}"
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