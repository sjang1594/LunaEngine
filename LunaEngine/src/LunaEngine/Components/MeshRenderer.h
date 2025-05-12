#pragma once
#include "Components/Component.h"
class Mesh;
class Material;
namespace Luna
{

class MeshRenderer : Component
{
public:
    MeshRenderer();
    virtual ~MeshRenderer();
    virtual void Update() override { Render(); }
    void SetMesh(const shared_ptr<Mesh> &mesh) { _mesh = mesh; }
    void SetMaterial(const shared_ptr<Material> &material) { _mat = material; }

    void Render();
private:
    shared_ptr<Mesh> _mesh;
    shared_ptr<Material> _mat;
};
}