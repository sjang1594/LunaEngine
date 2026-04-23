#include "LunaPCH.h"
#include "Renderer/RenderGraph.h"
#include <directx/d3dx12_resource_helpers.h>
#include <algorithm>
#include <cassert>

namespace Luna
{

// ---------------------------------------------------------------------------
// PassBuilder
// ---------------------------------------------------------------------------

PassBuilder::PassBuilder(RenderGraph* rg, uint32_t passIndex)
    : _rg(rg), _passIndex(passIndex)
{
}

PassBuilder& PassBuilder::Read(RGResourceHandle handle, D3D12_RESOURCE_STATES state)
{
    _rg->_PassRead(_passIndex, handle, state);
    return *this;
}

PassBuilder& PassBuilder::Write(RGResourceHandle handle, D3D12_RESOURCE_STATES state)
{
    _rg->_PassWrite(_passIndex, handle, state);
    return *this;
}

PassBuilder& PassBuilder::Execute(std::function<void(ID3D12GraphicsCommandList*)> fn)
{
    _rg->_PassExecute(_passIndex, std::move(fn));
    return *this;
}

PassBuilder& PassBuilder::SideEffect()
{
    _rg->_PassSideEffect(_passIndex);
    return *this;
}

// ---------------------------------------------------------------------------
// RenderGraph — constructors / destructor
// ---------------------------------------------------------------------------

RenderGraph::RenderGraph(ID3D12Device* device, ID3D12GraphicsCommandList* cmd)
    : _device(device), _cmd(cmd)
{
}

RenderGraph::RenderGraph(ID3D12GraphicsCommandList* cmd)
    : _device(nullptr), _cmd(cmd)
{
}

RenderGraph::~RenderGraph()
{
    Shutdown();
}

void RenderGraph::Shutdown()
{
    // Release placed transient resources first, then heaps.
    _transientResources.clear();
    _aliasSlots.clear();
    _compiled = false;
}

// ---------------------------------------------------------------------------
// Resource declarations
// ---------------------------------------------------------------------------

RGResourceHandle RenderGraph::ImportTexture(const char*           name,
                                             ID3D12Resource*       resource,
                                             D3D12_RESOURCE_STATES currentState,
                                             D3D12_RESOURCE_STATES finalState)
{
    if (finalState == RG_STATE_PRESERVE)
        finalState = currentState;

    RGResourceHandle handle = static_cast<RGResourceHandle>(_resources.size());
    ResourceNode node{};
    node.name         = name ? name : "";
    node.resource     = resource;
    node.currentState = currentState;
    node.finalState   = finalState;
    node.isTransient  = false;
    _resources.push_back(std::move(node));
    return handle;
}

RGResourceHandle RenderGraph::CreateTransientTexture(const char*                  name,
                                                      const D3D12_RESOURCE_DESC&   desc,
                                                      D3D12_RESOURCE_STATES        initialState,
                                                      const D3D12_CLEAR_VALUE*     clearValue)
{
    assert(_device && "RenderGraph: device is required for transient resource creation");

    RGResourceHandle handle = static_cast<RGResourceHandle>(_resources.size());
    ResourceNode node{};
    node.name         = name ? name : "";
    node.resource     = nullptr;   // filled in by _CreateTransientResources()
    node.currentState = initialState;
    node.finalState   = initialState;   // auto-restore to initial by default
    node.isTransient  = true;

    node.transientDesc.desc        = desc;
    node.transientDesc.initialState = initialState;
    if (clearValue)
    {
        node.transientDesc.clearValue    = *clearValue;
        node.transientDesc.hasClearValue = true;
    }

    _resources.push_back(std::move(node));
    return handle;
}

ID3D12Resource* RenderGraph::GetTransientResource(RGResourceHandle handle) const
{
    if (handle == RG_NULL_HANDLE || handle >= _resources.size())
        return nullptr;
    return _resources[handle].resource;
}

// ---------------------------------------------------------------------------
// Pass declarations
// ---------------------------------------------------------------------------

PassBuilder RenderGraph::AddPass(const char* name)
{
    uint32_t index = static_cast<uint32_t>(_passes.size());
    PassNode p{};
    p.name = name ? name : "";
    _passes.push_back(std::move(p));
    return PassBuilder(this, index);
}

void RenderGraph::_PassRead(uint32_t passIndex, RGResourceHandle handle, D3D12_RESOURCE_STATES state)
{
    if (handle != RG_NULL_HANDLE)
        _passes[passIndex].reads.push_back({handle, state});
}

void RenderGraph::_PassWrite(uint32_t passIndex, RGResourceHandle handle, D3D12_RESOURCE_STATES state)
{
    if (handle != RG_NULL_HANDLE)
        _passes[passIndex].writes.push_back({handle, state});
}

void RenderGraph::_PassExecute(uint32_t passIndex, std::function<void(ID3D12GraphicsCommandList*)> fn)
{
    _passes[passIndex].execute = std::move(fn);
}

void RenderGraph::_PassSideEffect(uint32_t passIndex)
{
    _passes[passIndex].sideEffect = true;
}

// ---------------------------------------------------------------------------
// Phase 14: DAG reference-count flood-fill cull
//
// Algorithm:
//   1. Count how many passes declare each resource as a Read (refCount).
///   2. Seed live set: passes with sideEffect = true.
//   3. Flood-fill backward: for each live pass, for each resource it reads,
//      find the pass that last wrote that resource and mark it live.
//   4. Passes not reached stay culled (live = false).
//
// Note: we iterate in reverse until convergence, which handles arbitrary
// dependency depths without topological sort.
// ---------------------------------------------------------------------------
void RenderGraph::_CullPasses()
{
    const uint32_t N = static_cast<uint32_t>(_passes.size());
    if (N == 0) return;

    // Reset ref counts
    for (auto& res : _resources)
        res.refCount = 0;

    // Count reads per resource (for documentation; not strictly needed for cull)
    for (auto& pass : _passes)
        for (auto& acc : pass.reads)
            if (acc.handle < _resources.size())
                _resources[acc.handle].refCount++;

    // Build a map: for each resource, which pass last writes it.
    // We use "last write in declaration order" as the producer.
    // A resource can have multiple writers across passes; the final writer feeds later consumers.
    std::vector<int> lastWriter(_resources.size(), -1);
    for (uint32_t pi = 0; pi < N; ++pi)
        for (auto& acc : _passes[pi].writes)
            if (acc.handle < _resources.size())
                lastWriter[acc.handle] = static_cast<int>(pi);

    // Seed: side-effect passes are unconditionally live
    for (auto& pass : _passes)
        if (pass.sideEffect)
            pass.live = true;

    // Flood-fill: iterate backward until stable
    bool changed = true;
    while (changed)
    {
        changed = false;
        for (int pi = static_cast<int>(N) - 1; pi >= 0; --pi)
        {
            if (!_passes[pi].live) continue;

            // For each resource this live pass reads, mark its producer as live
            for (auto& acc : _passes[pi].reads)
            {
                if (acc.handle >= _resources.size()) continue;
                int producer = lastWriter[acc.handle];
                if (producer >= 0 && !_passes[producer].live)
                {
                    _passes[producer].live = true;
                    changed = true;
                }
            }
        }
    }

    // Any pass with no live outputs and no side effect stays culled.
    // Log culled passes (debug builds only):
#ifdef _DEBUG
    for (auto& pass : _passes)
    {
        if (!pass.live)
        {
            LUNA_LOG_WARN("[RenderGraph] Pass '%s' culled by DAG analysis (no live consumers)",
                          pass.name.c_str());
        }
    }
#endif
}

// ---------------------------------------------------------------------------
// Phase 14: Compute [firstPass, lastPass] for each transient resource.
// Only active (live) passes are counted.
// ---------------------------------------------------------------------------
void RenderGraph::_ComputeTransientLifetimes()
{
    const uint32_t N = static_cast<uint32_t>(_passes.size());

    for (uint32_t pi = 0; pi < N; ++pi)
    {
        if (!_passes[pi].live) continue;

        auto touch = [&](RGResourceHandle handle)
        {
            if (handle == RG_NULL_HANDLE || handle >= _resources.size()) return;
            ResourceNode& res = _resources[handle];
            if (!res.isTransient) return;

            int idx = static_cast<int>(pi);
            if (res.firstPass < 0 || idx < res.firstPass) res.firstPass = idx;
            if (res.lastPass  < 0 || idx > res.lastPass)  res.lastPass  = idx;
        };

        for (auto& acc : _passes[pi].reads)  touch(acc.handle);
        for (auto& acc : _passes[pi].writes) touch(acc.handle);
    }
}

// ---------------------------------------------------------------------------
// Phase 14: Greedy interval-graph colouring for aliasing slot assignment.
//
// Resources are sorted by firstPass.  We maintain a list of "free" slots
// whose lastPass is strictly less than the current resource's firstPass.
// If a free slot exists it is reused; otherwise a new slot is opened.
// The maximum required size of each slot is tracked across all residents.
// ---------------------------------------------------------------------------
void RenderGraph::_AssignAliasingSlots()
{
    // Collect transient resource indices sorted by firstPass
    std::vector<uint32_t> order;
    for (uint32_t i = 0; i < _resources.size(); ++i)
        if (_resources[i].isTransient && _resources[i].firstPass >= 0)
            order.push_back(i);

    // Sort by firstPass ascending
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b){
        return _resources[a].firstPass < _resources[b].firstPass;
    });

    // Track slot last-use so we know when it becomes free
    struct SlotInfo { int lastPass; UINT64 sizeBytes; };
    std::vector<SlotInfo> slots;

    for (uint32_t ri : order)
    {
        ResourceNode& res = _resources[ri];

        // Query D3D12 allocation requirements for this resource
        D3D12_RESOURCE_ALLOCATION_INFO allocInfo = {};
        if (_device)
        {
            // Add D3D12_RESOURCE_FLAG_ALLOW_ALIAS so the resource can share heap memory
            D3D12_RESOURCE_DESC desc = res.transientDesc.desc;
            desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;  // not needed but harmless
            allocInfo = _device->GetResourceAllocationInfo(0, 1, &desc);
        }
        res.sizeBytes = allocInfo.SizeInBytes;
        if (res.sizeBytes == 0) res.sizeBytes = 64 * 1024;  // 64 KB minimum safe fallback

        // Search for a free slot (lastPass < res.firstPass)
        int chosen = -1;
        for (int si = 0; si < (int)slots.size(); ++si)
        {
            if (slots[si].lastPass < res.firstPass)
            {
                chosen = si;
                break;
            }
        }

        if (chosen < 0)
        {
            // Open a new slot
            chosen = static_cast<int>(slots.size());
            slots.push_back({res.lastPass, res.sizeBytes});
        }
        else
        {
            // Reuse the slot — extend lastPass and grow size if needed
            slots[chosen].lastPass  = std::max(slots[chosen].lastPass, res.lastPass);
            slots[chosen].sizeBytes = std::max(slots[chosen].sizeBytes, res.sizeBytes);
        }

        res.aliasSlot   = chosen;
        res.aliasOffset = 0;  // all resources in a slot are placed at offset 0 (different resources)
    }

    // Materialise alias slot descriptors (size only; heaps created later)
    _aliasSlots.resize(slots.size());
    for (int si = 0; si < (int)slots.size(); ++si)
        _aliasSlots[si].sizeBytes = slots[si].sizeBytes;
}

