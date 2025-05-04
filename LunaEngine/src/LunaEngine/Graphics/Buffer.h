#pragma once

// TRIANGLE TEST
struct Vertex {
  float position[3];
  float color[3];
};

static Vertex verticies[] = {
  {{0.0f, 0.5f, 0.0f}, {1.0f, 0.0f, 0.0f} }, // TOP (RED)
  {{0.5f, -0.5f, 0.0f}, {0.0f, 1.0f, 0.0f} }, // Right (Green)
  {{-0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f} } // Left (Blue)
};

namespace Luna
{
  enum class BufferUsage
  {
    Vertex,
    Index,
    Uniform
  };

  class Buffer
  {
  public:
    virtual ~Buffer() = default;
    virtual void Bind() = 0;
  };

  class VertexBuffer: public Buffer
  {
  public:
    static shared_ptr<VertexBuffer> Create(void* data, uint32_t size);
  };
}