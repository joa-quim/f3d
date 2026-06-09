#include "PseudoUnitTest.h"

#include <engine.h>
#include <image.h>
#include <log.h>
#include <mesh_view.h>
#include <scene.h>
#include <types.h>
#include <window.h>

#include <array>
#include <memory>
#include <vector>

// A minimal static zero-copy mesh (two triangles forming a tilted quad) carrying an
// overridable per-mesh transform, used to exercise f3d::mesh_view::getTransform and
// f3d::transform3d_t.
class TransformMesh : public f3d::mesh_view
{
public:
  explicit TransformMesh(const f3d::transform3d_t& transform)
    : Transform(transform)
  {
  }

  std::string getName() const override
  {
    return "transform_mesh";
  }

  f3d::transform3d_t getTransform(double) const override
  {
    return this->Transform;
  }

  f3d::mesh_view::memory_view_t getMemoryView(double) const override
  {
    return { .pointCount = this->Points.size() / 3,
      .points = { .data = this->Points.data(), .components = 3 },
      .polygons = { .offsetCount = this->FaceOffsets.size(),
        .offsets = { .type = f3d::mesh_view::data_type::I32, .data = this->FaceOffsets.data() },
        .indexCount = this->FaceIndices.size(),
        .indices = { .type = f3d::mesh_view::data_type::I32, .data = this->FaceIndices.data() } } };
  }

private:
  f3d::transform3d_t Transform;

  // Tilted quad: z rises along +y so the shape has genuine 3D relief and a non-square
  // silhouette, which a z- or x-scale visibly changes.
  std::vector<float> Points = {
    -1.f, -1.f, 0.f, 1.f, -1.f, 0.f, 1.f, 1.f, 1.f, -1.f, 1.f, 1.f };
  std::vector<unsigned int> FaceOffsets = { 0, 3, 6 };
  std::vector<unsigned int> FaceIndices = { 0, 1, 2, 0, 2, 3 };
};

int TestSDKMeshViewTransform([[maybe_unused]] int argc, [[maybe_unused]] char* argv[])
{
  PseudoUnitTest test;

  f3d::log::setVerboseLevel(f3d::log::VerboseLevel::DEBUG);

  // A default-constructed f3d::transform3d_t is the identity (no transform).
  const f3d::transform3d_t identity;
  const std::array<double, 16> identityValues = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
  bool defaultIsIdentity = true;
  for (size_t i = 0; i < 16; ++i)
  {
    defaultIsIdentity = defaultIsIdentity && (identity[i] == identityValues[i]);
  }
  test("default transform3d_t is the identity matrix", defaultIsIdentity);

  f3d::engine eng = f3d::engine::create(true); // offscreen
  f3d::scene& sce = eng.getScene();
  f3d::window& win = eng.getWindow().setSize(300, 300);

  // Reference render: mesh with the default (identity) transform.
  test("add mesh with identity transform",
    [&]() { sce.add(std::make_shared<TransformMesh>(identity)); });
  const f3d::image imgIdentity = win.renderToImage();

  // An explicitly supplied identity matrix must render identically to the default: the
  // identity is recognized as "no transform".
  test("clear scene", [&]() { sce.clear(); });
  const f3d::transform3d_t explicitIdentity{ 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
  test("add mesh with explicit identity transform",
    [&]() { sce.add(std::make_shared<TransformMesh>(explicitIdentity)); });
  test("explicit identity transform renders like the default", win.renderToImage() == imgIdentity);

  // A non-identity, anisotropic-scale transform must change the render. Scaling x and z but
  // not y changes the silhouette aspect ratio, which the camera-to-bounds fit preserves, so
  // the resulting image must differ from the identity one.
  test("clear scene again", [&]() { sce.clear(); });
  const f3d::transform3d_t scaled{ 2, 0, 0, 0, 0, 1, 0, 0, 0, 0, 5, 0, 0, 0, 0, 1 };
  test("add mesh with anisotropic-scale transform",
    [&]() { sce.add(std::make_shared<TransformMesh>(scaled)); });
  test("anisotropic-scale transform changes the render", win.renderToImage() != imgIdentity);

  return test.result();
}