// ---------------------------------------------------------------------------
// Phase 14: Allocate D3D12 aliasing heaps and create placed resources.
// ---------------------------------------------------------------------------
bool RenderGraph::_CreateTransientResources()
{
    if (!_device) return true;  // nothing to do without a device
    if (_aliasSlots.empty()) return true;

    // Create one heap per alias slot
    for (auto& slot : _aliasSlots)
    {
        if (slot.sizeBytes == 0) continue;

        D3D12_HEAP_DESC heapDesc = {};
        heapDesc.SizeInBytes = slot.sizeBytes;
        heapDesc.Properties  = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        // Allow all texture types (RT, DS, shader-visible) on this heap.
        // Placed resources on this heap can alias each other.
        heapDesc.Flags = D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES;

        HRESULT hr = _device->CreateHeap(&heapDesc, IID_PPV_ARGS(&slot.heap));
        if (FAILED(hr))
        {
            LUNA_LOG_ERROR("[RenderGraph] Failed to create aliasing heap (size %llu B): 0x%08X",
                           slot.sizeBytes, hr);
            return false;
        }
    }

    // Create placed resources for each transient resource
    for (uint32_t ri = 0; ri < _resources.size(); ++ri)
    {
        ResourceNode& res = _resources[ri];
        if (!res.isTransient || res.aliasSlot < 0) continue;

        AliasSlot& slot = _aliasSlots[res.aliasSlot];
        if (!slot.heap) continue;

        D3D12_RESOURCE_DESC desc = res.transientDesc.desc;
        // Resources placed on aliasing heaps must have the allow-alias flag cleared
        // (D3D12 will return an error if D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS is set
        //  without ALLOW_CROSS_ADAPTER on a default heap — remove it here)
        desc.Flags &= ~D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

        const D3D12_CLEAR_VALUE* pClear =
            res.transientDesc.hasClearValue ? &res.transientDesc.clearValue : nullptr;

        ComPtr<ID3D12Resource> placed;
        HRESULT hr = _device->CreatePlacedResource(
            slot.heap.Get(),
            res.aliasOffset,
            &desc,
            res.transientDesc.initialState,
            pClear,
            IID_PPV_ARGS(&placed));

        if (FAILED(hr))
        {
            LUNA_LOG_ERROR("[RenderGraph] Failed to create placed resource '%s': 0x%08X",
                           res.name.c_str(), hr);
            return false;
        }

        res.resource = placed.Get();
        _transientResources.push_back(std::move(placed));
    }

    return true;
}

