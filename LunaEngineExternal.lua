VULKAN_SDK = os.getenv("VULKAN_SDK")

IncludeDir = {}
IncludeDir["VulkanSDK"] = "%{VULKAN_SDK}/Include"
IncludeDir['glm'] = "../vendor/glm"

Library = {}
Library["VulkanSDK"] = "%{VULKAN_SDK}/Lib/vulkan-1.lib"

group "Dependencies"
   include "vendor/imgui"
   include "vendor/glfw"
group ""

group "Core"
include "LunaEGUI"
include "LunaEAPP"
group ""
