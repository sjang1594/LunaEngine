#include "LunaPCH.h"
#include "MeshRenderer.h"

namespace Luna
{
MeshRenderer::MeshRenderer() : Component(ComponentType::MESH_RENDERER) {}

MeshRenderer::~MeshRenderer() {}

void MeshRenderer::Render()
{
    // GetTransform()->Update()
    // _mat->Update();
    // _mesh->Render();
}
}