// ---------------------------------------------------------------------------
// Phase 6 barrier schedule (updated for Phase 14 to skip culled passes and
// handle transient resources correctly)
// ---------------------------------------------------------------------------
void RenderGraph::_ScheduleBarriers()
{
    for (PassNode& pass : _passes)
    {
        if (!pass.live) continue;

        auto tryTransition = [&](RGResourceHandle handle, D3D12_RESOURCE_STATES needed)
        {
            if (handle == RG_NULL_HANDLE || handle >= _resources.size()) return;
            ResourceNode& res = _resources[handle];
            if (!res.resource) return;   // transient resource not created (aliasing failed)
            if (res.currentState != needed)
            {
                pass.preBarriers.push_back(
                    CD3DX12_RESOURCE_BARRIER::Transition(
                        res.resource,
                        res.currentState,
                        needed));
                res.currentState = needed;
            }
        };

        for (const ResourceAccess& a : pass.reads)  tryTransition(a.handle, a.state);
        for (const ResourceAccess& a : pass.writes) tryTransition(a.handle, a.state);
    }

    // Build final barriers: restore every resource to its declared finalState.
    for (ResourceNode& res : _resources)
    {
        if (!res.resource) continue;
        if (res.currentState != res.finalState)
        {
            _finalBarriers.push_back(
                CD3DX12_RESOURCE_BARRIER::Transition(
                    res.resource,
                    res.currentState,
                    res.finalState));
            res.currentState = res.finalState;
        }
    }
}

