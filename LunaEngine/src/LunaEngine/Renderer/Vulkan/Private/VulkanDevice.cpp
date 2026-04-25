#include "LunaPCH.h"
#include "LunaEngine/Renderer/Vulkan/Public/VulkanDevice.h"
#include "Logger/Logger.h"

namespace Luna
{

VulkanDevice::~VulkanDevice()
{
    ShutDown();
}

bool VulkanDevice::Initialize(VkInstance instance, VkSurfaceKHR surface)
{
    if (!PickPhysicalDevice(instance, surface))
    {
        LUNA_LOG_ERROR("Failed to pick a physical device");
        return false;
    }

    if (!CreateLogicalDevice())
    {
        LUNA_LOG_ERROR("Failed to create logical device");
        return false;
    }

    vkGetDeviceQueue(_device, _graphicsQueueFamily, 0, &_graphicsQueue);
    vkGetDeviceQueue(_device, _presentQueueFamily, 0, &_presentQueue);

    // Phase 20: Get compute queue
    if (_asyncComputeSupported)
    {
        if (_computeQueueFamily == _graphicsQueueFamily)
            vkGetDeviceQueue(_device, _computeQueueFamily, 1, &_computeQueue);  // second queue in same family
        else
            vkGetDeviceQueue(_device, _computeQueueFamily, 0, &_computeQueue);
        LUNA_LOG_INFO("Phase 20: Async compute queue acquired (family %u, %s)",
                      _computeQueueFamily,
                      _computeQueueFamily != _graphicsQueueFamily ? "dedicated" : "shared family");
    }
    else
    {
        LUNA_LOG_INFO("Phase 20: No async compute queue — cull on graphics queue");
    }
    return true;
}

void VulkanDevice::ShutDown()
{
    if (_device)
    {
        vkDestroyDevice(_device, nullptr);
        _device = VK_NULL_HANDLE;
    }
}

bool VulkanDevice::PickPhysicalDevice(VkInstance instance, VkSurfaceKHR surface)
{
    uint32_t _physicalDeviceCount = 0; // # of gpu
    vkEnumeratePhysicalDevices(instance, &_physicalDeviceCount, nullptr);
    if (_physicalDeviceCount == 0)
    {
        LUNA_LOG_ERROR("No Vulkan-Support GPUs with Vulkan support");
        return false;
    }

    std::vector<VkPhysicalDevice> devices(_physicalDeviceCount);
    
    devices.resize(_physicalDeviceCount);
    vkEnumeratePhysicalDevices(instance, &_physicalDeviceCount, devices.data());

    for (const auto& device : devices)
    {
        if (FindQueueFamilies(device, surface))
        {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(device, &props);
            LUNA_LOG_INFO("Found GPU: %s", props.deviceName);
            _physicalDevice = device;
            return true;
        }
    }

    return false;
}

bool VulkanDevice::CreateLogicalDevice()
{
    float queuePriorities[2] = { 1.0f, 1.0f };
    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    VkDeviceQueueCreateInfo queueCreateInfo {};
    queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreateInfo.queueFamilyIndex = _graphicsQueueFamily;

    // Phase 20: If compute queue is from the same family, request 2 queues
    if (_asyncComputeSupported && _computeQueueFamily == _graphicsQueueFamily)
    {
        queueCreateInfo.queueCount = 2;
        queueCreateInfo.pQueuePriorities = queuePriorities;
    }
    else
    {
        queueCreateInfo.queueCount = 1;
        queueCreateInfo.pQueuePriorities = queuePriorities;
    }
    queueCreateInfos.push_back(queueCreateInfo);

    if (_graphicsQueueFamily != _presentQueueFamily)
    {
        VkDeviceQueueCreateInfo presentQueueInfo{};
        presentQueueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        presentQueueInfo.queueFamilyIndex = _presentQueueFamily;
        presentQueueInfo.queueCount = 1;
        presentQueueInfo.pQueuePriorities = queuePriorities;
        queueCreateInfos.push_back(presentQueueInfo);
    }

    // Phase 20: Dedicated compute family (different from graphics and present)
    if (_asyncComputeSupported && _computeQueueFamily != _graphicsQueueFamily
        && _computeQueueFamily != _presentQueueFamily)
    {
        VkDeviceQueueCreateInfo computeQueueInfo{};
        computeQueueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        computeQueueInfo.queueFamilyIndex = _computeQueueFamily;
        computeQueueInfo.queueCount = 1;
        computeQueueInfo.pQueuePriorities = queuePriorities;
        queueCreateInfos.push_back(computeQueueInfo);
    }

    // Phase 18D: probe RT support before enabling extensions
    VkPhysicalDeviceAccelerationStructureFeaturesKHR asFeatures{};
    asFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtFeatures{};
    rtFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
    VkPhysicalDeviceBufferDeviceAddressFeatures bdaFeatures{};
    bdaFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
    VkPhysicalDeviceRayQueryFeaturesKHR rqFeatures{};
    rqFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;

    // Chain all feature structs and query
    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &asFeatures;
    asFeatures.pNext = &rtFeatures;
    rtFeatures.pNext = &bdaFeatures;
    bdaFeatures.pNext = &rqFeatures;
    vkGetPhysicalDeviceFeatures2(_physicalDevice, &features2);

    _rtSupported = asFeatures.accelerationStructure && rtFeatures.rayTracingPipeline
                 && bdaFeatures.bufferDeviceAddress && rqFeatures.rayQuery;
    if (_rtSupported)
        LUNA_LOG_INFO("VK RT: hardware ray tracing supported — enabling RT extensions");
    else
        LUNA_LOG_INFO("VK RT: ray tracing not supported — falling back to CSM shadows");

    // Vulkan 1.2 features — covers descriptor indexing, drawIndirectCount, bufferDeviceAddress.
    // VkPhysicalDeviceVulkan12Features must not coexist with VkPhysicalDeviceDescriptorIndexingFeatures
    // or VkPhysicalDeviceBufferDeviceAddressFeatures (both promoted in 1.2).
    VkPhysicalDeviceVulkan12Features vk12Features{};
    vk12Features.sType                                        = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    vk12Features.drawIndirectCount                            = VK_TRUE;
    vk12Features.descriptorIndexing                           = VK_TRUE;
    vk12Features.runtimeDescriptorArray                       = VK_TRUE;
    vk12Features.descriptorBindingPartiallyBound              = VK_TRUE;
    vk12Features.shaderSampledImageArrayNonUniformIndexing    = VK_TRUE;
    // bufferDeviceAddress enabled conditionally below (requires RT)

    VkPhysicalDeviceFeatures deviceFeatures {};
    deviceFeatures.samplerAnisotropy = VK_TRUE;
    deviceFeatures.multiDrawIndirect = VK_TRUE;

    // Build pNext chain: vk12Features → [asFeatures → rtFeatures → rqFeatures] if RT
    // bdaFeatures removed — bufferDeviceAddress lives in vk12Features now
    void** ppNextTail = &vk12Features.pNext;
    if (_rtSupported)
    {
        vk12Features.bufferDeviceAddress = VK_TRUE;   // promoted into 1.2
        asFeatures.accelerationStructure = VK_TRUE;
        rtFeatures.rayTracingPipeline    = VK_TRUE;
        rqFeatures.rayQuery              = VK_TRUE;
        asFeatures.pNext = &rtFeatures;
        rtFeatures.pNext = &rqFeatures;
        rqFeatures.pNext = nullptr;
        *ppNextTail = &asFeatures;
    }

    VkDeviceCreateInfo createInfo {};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.pNext = &vk12Features;   // head of pNext chain
    createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();
    createInfo.pEnabledFeatures = &deviceFeatures;

    // Build device extension list (conditionally include RT extensions)
    std::vector<const char*> deviceExtensions = {
        "VK_KHR_swapchain",
        "VK_EXT_descriptor_indexing",
    };
    if (_rtSupported)
    {
        deviceExtensions.push_back("VK_KHR_acceleration_structure");
        deviceExtensions.push_back("VK_KHR_ray_tracing_pipeline");
        deviceExtensions.push_back("VK_KHR_deferred_host_operations");
        deviceExtensions.push_back("VK_KHR_buffer_device_address");
        deviceExtensions.push_back("VK_KHR_ray_query");
    }
    createInfo.enabledExtensionCount   = static_cast<uint32_t>(deviceExtensions.size());
    createInfo.ppEnabledExtensionNames = deviceExtensions.data();

    if (vkCreateDevice(_physicalDevice, &createInfo, nullptr, &_device) != VK_SUCCESS)
    {
        if (_rtSupported)
        {
            // Retry without RT extensions in case driver reports support but can't enable them
            LUNA_LOG_WARN("VK: Device creation with RT extensions failed — retrying without RT");
            _rtSupported = false;
            deviceExtensions.resize(2);
            createInfo.enabledExtensionCount   = 2;
            createInfo.pNext = &vk12Features;
            vk12Features.pNext = nullptr;
            if (vkCreateDevice(_physicalDevice, &createInfo, nullptr, &_device) != VK_SUCCESS)
            {
                LUNA_LOG_ERROR("Failed to create logical device");
                return false;
            }
        }
        else
        {
            LUNA_LOG_ERROR("Failed to create logical device");
            return false;
        }
    }
    return true;
}

bool VulkanDevice::FindQueueFamilies(VkPhysicalDevice device, VkSurfaceKHR surface)
{
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);

    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

    bool graphicsFound = false;
    bool presentFound = false;
    _computeQueueFamily = UINT32_MAX;
    _asyncComputeSupported = false;

    for (uint32_t i = 0; i < queueFamilies.size(); i++)
    {
        if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
        {
            _graphicsQueueFamily = i;
            graphicsFound = true;
        }

        VkBool32 presentSupport = false;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &presentSupport);
        if (presentSupport) {
            _presentQueueFamily = i;
            presentFound = true;
        }

        if (graphicsFound && presentFound) break;
    }

    if (!graphicsFound || !presentFound) return false;

    // Phase 20: Find dedicated compute queue family (COMPUTE but NOT GRAPHICS)
    for (uint32_t i = 0; i < queueFamilies.size(); i++)
    {
        if ((queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
            !(queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
        {
            _computeQueueFamily = i;
            _asyncComputeSupported = true;
            break;
        }
    }

    // Fallback: use a second queue from the graphics family
    if (!_asyncComputeSupported && queueFamilies[_graphicsQueueFamily].queueCount > 1)
    {
        _computeQueueFamily = _graphicsQueueFamily;
        _asyncComputeSupported = true;
    }

    return true;
}

}