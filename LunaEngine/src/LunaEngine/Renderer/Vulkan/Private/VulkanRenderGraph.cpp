// VulkanRenderGraph.cpp — Phase 18C + Phase 26
// Barrier-scheduling render graph for the Vulkan backend with transient resource aliasing.
//
// Phase 18C: Lightweight barrier scheduling for imported (persistent) images.
// Phase 26:  Transient image support — graph-owned VkImages that share VkDeviceMemory
//            when their pass lifetimes don't overlap (interval-graph colouring).

#include "LunaPCH.h"

#ifdef LUNA_ENABLE_VULKAN

#include "LunaEngine/Renderer/Vulkan/Public/VulkanRenderGraph.h"
#include "LunaEngine/Renderer/Vulkan/Public/VulkanGPUProfiler.h"
#include "Logger/Logger.h"
#include <algorithm>
#include <cassert>

namespace Luna
{

// ---------------------------------------------------------------------------
// Constructors / Destructor
// ---------------------------------------------------------------------------

VulkanRenderGraph::VulkanRenderGraph(VkDevice device, VkPhysicalDevice physicalDevice)
    : _device(device), _physicalDevice(physicalDevice)
{
}

VulkanRenderGraph::~VulkanRenderGraph()
{
    Shutdown();
}

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
    ImageNode node{};
    node.image          = image;
    node.aspect         = aspect;
    node.current        = { currentLayout, currentStage, currentAccess };
    node.initial        = node.current;
    node.isTransient    = false;
    _images.push_back(std::move(node));
    return h;
}

// ---------------------------------------------------------------------------
// Phase 26: CreateTransientImage
// ---------------------------------------------------------------------------
VKRGHandle VulkanRenderGraph::CreateTransientImage(const char*             name,
                                                     const VkImageCreateInfo& createInfo,
                                                     VkImageLayout           initialLayout,
                                                     VkImageAspectFlags      aspect)
{
    assert(_device != VK_NULL_HANDLE && "VulkanRenderGraph: device required for transient images");

    VKRGHandle h = static_cast<VKRGHandle>(_images.size());
    ImageNode node{};
    node.name          = name ? name : "";
    node.image         = VK_NULL_HANDLE;   // created in _CreateTransientResources()
    node.aspect        = aspect;
    node.current       = { initialLayout, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0 };
    node.initial       = node.current;
    node.isTransient   = true;
    node.transientCI   = createInfo;
    node.initialLayout = initialLayout;
    _images.push_back(std::move(node));
    return h;
}

