-- LunaExternal.lua

VULKAN_SDK = os.getenv("VULKAN_SDK")
if not VULKAN_SDK then 
   error("VULKAN_SDK environment variable not set! Please Make sure you Download SDK: https://vulkan.lunarg.com/sdk/home#windows")
end 

IncludeDir = {}
IncludeDir["VulkanSDK"] = "%{VULKAN_SDK}/Include"
IncludeDir["glm"] = "../vendor/glm"

LibraryDir = {}
LibraryDir["VulkanSDK"] = "%{VULKAN_SDK}/Lib"

Library = {}
Library["Vulkan"] = "%{LibraryDir.VulkanSDK}/vulkan-1.lib"

group "Dependencies"
   include "vendor/imgui"
   include "vendor/glfw"
group ""

group "Core"
include "LunaEngine"
group ""