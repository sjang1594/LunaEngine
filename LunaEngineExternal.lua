-- LunaExternal.lua

VULKAN_SDK = os.getenv("VULKAN_SDK")
LUNA_ENABLE_VULKAN = (VULKAN_SDK ~= nil and VULKAN_SDK ~= "")
if not LUNA_ENABLE_VULKAN then
   print("[LunaEngine] VULKAN_SDK not set — Vulkan backend disabled. Install from https://vulkan.lunarg.com/sdk/home#windows")
end

IncludeDir = {}
IncludeDir["glm"] = "../vendor/glm"
if LUNA_ENABLE_VULKAN then
   IncludeDir["VulkanSDK"] = VULKAN_SDK .. "/Include"
end

LibraryDir = {}
if LUNA_ENABLE_VULKAN then
   LibraryDir["VulkanSDK"] = VULKAN_SDK .. "/Lib"
end

Library = {}
if LUNA_ENABLE_VULKAN then
   Library["Vulkan"] = LibraryDir["VulkanSDK"] .. "/vulkan-1.lib"
end

group "Dependencies"
   include "vendor/imgui"
   include "vendor/glfw"
group ""

group "Core"
include "LunaEngine"
group ""
