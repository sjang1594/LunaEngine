#pragma once

#include <LunaEngine/LunaPCH.h>
#include <LunaEngine/Renderer/HAL/Public/IRenderBackend.h>

namespace Luna
{
class VulkanDevice;
class VulkanBackend : public IRenderBackend
{
  public:
    VulkanBackend();
    virtual ~VulkanBackend() override;

    bool Init(void *windowHandler, uint32_t width, uint32_t height) override;
    void Shutdown() override;
    
    void BeginFrame() override;
    void DrawFrame() override;
    void EndFrame() override;
    
    void InitImGui(void *windowHandler) override;
    void StartImGui() override;
    void RenderImGui() override;
    void ShutdownImGui() override;
    void Resize(uint32_t width, uint32_t height) override;

    void Draw(uint32_t vertexCount) override;
    void SetVertexBuffer(class IBuffer *buffer) override;
    void BindPipeline(class IPipeline *pipeline) override;

    const char *GetBackendName() const override { return "Vulkan"; }
    
  private:
    void SetupImGui();
    void SetupVulkanWindow(ImGui_ImplVulkanH_Window* wd, VkSurfaceKHR surface, int width, int height);
    bool CreateInstance();
    bool CreateSurface(void *windowHandle);

    ImGui_ImplVulkanH_Window        _windowData;
    VkInstance                      _instance;
    VkSurfaceKHR                    _surface;
    std::unique_ptr<VulkanDevice>   _device;
    VkDescriptorPool                _imguiDescriptorPool;
    int                             _minImageCount = 2;
};
} // namespace Luna