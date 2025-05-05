#include "LunaPCH.h"
#include "IRenderContext.h"
#include "LunaEngine/Renderer/DX12/public/DX12Backend.h"
#include "LunaEngine/Renderer/Vulkan/VulkanBackend.h"

namespace Luna
{
std::unique_ptr<IRenderBackend> IRenderContext::s_Backend = nullptr;

RenderBackendType IRenderContext::s_BackendType = RenderBackendType::DirectX12;

void IRenderContext::Initialize(
    RenderBackendType backendType,
    void *windowHandler,
    uint32_t width, uint32_t height)
{
    s_BackendType = backendType;
    switch (backendType)
    {
    case RenderBackendType::Vulkan:
        s_Backend = std::make_unique<VulkanBackend>();
        break;

    case RenderBackendType::DirectX12:
        s_Backend = std::make_unique<DX12Backend>();
        break;
    }

    s_Backend->Init(windowHandler, width, height);
}

void IRenderContext::Shutdown()
{
    s_Backend.reset();
}

void IRenderContext::BeginFrame()
{
    if (s_Backend)
        s_Backend->BeginFrame();
}

void IRenderContext::DrawFrame()
{
    if (s_Backend)
        s_Backend->DrawFrame();
}

void IRenderContext::EndFrame()
{
    if (s_Backend)
        s_Backend->EndFrame();
}

void IRenderContext::Resize(uint32_t width, uint32_t height)
{
    if (s_Backend)
        s_Backend->Resize(width, height);
}

void IRenderContext::InitImGui(void *windowHandler)
{
    if (s_Backend)
        s_Backend->InitImGui(windowHandler);
}

void IRenderContext::StartImGuiFrame()
{
    if (s_Backend)
        s_Backend->StartImGui();
}

void IRenderContext::RenderImGui()
{
    if (s_Backend)
        s_Backend->RenderImGui();
}

void IRenderContext::ShutdownImGui()
{
    if (s_Backend)
        s_Backend->ShutdownImGui();
}

IRenderBackend *IRenderContext::GetBackend()
{
    return s_Backend.get();
}

RenderBackendType IRenderContext::GetCurrentBackendType()
{
    return s_BackendType;
}
} // namespace Luna