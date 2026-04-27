#pragma once

#include <directx/d3d12.h>
#include <functional>
#include <vector>
#include <cstdint>
#include <string>

namespace Luna
{

using RGResourceHandle = uint32_t;
constexpr RGResourceHandle   RG_NULL_HANDLE    = UINT32_MAX;
// Sentinel: when finalState == RG_STATE_PRESERVE, ImportTexture sets finalState = currentState
constexpr D3D12_RESOURCE_STATES RG_STATE_PRESERVE = (D3D12_RESOURCE_STATES)UINT32_MAX;

class RenderGraph;

// ---------------------------------------------------------------------------
// PassBuilder — fluent interface returned by RenderGraph::AddPass().
// Holds an index into RenderGraph::_passes (safe across vector reallocation).
// ---------------------------------------------------------------------------
class PassBuilder
{
public:
    PassBuilder(RenderGraph* rg, uint32_t passIndex);

    PassBuilder& Read (RGResourceHandle handle, D3D12_RESOURCE_STATES state);
    PassBuilder& Write(RGResourceHandle handle, D3D12_RESOURCE_STATES state);
    PassBuilder& Execute(std::function<void(ID3D12GraphicsCommandList*)> fn);

    // Mark this pass as having external side effects (GPU writes visible outside
    // the graph, e.g. binding RTVs that DrawMesh() will use after Execute()).
    // Side-effect passes are never culled by the DAG cull step.
    PassBuilder& SideEffect();

private:
    RenderGraph* _rg;
    uint32_t     _passIndex;
};

// ---------------------------------------------------------------------------
// RenderGraph — data-driven barrier scheduling for a single frame.
//
// Phase 14 additions over Phase 6:
//   • DAG cull  — passes whose outputs are never consumed (and have no side
//                 effects) are skipped at Execute() time.
//   • Transient resource aliasing — resources declared with
//                 CreateTransientTexture() are given D3D12 placed resources on
//                 aliasing heaps.  Resources whose lifetimes do not overlap
//                 share the same heap memory (interval graph colouring).
//
// Usage:
//   RenderGraph rg(device, cmdList);
//
//   // Persistent resource (caller owns allocation):
//   auto hBB = rg.ImportTexture("BackBuffer", res, PRESENT, RENDER_TARGET);
//
//   // Transient resource (graph allocates / frees memory):
//   D3D12_RESOURCE_DESC rtDesc = CD3DX12_RESOURCE_DESC::Tex2D(...);
//   auto hGbuf = rg.CreateTransientTexture("GBuf0", rtDesc, RENDER_TARGET);
//
//   rg.AddPass("GBuffer Fill")
//     .SideEffect()
//     .Write(hGbuf, RENDER_TARGET)
//     .Execute([](auto* cmd){ ... });
//
//   rg.AddPass("Lighting")
//     .Read(hGbuf, PIXEL_SHADER_RESOURCE)
//     .Write(hBB,  RENDER_TARGET)
//     .Execute([](auto* cmd){ ... });
//
//   rg.Compile();   // DAG cull → lifetime analysis → heap aliasing → barrier schedule
//   rg.Execute();   // inject barriers, run live-pass lambdas
// ---------------------------------------------------------------------------
class RenderGraph
{
public:
    // device is required for transient resource creation (placed resources on aliasing heaps).
    // If device is null, CreateTransientTexture() is unavailable but all other features work.
    explicit RenderGraph(ID3D12Device* device, ID3D12GraphicsCommandList* cmd);

    // Legacy constructor (no transient resource support) — preserves existing call sites.
    explicit RenderGraph(ID3D12GraphicsCommandList* cmd);

    ~RenderGraph();

    // -------------------------------------------------------------------
    // Resource declarations
    // -------------------------------------------------------------------

    // Import a persistent resource.  finalState defaults to currentState (auto-restore).
    RGResourceHandle ImportTexture(const char*           name,
                                   ID3D12Resource*       resource,
                                   D3D12_RESOURCE_STATES currentState,
                                   D3D12_RESOURCE_STATES finalState = RG_STATE_PRESERVE);

    // Declare a transient texture whose memory is managed by this RenderGraph.
    // The actual ID3D12Resource* is created during Compile() on an aliasing heap.
    // Requires a non-null device pointer.
    RGResourceHandle CreateTransientTexture(const char*                  name,
                                            const D3D12_RESOURCE_DESC&   desc,
                                            D3D12_RESOURCE_STATES        initialState,
                                            const D3D12_CLEAR_VALUE*     clearValue = nullptr);

