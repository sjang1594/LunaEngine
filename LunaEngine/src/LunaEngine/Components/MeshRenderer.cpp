#include "LunaPCH.h"
#include "MeshRenderer.h"
#include "Components/Transform.h"
#include "Renderer/Mesh.h"
#include "LunaEngine/Renderer/HAL/Public/IRenderContext.h"
#include "Logger/Logger.h"

namespace Luna
{

MeshRenderer::MeshRenderer() : Component(ComponentType::MESH_RENDERER) {}

MeshRenderer::~MeshRenderer() {}

void MeshRenderer::Render()
{
    if (!_mesh)
    {
        LUNA_LOG_WARN("MeshRenderer::Render() - _mesh is null");
        return;
    }

    // Build model matrix from this object's Transform component
    auto transform = GetTransform();
    XMMATRIX  world = transform ? transform->GetWorldMatrix() : XMMatrixIdentity();
    XMFLOAT4X4 model;
    XMStoreFloat4x4(&model, world);

    IRenderContext::DrawMesh(_mesh.get(), model);
}

} // namespace Luna
