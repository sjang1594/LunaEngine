project "LunaEngine"
   kind "StaticLib"
   language "C++"
   cppdialect "C++17"
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
      "src/LunaEngine/Shaders/*.slang",
      "../vendor/imgui/backends/imgui_impl_dx12.cpp",
      "../vendor/imgui/backends/imgui_impl_glfw.cpp",
      -- D3D12MA implementation (compiled as a non-PCH translation unit)
      "../vendor/d3d12ma/src/D3D12MemAlloc.cpp",
   }

   includedirs
   {
      -- DirectX 12 headers (microsoft/DirectX-Headers)
      "../vendor/dxheaders/include",
      "../vendor/dxheaders/include/directx",
      -- D3D12 Memory Allocator
      "../vendor/d3d12ma/include",
      -- DXC compiler (headers in inc/, not include/)
      "../vendor/dxc/inc",
      -- GLFW / ImGui / GLM
      "../vendor/glfw/include",
      "../vendor/imgui",
      "../vendor/imgui/backends",
      "../vendor/glm",
      -- Single-header loaders
      "../vendor/cgltf",
      "../vendor/stb_image",
      -- Project source roots
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
      "dxcompiler",   -- DXC for HLSL SM 6.x
   }

   defines { "D3D12_ENABLE_DEBUG_LAYER" }

   -- Vulkan + Slang support (optional — requires VULKAN_SDK env var)
   -- Slang ships inside the Vulkan SDK ($VULKAN_SDK/Include/slang/, $VULKAN_SDK/Lib/slang.lib)
   if LUNA_ENABLE_VULKAN then
      includedirs { "%{IncludeDir.VulkanSDK}" }
      libdirs     { "%{LibraryDir.VulkanSDK}" }
      links       { "%{Library.Vulkan}", "slang" }
      defines     { "LUNA_ENABLE_VULKAN", "LUNA_ENABLE_SLANG", "_GLFW_WIN32" }
      files       { "../vendor/imgui/backends/imgui_impl_vulkan.cpp" }
   else
      defines     { "_GLFW_WIN32" }
      -- Exclude Vulkan backend source from compile when SDK is absent
      removefiles { "src/LunaEngine/Renderer/Vulkan/**.cpp" }
   end

   -- Vendor files that must not use the project PCH
   filter { "files:../vendor/d3d12ma/src/D3D12MemAlloc.cpp" }
      enablepch "Off"

   filter { "files:../vendor/imgui/backends/imgui_impl_dx12.cpp" }
      enablepch "Off"

   filter { "files:../vendor/imgui/backends/imgui_impl_glfw.cpp" }
      enablepch "Off"

    filter { "files:../vendor/imgui/backends/imgui_impl_vulkan.cpp" }
      enablepch "Off"

   -- DX12Shader.cpp: unused FXC-era stub (DX12Shader != DX12ShaderProgram); exclude from build
   filter { "files:src/LunaEngine/Renderer/DX12/Private/DX12Shader.cpp" }
      buildaction "None"
   -- IShader.cpp: references wrong class name IShader (actual: IShaderProgram); exclude from build
   filter { "files:src/LunaEngine/Graphics/IShader.cpp" }
      buildaction "None"

   filter { "files:**.hlsl" }
      buildaction "None"

   -- Slang files: compiled at runtime via Slang C API, not by MSBuild
   filter { "files:**.slang" }
      buildaction "None"

   filter "system:windows"
      systemversion "latest"
      defines { "WL_PLATFORM_WINDOWS", "GLFW_EXPOSE_NATIVE_WIN32" }

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
