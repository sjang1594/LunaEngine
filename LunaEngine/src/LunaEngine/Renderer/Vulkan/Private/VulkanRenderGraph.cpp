// VulkanRenderGraph.cpp — Phase 18C
// Lightweight barrier-scheduling render graph for the Vulkan backend.
// Mirrors the DX12 RenderGraph (Renderer/RenderGraph.cpp) API.

#include "LunaPCH.h"

#ifdef LUNA_ENABLE_VULKAN

#include "LunaEngine/Renderer/Vulkan/Public/VulkanRenderGraph.h"
#include "LunaEngine/Renderer/Vulkan/Public/VulkanGPUProfiler.h"
#include "Logger/Logger.h"

namespace Luna
{

// ---------------------------------------------------------------------------
// ImportImage
// ---------------------------------------------------------------------------
VKRGHandle VulkanRenderGraph::ImportImage(VkImage              image,
                                           VkImageLayout        currentLayout,
                                           VkPipelineStageFlags currentStage,
                                           VkAccessFlags        currentAccess,
                                           VkImageAspectFlags   aspect)
{
    VKRGHandle h = static_cast<VKRGHandle>(_images.size());
    ImportedImage ii{};
    ii.image          = image;
    ii.aspect         = aspect;
    ii.current        = { currentLayout, currentStage, currentAccess };
    ii.initial        = ii.current;
    _images.push_back(ii);
    return h;
}

// ---------------------------------------------------------------------------
// AddPass
// ---------------------------------------------------------------------------
VulkanRenderGraph::PassBuilder& VulkanRenderGraph::AddPass(const char* name)
{
    _passes.emplace_back(this, name);
    return _passes.back();
}

// ---------------------------------------------------------------------------
// PassBuilder::Read / Write
// ---------------------------------------------------------------------------
VulkanRenderGraph::PassBuilder& VulkanRenderGraph::PassBuilder::Read(
    VKRGHandle handle, VkImageLayout layout, VkPipelineStageFlags stage, VkAccessFlags access)
{
    _reads.push_back({ handle, layout, stage, access });
    return *this;
}

VulkanRenderGraph::PassBuilder& VulkanRenderGraph::PassBuilder::Write(
    VKRGHandle handle, VkImageLayout layout, VkPipelineStageFlags stage, VkAccessFlags access)
{
    _writes.push_back({ handle, layout, stage, access });
    return *this;
}

// ---------------------------------------------------------------------------
// Compile: mark alive passes, determine barrier schedule.
// For simplicity, every pass with SideEffect() is kept; others are culled only
// if they have no writes that feed a later SideEffect pass (conservative).
// In practice all graphics passes use SideEffect(), so no DAG cull is needed
// for correctness — this is equivalent to the DX12 RenderGraph behaviour.
// ---------------------------------------------------------------------------
void VulkanRenderGraph::Compile()
{
    // Propagate liveness backwards from side-effect passes.
    // Mark live passes by reference count (write → later read counts as one consumer).
    std::vector<bool> live(_passes.size(), false);
    for (size_t i = 0; i < _passes.size(); ++i)
        if (_passes[i]._sideEffect) live[i] = true;

    // Backward flood-fill: if pass j reads handle h and pass i (i < j) writes h and is live,
    // mark i live as well.
    for (int j = (int)_passes.size() - 1; j >= 0; --j)
    {
        if (!live[j]) continue;
        for (auto& rd : _passes[j]._reads)
        {
            for (int i = j - 1; i >= 0; --i)
            {
                for (auto& wr : _passes[i]._writes)
                {
                    if (wr.handle == rd.handle) { live[i] = true; break; }
                }
            }
        }
    }

    // Remove dead passes (preserve order of live passes)
    std::deque<PassBuilder> kept;
    for (size_t i = 0; i < _passes.size(); ++i)
    {
        if (live[i]) kept.push_back(std::move(_passes[i]));
        else         LUNA_LOG_INFO("VKRenderGraph: culled dead pass '%s'", _passes[i]._name.c_str());
    }
    _passes = std::move(kept);
}

// ---------------------------------------------------------------------------
// Execute: emit barriers + call execute callbacks
// ---------------------------------------------------------------------------
void VulkanRenderGraph::EmitBarrier(VkCommandBuffer cmd, ImportedImage& img,
                                     VkImageLayout        newLayout,
                                     VkPipelineStageFlags newStage,
                                     VkAccessFlags        newAccess)
{
    if (img.current.layout == newLayout &&
        img.current.stage  == newStage  &&
        img.current.access == newAccess)
        return; // already in target state

    VkImageMemoryBarrier b{};
    b.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask                   = img.current.access;
    b.dstAccessMask                   = newAccess;
    b.oldLayout                       = img.current.layout;
    b.newLayout                       = newLayout;
    b.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    b.image                           = img.image;
    b.subresourceRange.aspectMask     = img.aspect;
    b.subresourceRange.baseMipLevel   = 0;
    b.subresourceRange.levelCount     = VK_REMAINING_MIP_LEVELS;
    b.subresourceRange.baseArrayLayer = 0;
    b.subresourceRange.layerCount     = VK_REMAINING_ARRAY_LAYERS;

    vkCmdPipelineBarrier(cmd,
        img.current.stage, newStage,
        0, 0, nullptr, 0, nullptr, 1, &b);

    img.current = { newLayout, newStage, newAccess };
}

void VulkanRenderGraph::Execute(VkCommandBuffer cmd, VulkanGPUProfiler* profiler)
{
    for (auto& pass : _passes)
    {
        // Emit barriers for all reads
        for (auto& rd : pass._reads)
        {
            if (rd.handle == VKRG_NULL_HANDLE || rd.handle >= _images.size()) continue;
            EmitBarrier(cmd, _images[rd.handle], rd.layout, rd.stage, rd.access);
        }
        // Emit barriers for all writes
        for (auto& wr : pass._writes)
        {
            if (wr.handle == VKRG_NULL_HANDLE || wr.handle >= _images.size()) continue;
            EmitBarrier(cmd, _images[wr.handle], wr.layout, wr.stage, wr.access);
        }
        // Auto-insert profiler timestamps around the pass
        if (profiler && profiler->IsEnabled())
            profiler->WriteBeginTimestamp(cmd, pass._name.c_str());
        // Execute the pass
        if (pass._executeFn) pass._executeFn(cmd);
        if (profiler && profiler->IsEnabled())
            profiler->WriteEndTimestamp(cmd);
    }
}

// ---------------------------------------------------------------------------
// Reset: clear passes and restore image states to initial
// ---------------------------------------------------------------------------
void VulkanRenderGraph::Reset()
{
    _passes.clear();
    for (auto& img : _images)
        img.current = img.initial;
    _images.clear();
}

} // namespace Luna

#endif // LUNA_ENABLE_VULKAN
