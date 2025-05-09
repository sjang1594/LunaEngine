#include "LunaPCH.h"
#include "VulkanBackend.h"

Luna::VulkanBackend::VulkanBackend()
{
}

Luna::VulkanBackend::~VulkanBackend()
{
}

bool Luna::VulkanBackend::Init(void *windowHandler, uint32_t width, uint32_t height)
{
    return false;
}

void Luna::VulkanBackend::BeginFrame()
{
}

void Luna::VulkanBackend::InitImGui(void *windowHandler)
{
}

void Luna::VulkanBackend::StartImGui()
{
}

void Luna::VulkanBackend::RenderImGui()
{
}

void Luna::VulkanBackend::DrawFrame()
{
}

void Luna::VulkanBackend::ShutdownImGui()
{
}

void Luna::VulkanBackend::EndFrame()
{
}

void Luna::VulkanBackend::Resize(uint32_t width, uint32_t height)
{
}

const char *Luna::VulkanBackend::GetBackendName() const
{
    return "Vulkan";
}

void Luna::VulkanBackend::Draw(uint32_t vertexCount){
}

void Luna::VulkanBackend::SetVertexBuffer(class IBuffer *buffer){
}

void Luna::VulkanBackend::BindPipeline(class IPipeline *pipeline){
}

void Luna::VulkanBackend::CreateInstanceAndAurface(void *windowHandle)
{
}