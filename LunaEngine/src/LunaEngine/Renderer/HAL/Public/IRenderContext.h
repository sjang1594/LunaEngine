#pragma once

#include "IRenderBackend.h"
#include <memory>
#include <cstdint>

enum class RenderBackendType
{
    Auto,
    Vulkan,
    DirectX12,
    VulkanMolt // USE MoltenVK 
};

namespace Luna
{
struct Mesh;  // forward-declared — callers that pass a Mesh* must include Renderer/Mesh.h

class IRenderContext
{
  public:
    static void Initialize(RenderBackendType backendType, void *windowHandler, uint32_t width,
                           uint32_t height);
    static void Shutdown();
    static void BeginFrame();
    static void DrawFrame();
    static void EndFrame();
    static void Resize(uint32_t width, uint32_t height);
    static void UpdateMVP(const XMFLOAT4X4& model, const XMFLOAT4X4& view,
                          const XMFLOAT4X4& proj);

    /* Records a mesh draw into the open command list (call between BeginFrame / EndFrame) */
    static void DrawMesh(const Mesh* mesh, const XMFLOAT4X4& model);

    /* IMGUI Control */
    static void InitImGui(void *windowHandler);
    static void StartImGuiFrame();
    static void RenderImGui();
    static void ShutdownImGui();

    /* Vsync — delegates to backend::SetVSync(); call after Initialize() */
    static void SetVSync(bool vsync);

    static IRenderBackend *GetBackend();
    static RenderBackendType GetCurrentBackendType();

  private:
    static std::unique_ptr<IRenderBackend> s_Backend;
    static RenderBackendType s_BackendType;
};
} // namespace Luna