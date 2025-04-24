#pragma once 

#include "IRenderBackend.h"
#include <memory>

namespace Luna {

    enum class RenderBackendType {
        Vulkan,
        DirectX12
    };

    class RenderContext
    {
    public:
        static void Initialize(RenderBackendType backendType, void* windowHandler);
        static void Shutdown();
        static void BeginFrame();
        static void DrawFrame();
        static void EndFrame();
        static void Resize(uint32_t width, uint32_t height);

        static IRenderBackend* GetBackend();
        static RenderBackendType GetCurrentBackendType();

    private:
        static std::unique_ptr<IRenderBackend> s_Backend;
        static RenderBackendType s_BackendType;
    };
}