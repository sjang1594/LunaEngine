project "LunaEGUI"
    kind "StaticLib"
    language "C++"
    cppdialect "C++17"
    targetder "bin/%{cfg.buildcfg}"
    staticruntime "off"

    files{"src/**.h", "src/**.cpp"}

    includedirs{
        "src", 
        "../vendor/imgui", 
        "../vendor/glfw/include", 
        "../vendor/stb_image",
        
        "%{IncludeDir.VulkanSDK}"
        "%{IncludeDir.glm}",
    }

    links
    {
        "ImGUI",
        "GLFW",

        "%{Library.VulkanSDK}",
    }

    targetdir("bin/" .. outputdir .. "/%{prj.name}")
    objdir("../bin-int/" .. outputdir .. "/%{prj.name}")

    filter "system:windows"
        systemversion "latest"

        defines
        {
            "LUNA_PLATFORM_WINDOWS",
            "LUNA_BUILD_DLL",
            "GLFW_INCLUDE_NONE",
        }