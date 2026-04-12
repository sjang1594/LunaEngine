-- LunaExternal.lua

-- Prefer the environment variable; fall back to the known local install path.
VULKAN_SDK = os.getenv("VULKAN_SDK") or "C:/VulkanSDK/1.4.328.1"
VULKAN_ENABLED = (os.isdir(VULKAN_SDK))

if VULKAN_ENABLED then
   print("Vulkan SDK found: " .. VULKAN_SDK)
else
   print("NOTE: Vulkan SDK not found — Vulkan backend will be excluded from the build.")
   print("      Set VULKAN_SDK or install to the default path.")
end

IncludeDir = {}
IncludeDir["VulkanSDK"] = VULKAN_ENABLED and ("%{VULKAN_SDK}/Include") or ""
IncludeDir["glm"] = "../vendor/glm"

LibraryDir = {}
LibraryDir["VulkanSDK"] = VULKAN_ENABLED and ("%{VULKAN_SDK}/Lib") or ""

Library = {}
Library["Vulkan"] = VULKAN_ENABLED and ("%{LibraryDir.VulkanSDK}/vulkan-1.lib") or ""

group "Dependencies"
   include "vendor/imgui"
   include "vendor/glfw"
group ""

group "Core"
include "LunaEngine"
group ""