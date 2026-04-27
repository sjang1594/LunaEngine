#pragma once
// VulkanRenderGraph.h — Phase 18C + Phase 26: Vulkan render graph with automatic
// barrier scheduling and transient resource aliasing.
//
// Phase 18C: Barrier scheduling for persistent (imported) images.
// Phase 26:  Transient image support — graph-owned VkImages that share VkDeviceMemory
//            when their lifetimes don't overlap (interval-graph colouring, matching
//            DX12 RenderGraph Phase 14).
//
// Usage:
//   VulkanRenderGraph rg(device, physicalDevice);
//   auto hdr = rg.ImportImage(_hdrImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
//                              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
//                              VK_ACCESS_SHADER_READ_BIT);
//
//   // Transient image (graph allocates + frees memory):
//   VkImageCreateInfo ci = { ... };
//   auto hTemp = rg.CreateTransientImage("Temp", ci, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
//
//   rg.AddPass("MyPass").Write(hTemp, ...).SideEffect().Execute([](VkCommandBuffer cmd){ ... });
//   rg.Compile();   // DAG cull → lifetime analysis → alias slots → allocate
//   rg.Execute(cmd);

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

    // Phase 26: unified image node — holds both imported and transient images.
    struct ImageNode
    {
        std::string        name;
        VkImage            image  = VK_NULL_HANDLE;
        VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        ImageState         current;   // tracked state (mutated as passes run)
        ImageState         initial;   // state at import time (for reset)

        // Transient-only fields (Phase 26)
        bool               isTransient  = false;
        VkImageCreateInfo  transientCI  = {};       // creation info for transient
        VkImageLayout      initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        int                firstPass    = -1;        // earliest live pass that touches this
        int                lastPass     = -1;        // latest  live pass that touches this
        int                aliasSlot    = -1;        // assigned alias memory slot
        VkDeviceSize       sizeBytes    = 0;         // memory requirement
        uint32_t           memTypeBits  = 0;         // compatible memory type bits
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
        bool                                     _live       = false;  // Phase 26: DAG cull result
        std::function<void(VkCommandBuffer)>     _executeFn;

        struct Dep { VKRGHandle handle; VkImageLayout layout;
                     VkPipelineStageFlags stage; VkAccessFlags access; };
        std::vector<Dep> _reads;
        std::vector<Dep> _writes;
    };

    // -------------------------------------------------------------------
    // Constructors
    // -------------------------------------------------------------------

    // Default constructor — no transient image support (Phase 18C compat).
    VulkanRenderGraph() = default;

    // Phase 26 constructor — enables transient image creation.
    VulkanRenderGraph(VkDevice device, VkPhysicalDevice physicalDevice);

    ~VulkanRenderGraph();

    // -------------------------------------------------------------------
    // Resource declarations
    // -------------------------------------------------------------------

    // Import a persistent image into the graph.
    // aspect: VK_IMAGE_ASPECT_COLOR_BIT or DEPTH_BIT.
    VKRGHandle ImportImage(VkImage image,
                           VkImageLayout        currentLayout,
                           VkPipelineStageFlags currentStage,
                           VkAccessFlags        currentAccess,
                           VkImageAspectFlags   aspect = VK_IMAGE_ASPECT_COLOR_BIT);

    // Phase 26: Declare a transient image whose memory is managed by this graph.
    // The actual VkImage is created during Compile() on aliased VkDeviceMemory.
    // Requires non-null device/physicalDevice from constructor.
    VKRGHandle CreateTransientImage(const char*            name,
                                     const VkImageCreateInfo& createInfo,
                                     VkImageLayout           initialLayout,
                                     VkImageAspectFlags      aspect = VK_IMAGE_ASPECT_COLOR_BIT);

    // Phase 26: Retrieve the VkImage created for a transient handle.
    // Only valid after Compile(). Returns VK_NULL_HANDLE for imported handles.
    VkImage GetTransientImage(VKRGHandle handle) const;

    // -------------------------------------------------------------------
    // Pass declarations
    // -------------------------------------------------------------------

    // Add a render pass; returns a PassBuilder to chain Read/Write/Execute calls.
    // Uses std::deque internally — references remain valid across subsequent AddPass() calls.
    PassBuilder& AddPass(const char* name);

    // -------------------------------------------------------------------
    // Compile + Execute
    // -------------------------------------------------------------------

    // 1) DAG reference-count cull — mark live passes.
    // 2) Transient lifetime analysis — compute [firstPass, lastPass] per transient.
    // 3) Interval-graph colouring — assign transients to shared memory slots.
    // 4) Allocate VkDeviceMemory per slot, create + bind VkImages.
    // 5) Barrier schedule ready for Execute().
    void Compile();

    // Emit barriers and invoke execute callbacks in compiled order.
    // Emits aliasing barriers (oldLayout=UNDEFINED) between transients on same slot.
    // If profiler is non-null and enabled, auto-inserts timestamps around each pass.
    void Execute(VkCommandBuffer cmd, class VulkanGPUProfiler* profiler = nullptr);

    // Discard all passes and reset image states to their imported values.
    // Transient images and memory are released (call before reuse or destruction).
    void Reset();

    // Release transient VkImages and VkDeviceMemory. Called by destructor.
    void Shutdown();

private:
    // Phase 26: Transient resource pipeline
    void _CullPasses();
    void _ComputeTransientLifetimes();
    void _AssignAliasingSlots();
    bool _CreateTransientResources();

    void EmitBarrier(VkCommandBuffer cmd,
                     ImageNode&      img,
                     VkImageLayout        newLayout,
                     VkPipelineStageFlags newStage,
                     VkAccessFlags        newAccess);

    uint32_t _FindMemoryType(uint32_t filter, VkMemoryPropertyFlags props) const;

    // -------------------------------------------------------------------
    // Members
    // -------------------------------------------------------------------
    VkDevice         _device         = VK_NULL_HANDLE;
    VkPhysicalDevice _physicalDevice = VK_NULL_HANDLE;

    std::vector<ImageNode>     _images;
    std::deque<PassBuilder>    _passes;  // deque: references stable across push_back

    // Phase 26: Transient resource aliasing
    struct AliasSlot
    {
        VkDeviceMemory memory    = VK_NULL_HANDLE;
        VkDeviceSize   sizeBytes = 0;
        uint32_t       memTypeBits = UINT32_MAX;  // intersection of all residents
    };
    std::vector<AliasSlot> _aliasSlots;
    std::vector<VkImage>   _ownedTransientImages;  // VkImages created by graph (for cleanup)

    bool _compiled = false;
};

} // namespace Luna

#endif // LUNA_ENABLE_VULKAN
