#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <directx/d3d12.h>
#include <directx/d3dx12_barriers.h>

// ---------------------------------------------------------------------------
// Phase 6 — Render Graph (header-only)
//
// Lightweight per-frame render graph. Responsibilities:
//   • Track each imported resource's current GPU state.
//   • Automatically emit D3D12 resource barriers before each pass execute lambda.
//   • Execute passes in declaration order (no reordering / culling).
//
// Intentional non-goals (deferred to later phases):
//   • Transient resource allocation  (Phase 7+)
//   • Automatic pass reordering / culling
//   • Cross-frame state persistence  (managed externally via _shadowInSRVState flag)
// ---------------------------------------------------------------------------

namespace Luna
{

using RGResourceHandle = uint32_t;
constexpr RGResourceHandle RG_NULL_HANDLE = UINT32_MAX;

// Declares a single resource access within a pass.
// The graph transitions the resource from its tracked state to requiredState
// before invoking the pass execute lambda.
struct RGResourceAccess
{
    RGResourceHandle      handle;
    D3D12_RESOURCE_STATES requiredState;
};

class RenderGraph
{
  public:
    // Reset all imported resources and registered passes.
    // Must be called at the start of each frame before adding passes.
    inline void Reset()
    {
        _resources.clear();
        _passes.clear();
    }

    // Import an external resource (back-buffer, depth, shadow UAV, etc.)
    // currentState must reflect the resource's actual GPU state at call time.
    inline RGResourceHandle ImportResource(ID3D12Resource*       resource,
                                           D3D12_RESOURCE_STATES currentState)
    {
        auto handle = static_cast<RGResourceHandle>(_resources.size());
        _resources.push_back({resource, currentState});
        return handle;
    }

    // Register a named pass.
    // The graph emits barriers to satisfy every RGResourceAccess declaration,
    // then invokes execute. execute may be nullptr for barrier-only passes.
    inline void AddPass(const std::string&                             name,
                        std::vector<RGResourceAccess>                  accesses,
                        std::function<void(ID3D12GraphicsCommandList*)> execute)
    {
        _passes.push_back({std::move(name), std::move(accesses), std::move(execute)});
    }

    // Emit all barriers and invoke pass lambdas in declaration order.
    inline void Execute(ID3D12GraphicsCommandList* cmdList)
    {
        for (auto& pass : _passes)
        {
            // Collect transitions needed to bring each resource to its required state.
            std::vector<D3D12_RESOURCE_BARRIER> barriers;
            for (auto& access : pass.accesses)
            {
                if (access.handle >= static_cast<RGResourceHandle>(_resources.size()))
                    continue;

                auto& node = _resources[access.handle];
                if (node.currentState != access.requiredState)
                {
                    barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
                        node.resource,
                        node.currentState,
                        access.requiredState));
                    node.currentState = access.requiredState;
                }
            }

            if (!barriers.empty())
                cmdList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());

            if (pass.execute)
                pass.execute(cmdList);
        }
    }

    // Query the last-tracked state of a resource (valid after Execute to
    // know what state a resource was left in for cross-frame tracking).
    inline ID3D12Resource* GetResource(RGResourceHandle handle) const
    {
        if (handle >= static_cast<RGResourceHandle>(_resources.size()))
            return nullptr;
        return _resources[handle].resource;
    }

    inline D3D12_RESOURCE_STATES GetCurrentState(RGResourceHandle handle) const
    {
        if (handle >= static_cast<RGResourceHandle>(_resources.size()))
            return D3D12_RESOURCE_STATE_COMMON;
        return _resources[handle].currentState;
    }

  private:
    struct ResourceNode
    {
        ID3D12Resource*       resource;
        D3D12_RESOURCE_STATES currentState;
    };

    struct PassEntry
    {
        std::string                                      name;
        std::vector<RGResourceAccess>                    accesses;
        std::function<void(ID3D12GraphicsCommandList*)>  execute;
    };

    std::vector<ResourceNode> _resources;
    std::vector<PassEntry>    _passes;
};

} // namespace Luna