    // Retrieve the placed resource created for a transient handle.
    // Only valid after Compile().  Returns nullptr for imported handles.
    ID3D12Resource* GetTransientResource(RGResourceHandle handle) const;

    // -------------------------------------------------------------------
    // Pass declarations
    // -------------------------------------------------------------------
    PassBuilder AddPass(const char* name);

    // -------------------------------------------------------------------
    // Compile + Execute
    // -------------------------------------------------------------------

    // 1) DAG reference-count cull — mark which passes are required.
    // 2) Transient lifetime analysis — compute [firstPass, lastPass] per transient.
    // 3) Interval-graph-colouring aliasing — assign resources to shared heap slots.
    // 4) Placed-resource creation — one D3D12 heap per alias slot.
    // 5) Barrier schedule — build per-pass preBarriers + finalBarriers.
    void Compile();

    // Inject barriers and run each live-pass lambda in declaration order.
    // Emits aliasing barriers between resources that share a heap slot.
    void Execute();

    // Release aliasing heaps and placed transient resources.
    // Called automatically by destructor.
    void Shutdown();

    // -------------------------------------------------------------------
    // Internal helpers (public so PassBuilder can reach them)
    // -------------------------------------------------------------------
    void _PassRead      (uint32_t passIndex, RGResourceHandle handle, D3D12_RESOURCE_STATES state);
    void _PassWrite     (uint32_t passIndex, RGResourceHandle handle, D3D12_RESOURCE_STATES state);
    void _PassExecute   (uint32_t passIndex, std::function<void(ID3D12GraphicsCommandList*)> fn);
    void _PassSideEffect(uint32_t passIndex);

private:
    // ------------------------------------------------------------------
    // Internal data structures
    // ------------------------------------------------------------------

    struct ResourceAccess
    {
        RGResourceHandle      handle;
        D3D12_RESOURCE_STATES state;
    };

    struct TransientDesc
    {
        D3D12_RESOURCE_DESC   desc;
        D3D12_CLEAR_VALUE     clearValue;
        bool                  hasClearValue = false;
        D3D12_RESOURCE_STATES initialState;
    };

    struct ResourceNode
    {
        std::string           name;
        ID3D12Resource*       resource;      // nullptr for transient until Compile()
        D3D12_RESOURCE_STATES currentState;
        D3D12_RESOURCE_STATES finalState;

        // Transient-resource fields (only valid when isTransient == true)
        bool                  isTransient   = false;
        TransientDesc         transientDesc;
        int                   firstPass     = -1;   // earliest active pass index
        int                   lastPass      = -1;   // latest  active pass index
        int                   aliasSlot     = -1;   // aliasing heap slot index
        UINT64                aliasOffset   = 0;
        UINT64                sizeBytes     = 0;

        // Ref-count for DAG cull
        int                   refCount      = 0;
    };

    struct PassNode
    {
        std::string                                      name;
        std::vector<ResourceAccess>                      reads;
        std::vector<ResourceAccess>                      writes;
        std::function<void(ID3D12GraphicsCommandList*)>  execute;
        std::vector<D3D12_RESOURCE_BARRIER>              preBarriers;
        bool sideEffect = false;
        bool live       = false;
    };

    // One D3D12 heap shared by all alias-slot members whose lifetimes don't overlap
    struct AliasSlot
    {
        ComPtr<ID3D12Heap> heap;
        UINT64             sizeBytes = 0;
    };

    // ------------------------------------------------------------------
    // Private methods
    // ------------------------------------------------------------------
    void _CullPasses();
    void _ComputeTransientLifetimes();
    void _AssignAliasingSlots();
    bool _CreateTransientResources();
    void _ScheduleBarriers();

    // ------------------------------------------------------------------
    // Members
    // ------------------------------------------------------------------
    ID3D12Device*                       _device        = nullptr;
    ID3D12GraphicsCommandList*          _cmd           = nullptr;

    std::vector<ResourceNode>           _resources;
    std::vector<PassNode>               _passes;
    std::vector<D3D12_RESOURCE_BARRIER> _finalBarriers;

    std::vector<AliasSlot>              _aliasSlots;
    std::vector<ComPtr<ID3D12Resource>> _transientResources;  // placed resources owned by graph

    bool _compiled = false;
};

} // namespace Luna