// ---------------------------------------------------------------------------
// Phase 26: GetTransientImage
// ---------------------------------------------------------------------------
VkImage VulkanRenderGraph::GetTransientImage(VKRGHandle handle) const
{
    if (handle == VKRG_NULL_HANDLE || handle >= _images.size()) return VK_NULL_HANDLE;
    if (!_images[handle].isTransient) return VK_NULL_HANDLE;
    return _images[handle].image;
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
// Phase 26: DAG reference-count flood-fill cull
// Matches DX12 RenderGraph::_CullPasses() algorithm.
// ---------------------------------------------------------------------------
void VulkanRenderGraph::_CullPasses()
{
    const size_t N = _passes.size();
    if (N == 0) return;

    // Build lastWriter map: for each image, which pass last writes it
    std::vector<int> lastWriter(_images.size(), -1);
    for (size_t pi = 0; pi < N; ++pi)
        for (auto& wr : _passes[pi]._writes)
            if (wr.handle < _images.size())
                lastWriter[wr.handle] = static_cast<int>(pi);

    // Seed: side-effect passes are unconditionally live
    for (auto& pass : _passes)
        pass._live = pass._sideEffect;

    // Flood-fill backward until stable
    bool changed = true;
    while (changed)
    {
        changed = false;
        for (int j = static_cast<int>(N) - 1; j >= 0; --j)
        {
            if (!_passes[j]._live) continue;
            for (auto& rd : _passes[j]._reads)
            {
                if (rd.handle >= _images.size()) continue;
                int producer = lastWriter[rd.handle];
                if (producer >= 0 && !_passes[producer]._live)
                {
                    _passes[producer]._live = true;
                    changed = true;
                }
            }
        }
    }

#ifdef _DEBUG
    for (auto& pass : _passes)
        if (!pass._live)
            LUNA_LOG_INFO("[VKRenderGraph] Culled dead pass '%s'", pass._name.c_str());
#endif
}

// ---------------------------------------------------------------------------
// Phase 26: Compute [firstPass, lastPass] for each transient image.
// Only live passes are counted.
// ---------------------------------------------------------------------------
void VulkanRenderGraph::_ComputeTransientLifetimes()
{
    for (size_t pi = 0; pi < _passes.size(); ++pi)
    {
        if (!_passes[pi]._live) continue;

        auto touch = [&](VKRGHandle handle)
        {
            if (handle == VKRG_NULL_HANDLE || handle >= _images.size()) return;
            ImageNode& img = _images[handle];
            if (!img.isTransient) return;

            int idx = static_cast<int>(pi);
            if (img.firstPass < 0 || idx < img.firstPass) img.firstPass = idx;
            if (img.lastPass  < 0 || idx > img.lastPass)  img.lastPass  = idx;
        };

        for (auto& rd : _passes[pi]._reads)  touch(rd.handle);
        for (auto& wr : _passes[pi]._writes) touch(wr.handle);
    }
}

// ---------------------------------------------------------------------------
// Phase 26: Greedy interval-graph colouring for alias slot assignment.
// Identical algorithm to DX12 RenderGraph::_AssignAliasingSlots().
// ---------------------------------------------------------------------------
void VulkanRenderGraph::_AssignAliasingSlots()
{
    // Collect transient image indices sorted by firstPass
    std::vector<uint32_t> order;
    for (uint32_t i = 0; i < static_cast<uint32_t>(_images.size()); ++i)
        if (_images[i].isTransient && _images[i].firstPass >= 0)
            order.push_back(i);

    if (order.empty()) return;

    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        return _images[a].firstPass < _images[b].firstPass;
    });

    // Query memory requirements for each transient image
    for (uint32_t ri : order)
    {
        ImageNode& img = _images[ri];

        // Create a temporary VkImage to query memory requirements
        VkImage tempImage = VK_NULL_HANDLE;
        VkResult vr = vkCreateImage(_device, &img.transientCI, nullptr, &tempImage);
        if (vr != VK_SUCCESS)
        {
            LUNA_LOG_ERROR("[VKRenderGraph] Failed to create temp image for '%s': %d",
                           img.name.c_str(), vr);
            img.sizeBytes = 64 * 1024;  // 64 KB fallback
            img.memTypeBits = UINT32_MAX;
            continue;
        }

        VkMemoryRequirements memReq{};
        vkGetImageMemoryRequirements(_device, tempImage, &memReq);
        img.sizeBytes   = memReq.size;
        img.memTypeBits = memReq.memoryTypeBits;

        vkDestroyImage(_device, tempImage, nullptr);
    }

    // Greedy interval colouring: reuse slots whose lastPass < current firstPass
    struct SlotInfo { int lastPass; VkDeviceSize sizeBytes; uint32_t memTypeBits; };
    std::vector<SlotInfo> slots;

    for (uint32_t ri : order)
    {
        ImageNode& img = _images[ri];

        int chosen = -1;
        for (int si = 0; si < static_cast<int>(slots.size()); ++si)
        {
            if (slots[si].lastPass < img.firstPass &&
                (slots[si].memTypeBits & img.memTypeBits) != 0)  // compatible memory types
            {
                chosen = si;
                break;
            }
        }

        if (chosen < 0)
        {
            chosen = static_cast<int>(slots.size());
            slots.push_back({ img.lastPass, img.sizeBytes, img.memTypeBits });
        }
        else
        {
            slots[chosen].lastPass     = std::max(slots[chosen].lastPass, img.lastPass);
            slots[chosen].sizeBytes    = std::max(slots[chosen].sizeBytes, img.sizeBytes);
            slots[chosen].memTypeBits &= img.memTypeBits;  // intersect compatible types
        }

        img.aliasSlot = chosen;
    }

    // Materialise alias slot descriptors
    _aliasSlots.resize(slots.size());
    for (size_t si = 0; si < slots.size(); ++si)
    {
        _aliasSlots[si].sizeBytes   = slots[si].sizeBytes;
        _aliasSlots[si].memTypeBits = slots[si].memTypeBits;
    }
}

