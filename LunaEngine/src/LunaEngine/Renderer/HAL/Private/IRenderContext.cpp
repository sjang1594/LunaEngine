#include "LunaPCH.h"
<<<<<<<< Updated upstream:LunaEngine/src/LunaEngine/Renderer/HAL/Private/IRenderContext.cpp
#include "LunaEngine/Renderer/HAL/Public/IRenderContext.h"

#include "Logger/Logger.h"
#include "LunaEngine/Renderer/DX12/Public/DX12Backend.h"
#include "LunaEngine/Renderer/Vulkan/Public/VulkanBackend.h"

namespace Luna
{
std::unique_ptr<IRenderBackend> IRenderContext::s_Backend = nullptr;
RenderBackendType IRenderContext::s_BackendType = RenderBackendType::Auto;

void IRenderContext::Initialize(RenderBackendType backendType, void *windowHandler,
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

    case RenderBackendType::VulkanMolt:
        LUNA_LOG_ERROR("MoltenVK backend is not supported yet");
        return;

    default:
        throw std::runtime_error("Unknown render backend type");
    }

    if (!s_Backend->Init(windowHandler, width, height))
    {
        LUNA_LOG_ERROR("Backend initialization failed");
        s_Backend.reset();
    }
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

void IRenderContext::Shutdown()
{
    if (s_Backend)
    {
        s_Backend->Shutdown();
        s_Backend.reset();
    }
}

void IRenderContext::Resize(uint32_t width, uint32_t height)
{
    if (s_Backend)
        s_Backend->Resize(width, height);
}

void IRenderContext::UpdateMVP(const XMFLOAT4X4& model, const XMFLOAT4X4& view,
                               const XMFLOAT4X4& proj)
{
    if (s_Backend)
        s_Backend->UpdateMVP(model, view, proj);
}

void IRenderContext::DrawMesh(const Mesh* mesh, const XMFLOAT4X4& model)
{
    if (s_Backend)
        s_Backend->DrawMesh(mesh, model);
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

void IRenderContext::SetVSync(bool vsync)
{
    if (s_Backend)
        s_Backend->SetVSync(vsync);
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
========
// Superseded by Renderer/HAL/Private/IRenderContext.cpp during the HAL refactor.
// This file is intentionally empty — do not add definitions here.
>>>>>>>> Stashed changes:LunaEngine/src/LunaEngine/Renderer/IRenderContext.cpp
