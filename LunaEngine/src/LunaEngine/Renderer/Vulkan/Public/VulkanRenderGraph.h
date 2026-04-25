#pragma once
// VulkanRenderGraph.h — Phase 18C: Vulkan render graph with automatic barrier scheduling.
// Mirrors the DX12 RenderGraph API (Renderer/RenderGraph.h) but emits vkCmdPipelineBarrier2
// for VkImage state transitions rather than D3D12_RESOURCE_BARRIER.
//
// Usage:
//   _renderGraph.Reset();
//   auto hdr = _renderGraph.ImportImage(_hdrImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
//                                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
//                                        VK_ACCESS_SHADER_READ_BIT);
//   _renderGraph.AddPass("MyPass").Read(hdr).SideEffect().Execute([](VkCommandBuffer cmd){ ... });
//   _renderGraph.Compile();
//   _renderGraph.Execute(cmd);

#ifdef LUNA_ENABLE_VULKAN

#include "vulkan/vulkan.h"
#include <cstdint>
#include <functional>
#include <string>
#include <deque>
#include <vector>

namespace Luna
{

using VKRGHandle = uint32_t;
static constexpr VKRGHandle VKRG_NULL_HANDLE = UINT32_MAX;

// ---------------------------------------------------------------------------
// VulkanRenderGraph
// ---------------------------------------------------------------------------
class VulkanRenderGraph
{
public:
    struct ImageState
    {
        VkImageLayout        layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkPipelineStageFlags stage  = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags        access = 0;
    };

    struct ImportedImage
    {
        VkImage    image;
        VkImageAspectFlags aspect;
        ImageState current;    // tracked state (mutated as passes run)
        ImageState initial;    // state at import time (for reset)
    };

    class PassBuilder
    {
    public:
        PassBuilder& Read(VKRGHandle handle,
                          VkImageLayout       dstLayout,
                          VkPipelineStageFlags dstStage,
                          VkAccessFlags        dstAccess);
        PassBuilder& Write(VKRGHandle handle,
                           VkImageLayout        dstLayout,
                           VkPipelineStageFlags dstStage,
                           VkAccessFlags        dstAccess);
        PassBuilder& SideEffect() { _sideEffect = true; return *this; }
        PassBuilder& Execute(std::function<void(VkCommandBuffer)> fn)
        { _executeFn = std::move(fn); return *this; }

        PassBuilder(VulkanRenderGraph* rg, const char* name)
            : _rg(rg), _name(name) {}

    private:
        friend class VulkanRenderGraph;

        VulkanRenderGraph*                       _rg;
        std::string                              _name;
        bool                                     _sideEffect = false;
        std::function<void(VkCommandBuffer)>     _executeFn;

        struct Dep { VKRGHandle handle; VkImageLayout layout;
                     VkPipelineStageFlags stage; VkAccessFlags access; };
        std::vector<Dep> _reads;
        std::vector<Dep> _writes;
    };

    // Import a persistent image into the graph.
    // aspect: VK_IMAGE_ASPECT_COLOR_BIT or DEPTH_BIT.
    VKRGHandle ImportImage(VkImage image,
                           VkImageLayout        currentLayout,
                           VkPipelineStageFlags currentStage,
                           VkAccessFlags        currentAccess,
                           VkImageAspectFlags   aspect = VK_IMAGE_ASPECT_COLOR_BIT);

    // Add a render pass; returns a PassBuilder to chain Read/Write/Execute calls.
    // Uses std::deque internally — references remain valid across subsequent AddPass() calls.
    PassBuilder& AddPass(const char* name);

    // Cull dead passes (no SideEffect and no live consumer), compute barrier schedule.
    void Compile();

    // Emit barriers and invoke execute callbacks in compiled order.
    // If profiler is non-null and enabled, auto-inserts timestamps around each pass.
    void Execute(VkCommandBuffer cmd, class VulkanGPUProfiler* profiler = nullptr);

    // Discard all passes and reset image states to their imported values.
    void Reset();

private:
    void EmitBarrier(VkCommandBuffer cmd,
                     ImportedImage&  img,
                     VkImageLayout        newLayout,
                     VkPipelineStageFlags newStage,
                     VkAccessFlags        newAccess);

    std::vector<ImportedImage> _images;
    std::deque<PassBuilder>    _passes;  // deque: references stable across push_back
};

} // namespace Luna

#endif // LUNA_ENABLE_VULKAN