// ---------------------------------------------------------------------------
// Compile — full Phase 14 pipeline
// ---------------------------------------------------------------------------
void RenderGraph::Compile()
{
    // 1. DAG cull
    _CullPasses();

    // 2. Transient lifetime analysis (requires cull to know active passes)
    _ComputeTransientLifetimes();

    // 3. Aliasing slot assignment (interval graph colouring)
    _AssignAliasingSlots();

    // 4. Allocate heaps + create placed resources
    if (!_CreateTransientResources())
        LUNA_LOG_WARN("[RenderGraph] Transient resource creation partially failed; "
                      "affected passes may not render correctly");

    // 5. Barrier schedule
    _ScheduleBarriers();

    _compiled = true;
}

// ---------------------------------------------------------------------------
// Execute — inject barriers and run live-pass lambdas.
// Emits aliasing barriers between resources on the same heap slot when
// transitioning between them (D3D12 spec §19.3 aliasing barrier).
// ---------------------------------------------------------------------------
void RenderGraph::Execute()
{
    // Track which transient resource currently "owns" each alias slot.
    // We need to emit an aliasing barrier when moving from one resident to another.
    std::vector<ID3D12Resource*> slotCurrentResident(_aliasSlots.size(), nullptr);

    for (PassNode& pass : _passes)
    {
        if (!pass.live) continue;

        // Check for aliasing transitions: for each write to a transient resource,
        // if the slot currently holds a different resource, emit an aliasing barrier.
        std::vector<D3D12_RESOURCE_BARRIER> aliasingBarriers;
        for (auto& acc : pass.writes)
        {
            if (acc.handle == RG_NULL_HANDLE || acc.handle >= _resources.size()) continue;
            ResourceNode& res = _resources[acc.handle];
            if (!res.isTransient || res.aliasSlot < 0 || !res.resource) continue;

            ID3D12Resource*& slotRes = slotCurrentResident[res.aliasSlot];
            if (slotRes != nullptr && slotRes != res.resource)
            {
                // Transitioning to a new resource on this heap slot — aliasing barrier
                aliasingBarriers.push_back(
                    CD3DX12_RESOURCE_BARRIER::Aliasing(slotRes, res.resource));
            }
            slotRes = res.resource;
        }

        if (!aliasingBarriers.empty())
            _cmd->ResourceBarrier(static_cast<UINT>(aliasingBarriers.size()),
                                  aliasingBarriers.data());

        if (!pass.preBarriers.empty())
            _cmd->ResourceBarrier(static_cast<UINT>(pass.preBarriers.size()),
                                  pass.preBarriers.data());

        if (pass.execute)
            pass.execute(_cmd);
    }

    if (!_finalBarriers.empty())
        _cmd->ResourceBarrier(static_cast<UINT>(_finalBarriers.size()),
                              _finalBarriers.data());
}

} // namespace Luna