// ---------------------------------------------------------------------------
// Phase 26: Find a memory type index matching filter bits + property flags.
// ---------------------------------------------------------------------------
uint32_t VulkanRenderGraph::_FindMemoryType(uint32_t filter, VkMemoryPropertyFlags props) const
{
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(_physicalDevice, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((filter & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props)
            return i;
    return UINT32_MAX;
}

// ---------------------------------------------------------------------------
// Phase 26: Allocate VkDeviceMemory per alias slot, create and bind VkImages
// for each transient resource.
// ---------------------------------------------------------------------------
bool VulkanRenderGraph::_CreateTransientResources()
{
    if (_device == VK_NULL_HANDLE) return true;  // no device — nothing to do
    if (_aliasSlots.empty()) return true;

    // Allocate one VkDeviceMemory per alias slot
    for (auto& slot : _aliasSlots)
    {
        if (slot.sizeBytes == 0) continue;

        uint32_t memType = _FindMemoryType(slot.memTypeBits,
                                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (memType == UINT32_MAX)
        {
            LUNA_LOG_ERROR("[VKRenderGraph] No suitable memory type for alias slot (size %llu B)",
                           static_cast<unsigned long long>(slot.sizeBytes));
            return false;
        }

        VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        ai.allocationSize  = slot.sizeBytes;
        ai.memoryTypeIndex = memType;

        VkResult vr = vkAllocateMemory(_device, &ai, nullptr, &slot.memory);
        if (vr != VK_SUCCESS)
        {
            LUNA_LOG_ERROR("[VKRenderGraph] Failed to allocate alias slot memory (%llu B): %d",
                           static_cast<unsigned long long>(slot.sizeBytes), vr);
            return false;
        }
    }

    // Create VkImage + bind for each transient resource
    for (auto& img : _images)
    {
        if (!img.isTransient || img.aliasSlot < 0) continue;

        AliasSlot& slot = _aliasSlots[img.aliasSlot];
        if (slot.memory == VK_NULL_HANDLE) continue;

        VkResult vr = vkCreateImage(_device, &img.transientCI, nullptr, &img.image);
        if (vr != VK_SUCCESS)
        {
            LUNA_LOG_ERROR("[VKRenderGraph] Failed to create transient image '%s': %d",
                           img.name.c_str(), vr);
            return false;
        }

        vr = vkBindImageMemory(_device, img.image, slot.memory, 0);
        if (vr != VK_SUCCESS)
        {
            LUNA_LOG_ERROR("[VKRenderGraph] Failed to bind transient image '%s': %d",
                           img.name.c_str(), vr);
            vkDestroyImage(_device, img.image, nullptr);
            img.image = VK_NULL_HANDLE;
            return false;
        }

        _ownedTransientImages.push_back(img.image);
    }

    // Log memory savings
    VkDeviceSize totalUsed = 0, totalUnaliased = 0;
    for (auto& slot : _aliasSlots) totalUsed += slot.sizeBytes;
    for (auto& img : _images)
        if (img.isTransient && img.aliasSlot >= 0) totalUnaliased += img.sizeBytes;

    if (totalUnaliased > 0)
    {
        VkDeviceSize saved = totalUnaliased - totalUsed;
        LUNA_LOG_INFO("[VKRenderGraph] Transient aliasing: %u slots, %llu KB used "
                      "(vs %llu KB unaliased, saved %llu KB / %.0f%%)",
                      static_cast<uint32_t>(_aliasSlots.size()),
                      static_cast<unsigned long long>(totalUsed / 1024),
                      static_cast<unsigned long long>(totalUnaliased / 1024),
                      static_cast<unsigned long long>(saved / 1024),
                      totalUnaliased > 0 ? 100.0 * saved / totalUnaliased : 0.0);
    }

    return true;
}

// ---------------------------------------------------------------------------
// Compile: Phase 26 full pipeline (DAG cull → transient aliasing → barriers)
// ---------------------------------------------------------------------------
void VulkanRenderGraph::Compile()
{
    // 1. DAG cull (Phase 26 — replaces Phase 18C inline cull)
    _CullPasses();

    // 2. Transient lifetime analysis
    _ComputeTransientLifetimes();

    // 3. Alias slot assignment (interval graph colouring)
    _AssignAliasingSlots();

    // 4. Allocate memory + create VkImages
    if (!_CreateTransientResources())
        LUNA_LOG_WARN("[VKRenderGraph] Transient resource creation partially failed; "
                      "affected passes may not render correctly");

    _compiled = true;
}

// ---------------------------------------------------------------------------
// EmitBarrier
// ---------------------------------------------------------------------------
void VulkanRenderGraph::EmitBarrier(VkCommandBuffer cmd, ImageNode& img,
                                     VkImageLayout        newLayout,
                                     VkPipelineStageFlags newStage,
                                     VkAccessFlags        newAccess)
{
    if (img.image == VK_NULL_HANDLE) return;

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

// ---------------------------------------------------------------------------
// Execute: emit barriers + aliasing barriers + run live-pass lambdas
// ---------------------------------------------------------------------------
void VulkanRenderGraph::Execute(VkCommandBuffer cmd, VulkanGPUProfiler* profiler)
{
    // Phase 26: Track which transient image currently "owns" each alias slot.
    std::vector<VkImage> slotCurrentResident(_aliasSlots.size(), VK_NULL_HANDLE);

    for (auto& pass : _passes)
    {
        if (!pass._live) continue;

        // Phase 26: Check for aliasing transitions on writes to transients.
        // When a different transient takes over a slot, emit a barrier with
        // oldLayout=UNDEFINED to invalidate the previous contents.
        for (auto& wr : pass._writes)
        {
            if (wr.handle == VKRG_NULL_HANDLE || wr.handle >= _images.size()) continue;
            ImageNode& img = _images[wr.handle];
            if (!img.isTransient || img.aliasSlot < 0 || img.image == VK_NULL_HANDLE) continue;

            VkImage& slotRes = slotCurrentResident[img.aliasSlot];
            if (slotRes != VK_NULL_HANDLE && slotRes != img.image)
            {
                // New resident taking over this memory slot — emit aliasing barrier.
                // oldLayout=UNDEFINED discards previous contents (Vulkan spec §12.4).
                VkImageMemoryBarrier ab{};
                ab.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                ab.srcAccessMask                   = VK_ACCESS_MEMORY_WRITE_BIT;
                ab.dstAccessMask                   = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
                ab.oldLayout                       = VK_IMAGE_LAYOUT_UNDEFINED;
                ab.newLayout                       = img.initialLayout;
                ab.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
                ab.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
                ab.image                           = img.image;
                ab.subresourceRange.aspectMask     = img.aspect;
                ab.subresourceRange.baseMipLevel   = 0;
                ab.subresourceRange.levelCount     = VK_REMAINING_MIP_LEVELS;
                ab.subresourceRange.baseArrayLayer = 0;
                ab.subresourceRange.layerCount     = VK_REMAINING_ARRAY_LAYERS;

                vkCmdPipelineBarrier(cmd,
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                    0, 0, nullptr, 0, nullptr, 1, &ab);

                // Reset tracked state since layout was set to UNDEFINED → initialLayout
                img.current = { img.initialLayout, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0 };
            }
            slotRes = img.image;
        }

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
// Reset: clear passes and restore image states to initial.
// Transient resources are released (images + memory).
// ---------------------------------------------------------------------------
void VulkanRenderGraph::Reset()
{
    _passes.clear();
    for (auto& img : _images)
        img.current = img.initial;
    _images.clear();

    // Phase 26: destroy transient images and free alias slot memory
    Shutdown();
}

// ---------------------------------------------------------------------------
// Phase 26: Shutdown — release all transient VkImages and VkDeviceMemory.
// ---------------------------------------------------------------------------
void VulkanRenderGraph::Shutdown()
{
    if (_device == VK_NULL_HANDLE) return;

    for (VkImage img : _ownedTransientImages)
    {
        if (img != VK_NULL_HANDLE)
            vkDestroyImage(_device, img, nullptr);
    }
    _ownedTransientImages.clear();

    for (auto& slot : _aliasSlots)
    {
        if (slot.memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(_device, slot.memory, nullptr);
            slot.memory = VK_NULL_HANDLE;
        }
    }
    _aliasSlots.clear();

    _compiled = false;
}

} // namespace Luna

#endif // LUNA_ENABLE_VULKAN
