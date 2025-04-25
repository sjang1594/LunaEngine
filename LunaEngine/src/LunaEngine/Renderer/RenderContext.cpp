#include "LunaPCH.h"
#include "RenderContext.h"
#include "LunaEngine/Renderer/Vulkan/VulkanBackend.h"
#include "LunaEngine/Renderer/DX12/DX12Backend.h"

namespace Luna {
    std::unique_ptr<IRenderBackend> RenderContext::s_Backend = nullptr;
    RenderBackendType RenderContext::s_BackendType = RenderBackendType::DirectX12;

    void RenderContext::Initialize(RenderBackendType backendType,
                                   void* windowHandler, uint32_t width,
                                   uint32_t height) {
      s_BackendType = backendType;
      switch (backendType) {
        case RenderBackendType::Vulkan:
          s_Backend = std::make_unique<VulkanBackend>();
          break;

        case RenderBackendType::DirectX12:
          s_Backend = std::make_unique<DX12Backend>();
          break;
      }

      s_Backend->Init(windowHandler, width, height);
    }

    void RenderContext::Shutdown()
    {
        s_Backend.reset();
    }

    void RenderContext::BeginFrame()
    {
        if (s_Backend) s_Backend->BeginFrame();
    }

    void RenderContext::DrawFrame()
    {
        if (s_Backend) s_Backend->DrawFrame();
    }

    void RenderContext::EndFrame()
    {
        if (s_Backend) s_Backend->EndFrame();
    }

    void RenderContext::Resize(uint32_t width, uint32_t height)
    {
        if (s_Backend) s_Backend->Resize(width, height);
    }

    void RenderContext::InitImGui(void* windowHandler) 
    {
      if (s_Backend) s_Backend->InitImGui(windowHandler);
    }

    void RenderContext::StartImGuiFrame(){
        if (s_Backend) s_Backend->StartImGui();
    }

    void RenderContext::RenderImGui() 
    {
      if (s_Backend) s_Backend->RenderImGui();
    }

    void RenderContext::ShutdownImGui() {
      if (s_Backend) s_Backend->ShutdownImGui();
    }

    IRenderBackend* RenderContext::GetBackend()
    {
        return s_Backend.get();
    }

    RenderBackendType RenderContext::GetCurrentBackendType()
    {
        return s_BackendType;
    }
}