#pragma once
#include "Components/Component.h"

namespace Luna
{
struct Mesh;  // defined in Renderer/Mesh.h

class MeshRenderer : public Component
{
public:
    MeshRenderer();
    virtual ~MeshRenderer();

    virtual void Update() override { Render(); }
    void SetMesh(const shared_ptr<Mesh>& mesh) { _mesh = mesh; }

    void Render();

private:
    shared_ptr<Mesh> _mesh;
};
}
