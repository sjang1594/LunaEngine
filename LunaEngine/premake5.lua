project "LunaEngine"
   kind "StaticLib"
   language "C++"
   cppdialect "C++17"
   targetdir "bin/%{cfg.buildcfg}"
   staticruntime "off"

   files { 
      "src/**.h", 
      "src/**.cpp",
      "Renderer/**.h",
      "Renderer/**.cpp"
   }

   includedirs
   {
      "src",

      "../vendor/glfw/include",
      "../vendor/glm",
      "../vendor/imgui",
      "../vendor/imgui/backends",
      "../vendor/stb_image",

      -- Vulkan
      "../vendor/volk",
      "%{IncludeDir.VulkanSDK}",

      -- DirectX
      "../vendor/dxheaders/include",
      "../vendor/d3d12ma/include",
      "../vendor/dxc/include"
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
      "VOLK_IMPLEMENTATION"
   }

   targetdir ("bin/" .. outputdir .. "/%{prj.name}")
   objdir ("bin-int/" .. outputdir .. "/%{prj.name}")

   filter "system:windows"
      systemversion "latest"
      defines { "WL_PLATFORM_WINDOWS" }

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