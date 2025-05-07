project "LunaApp"
   kind "ConsoleApp"
   language "C++"
   cppdialect "C++17"
   targetdir "bin/%{cfg.buildcfg}"
   staticruntime "off"

   files { "src/**.h", "src/**.cpp" }

   includedirs
   {
      "../vendor/imgui",
      "../vendor/glfw/include",
      "../vendor/imgui/backends",
      "../vendor/dxheaders/include",            
      "../vendor/dxheaders/include/directx",    
      "../LunaEngine/src",
      "%{IncludeDir.VulkanSDK}",
      "%{IncludeDir.glm}",
   }

   libdirs
   {
      "../bin/%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}/LunaEngine"
   }
   
   links
   {
      "LunaEngine"
   }

   targetdir ("bin/" .. outputdir .. "/%{prj.name}")
   objdir ("bin-int/" .. outputdir .. "/%{prj.name}")

   filter "system:windows"
      systemversion "latest"
      defines { "WL_PLATFORM_WINDOWS" }

   filter "system:macosx"
      systemversion "latest"
      defines { "WL_PLATFORM_MACOS" }

      files {
         "src/Platform/MacOS/**.mm",
         "src/Platform/MacOS/**.h"
      }

      links { "Cocoa", "Metal", "QuartzCore" }

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
      kind "WindowedApp"
      defines { "WL_DIST" }
      runtime "Release"
      optimize "On"
      symbols "Off"