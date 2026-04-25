LunaEngine Full System Evaluation
Overall Assessment
This is a well-structured, ambitious dual-backend (DX12 + Vulkan) engine with ~23 phases of incremental development. The architecture is coherent and production-oriented. Below are findings organized by subsystem.
 
1. Architecture & HAL Layer
Strengths:
Clean IRenderBackend abstraction with sensible virtual interface and default no-op implementations for optional features
Dual-backend parity (DX12 + Vulkan) for nearly all features including RT, SSR, SSAO, bloom, TAA, motion blur, Hi-Z
IGPUProfiler cleanly abstracts platform-specific timestamp query mechanisms
Issues:
using namespace DirectX in IRenderBackend.h (line 9) — pollutes every translation unit that includes the HAL header. Should be scoped to the Luna namespace or use explicit DirectX:: prefixes.
IRenderBackend is growing fat — 798 lines in VulkanBackend.h with ~100 member variables. The class has become a god object housing swapchain, G-buffer, deferred, CSM, SSAO, IBL, SSR, TAA, bloom, motion blur, RT, Hi-Z, and GPU-driven rendering. Consider extracting subsystems into owned objects (e.g., VulkanPostProcess, VulkanShadowSystem, VulkanRTSystem).
No RAII wrappers for Vulkan handles — all VkImage/VkDeviceMemory/VkImageView are raw handles. ~50 DestroyXxx() functions need to be called in the correct order.
 
2. Render Graph
DX12 RenderGraph (RenderGraph.h)
Solid design: DAG cull, transient resource aliasing via interval-graph colouring, placed resources on aliasing heaps
Good separation of Compile() and Execute() phases
VulkanRenderGraph (VulkanRenderGraph.h/cpp)
Image-only — no buffer barrier support. FlushDraws() retains manual barriers, which is documented and intentional but limits the graph's coverage.
Compile() liveness algorithm is O(n³) in the worst case (3 nested loops at lines 78-91). Fine for <50 passes, but worth noting.
AddPass() returns a reference to a vector element (line 41: return _passes.back()). If a subsequent AddPass() causes _passes to reallocate, all previously returned PassBuilder references become dangling. The comment in the header (line 91) warns about this across Reset() calls, but the real danger is between consecutive AddPass() calls within the same frame. The DX12 RenderGraph avoids this by storing an index.
Reset() clears _images (line 173) which means handles from ImportImage() are invalidated every frame. This is fine if all Import+AddPass+Compile+Execute+Reset happen within one frame, but the API doesn't enforce this constraint.
 
3. GPU Profiler
VulkanGPUProfiler
Correct double-buffered (triple-buffered, FRAME_LATENCY=3) query pool approach
Rolling average history with fixed 60-frame window is good for UI stability
WriteBeginTimestamp() auto-closes a previous unclosed pass (lines 133-140) — good defensive code
Issues:
_totalMs accumulates avg not ms (line 232). This is intentional (UI shows smoothed total), but the method name GetTotalGpuTimeMs() is ambiguous — it's actually "total average GPU time." Consider renaming or documenting.
No VK_QUERY_RESULT_WAIT_BIT in ReadbackResults() (line 188). The code uses VK_QUERY_RESULT_64_BIT only and early-returns on VK_NOT_READY. This is the correct approach for non-blocking readback, but if queries are never ready (e.g., queue never submitted), you'll silently get 0 results forever with no warning.
DX12 profiler has FRAME_LATENCY = 2; Vulkan has FRAME_LATENCY = 3 — this mirrors their respective FRAMES_IN_FLIGHT constants, which is correct, but the mismatch is implicit. Consider deriving from a shared constant or at least cross-referencing in comments.
GPUProfilerOverlay
Clean ImGui rendering with stacked bar chart and sortable table
Minor: the stacked bar (line 89) iterates results in declaration order regardless of _sortMode, while the table respects sorting. Stacked bar and table can be visually inconsistent when sorting by time.
 
4. Transform Gizmo
Strengths:
Isaac Sim-style universal gizmo (translate + rotate + scale simultaneously) is a good UX choice
3D-projected rotation rings with proper tangent-space construction
Screen-space constant-size scaling (ScreenSpaceScale())
GizmoOverlay window approach with NoInputs flag — click-through transparent overlay
Issues:
Global mutable state (static GizmoState s_state at line 83) — only one gizmo can exist at a time. If you ever need multiple viewports or multi-select, this breaks.
Float equality comparisons for hit priority (lines 261-263, 272-275, 289-291): if (dX == minAxisDist) — comparing floats with == after std::min(). This works because std::min returns one of its inputs by reference, but it's fragile. If any intermediate computation changes, this silently breaks. Use an epsilon or track the winning index explicitly.
Rotation uses screen-space angle which is intuitive for the user but doesn't respect local-space when localSpace=true. The rotation delta is applied directly to Euler angles (lines 424-440) regardless of the localSpace flag. For local-space rotation, you'd need to decompose the delta into the object's local frame.
RotateScreen axis (line 428-437): newRot.y += sign * screenDist * 0.5f — the 0.5f magic number is an arbitrary sensitivity that doesn't scale with viewport size or DPI.
Quaternion-to-Euler decomposition is duplicated — identical code exists in DecomposeTransform() (lines 116-126) and LunaApp.cpp (lines 186-195). Extract to a shared utility.
Y↔Z axis swap (lines 88-98) is handled ad-hoc with inline comments. The axis mapping (displayX=worldX, displayY=worldZ, displayZ=worldY) is scattered across 4+ places. A single DisplayToWorld / WorldToDisplay mapping table would be cleaner.
 
5. GizmoMath Utilities
WorldToScreen, ScreenToRay, RayPlaneIntersect, DistPointToSegmentSS — all correct and well-implemented inline functions
ScreenSpaceScale() correctly handles perspective projection via proj._22 focal length extraction
No issues found.
 
6. Code Quality / Miscellaneous
Memory management: VulkanBackend has ~50 VkDeviceMemory fields with individual allocations. A sub-allocator (VMA) would dramatically reduce the number of vkAllocateMemory calls and avoid hitting driver allocation limits on some hardware (4096 allocations on NVIDIA).
_deviceLost flag exists but recovery path quality would need testing under actual device-lost scenarios.
Thread safety: None of the systems use atomics or locks. This is fine for single-threaded rendering but limits future multi-threaded command recording.
ComPtr<> used in DX12 but raw handles everywhere in Vulkan — asymmetric lifetime management increases Vulkan leak risk.
 
Summary of Recommended Fixes (Priority Order)
Priority
Issue
Location
High
AddPass() returning reference to vector element → potential dangling reference
VulkanRenderGraph.h:41
Medium
Extract quaternion-to-Euler into shared utility (duplicated 3x)
TransformGizmo.cpp, LunaApp.cpp
Medium
Float equality in hit-test priority selection
TransformGizmo.cpp:261-275
Medium
using namespace DirectX in public HAL header
IRenderBackend.h:9
Low
Stacked bar doesn't respect sort order
GPUProfilerOverlay.cpp:89
Low
Gizmo rotation ignores localSpace flag
TransformGizmo.cpp:410-441
Low
VulkanBackend god-object decomposition
VulkanBackend.h
Low
Consider VMA for Vulkan memory management
VulkanBackend.cpp
The codebase is solid for a portfolio project. The core rendering pipeline is correct, the dual-backend parity is impressive, and the phased development approach is well-documented. The main risk areas are the Vulkan render graph's dangling reference and the growing complexity of the backend monolith.