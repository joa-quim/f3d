/**
 * f3d_ext_pick.cxx — implementation of the f3d_ext area-pick + renderer escape hatch.
 *
 * BUILD (must compile inside the f3d tree so the private header resolves).
 * The C-API target in c/CMakeLists.txt is `c_api` (links `libf3d` PRIVATE, so VTK
 * is NOT transitively visible and must be added explicitly). Append to that file:
 *
 *     # 1. add the source + (optional) installed header
 *     list(APPEND F3D_C_SOURCE_FILES   ${CMAKE_CURRENT_SOURCE_DIR}/f3d_ext_pick.cxx)
 *     list(APPEND F3D_C_PUBLIC_HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/f3d_ext.h)
 *     #    (or, after add_library:  target_sources(c_api PRIVATE f3d_ext_pick.cxx) )
 *
 *     # 2. private headers (window_impl.h & its includes) + VTK modules used here
 *     target_include_directories(c_api PRIVATE ${CMAKE_SOURCE_DIR}/library/private)
 *     find_package(VTK COMPONENTS RenderingCore RenderingOpenGL2 CommonCore CommonDataModel)
 *     target_link_libraries(c_api PRIVATE
 *       VTK::RenderingCore VTK::RenderingOpenGL2 VTK::CommonCore VTK::CommonDataModel)
 *
 * F3D_EXPORT already resolves to dllexport here (c_api defines libf3d_EXPORTS), so
 * the f3d_ext_* symbols land in f3d_c_api.dll alongside the stock C API.
 */

#include "f3d_ext.h"

#include "window.h"        // f3d::window  (public)
#include "window_impl.h"   // f3d::detail::window_impl  (PRIVATE)

#include "vtkF3DMetaImporter.h" // imported-geometry actors (PRIVATE vtkext module)
#include "vtkF3DRenderer.h"     // f3d's concrete renderer (PRIVATE vtkext module)

#include <vtkActor.h>
#include <vtkActorCollection.h>
#include <vtkDataArray.h>
#include <vtkDataSet.h>
#include <vtkMapper.h>
#include <vtkMatrix4x4.h>
#include <vtkNew.h>
#include <vtkPlanes.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkRenderWindow.h>
#include <vtkRenderedAreaPicker.h>
#include <vtkRenderer.h>
#include <vtkRendererCollection.h>
#include <vtkSmartPointer.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace
{
// f3d_window_t* is a reinterpret_cast of f3d::window* (see engine_c_api.cxx). The
// concrete object engine creates is always a window_impl, so the downcast is safe.
f3d::detail::window_impl* impl_of(f3d_window_t* window)
{
  if (!window)
  {
    return nullptr;
  }
  f3d::window* win = reinterpret_cast<f3d::window*>(window);
  return static_cast<f3d::detail::window_impl*>(win);
}

vtkRenderer* renderer_of(f3d_window_t* window)
{
  f3d::detail::window_impl* impl = impl_of(window);
  if (!impl)
  {
    return nullptr;
  }
  vtkRenderWindow* rw = impl->GetRenderWindow();
  if (!rw)
  {
    return nullptr;
  }
  return rw->GetRenderers() ? rw->GetRenderers()->GetFirstRenderer() : nullptr;
}
} // namespace

extern "C"
{

  size_t* f3d_ext_area_pick_points(
    f3d_window_t* window, int x0, int y0, int x1, int y1, size_t* count)
  {
    if (count)
    {
      *count = 0;
    }
    vtkRenderer* ren = renderer_of(window);
    if (!ren || !count)
    {
      return nullptr;
    }

    const double xmin = std::min(x0, x1);
    const double ymin = std::min(y0, y1);
    const double xmax = std::max(x0, x1);
    const double ymax = std::max(y0, y1);

    // Build the view frustum for the dragged rectangle.
    vtkNew<vtkRenderedAreaPicker> picker;
    picker->AreaPick(xmin, ymin, xmax, ymax, ren);
    vtkPlanes* frustum = picker->GetFrustum();
    vtkPoints* fpts = frustum ? frustum->GetPoints() : nullptr;
    vtkDataArray* fnorm = frustum ? frustum->GetNormals() : nullptr;
    if (!frustum || !fpts || !fnorm)
    {
      return nullptr;
    }
    const int NP = frustum->GetNumberOfPlanes();

    // A point KNOWN to be inside the frustum: centroid of the 8 clip corners. We do the
    // half-space test ourselves rather than via vtkExtractSelectedFrustum because the
    // area-picker's plane normals do not match the outward-normal convention the extract
    // filter (and vtkPlanes::EvaluateFunction) assume — that flipped the result, so a
    // small box selected EVERY point and a big box selected none. Calibrating each plane's
    // "inside" sign against this interior point is convention-proof.
    double center[3] = { 0.0, 0.0, 0.0 };
    if (vtkPoints* clip = picker->GetClipPoints())
    {
      const vtkIdType nc = clip->GetNumberOfPoints();
      for (vtkIdType i = 0; i < nc; ++i)
      {
        double q[3];
        clip->GetPoint(i, q);
        center[0] += q[0]; center[1] += q[1]; center[2] += q[2];
      }
      if (nc > 0) { center[0] /= nc; center[1] /= nc; center[2] /= nc; }
    }
    std::vector<double> PX(NP), PY(NP), PZ(NP), NX(NP), NY(NP), NZ(NP), CS(NP);
    for (int k = 0; k < NP; ++k)
    {
      double p[3], n[3];
      fpts->GetPoint(k, p);
      fnorm->GetTuple(k, n);
      PX[k] = p[0]; PY[k] = p[1]; PZ[k] = p[2];
      NX[k] = n[0]; NY[k] = n[1]; NZ[k] = n[2];
      CS[k] = n[0] * (center[0] - p[0]) + n[1] * (center[1] - p[1]) + n[2] * (center[2] - p[2]);
    }
    // A world point is inside iff, for every plane, it is on the same side as `center`.
    auto inside = [&](const double w[3]) {
      for (int k = 0; k < NP; ++k)
      {
        const double v = NX[k] * (w[0] - PX[k]) + NY[k] * (w[1] - PY[k]) + NZ[k] * (w[2] - PZ[k]);
        if (v * CS[k] < 0.0)
        {
          return false;
        }
      }
      return true;
    };

    // Test only the IMPORTED geometry actors (the loaded mesh / point cloud), not f3d's
    // helper props or our own cube-axes / floor / highlight overlays — those would inject
    // bogus point indices. Points are in the actor's local space; map to world first.
    std::vector<size_t> ids;
    auto processActor = [&](vtkActor* actor) {
      if (!actor || !actor->GetMapper())
      {
        return;
      }
      vtkDataSet* ds = vtkDataSet::SafeDownCast(actor->GetMapper()->GetInput());
      if (!ds || ds->GetNumberOfPoints() == 0)
      {
        return;
      }
      vtkMatrix4x4* M = actor->GetMatrix();
      const vtkIdType n = ds->GetNumberOfPoints();
      for (vtkIdType i = 0; i < n; ++i)
      {
        double p[3];
        ds->GetPoint(i, p);
        double in[4] = { p[0], p[1], p[2], 1.0 }, w[4];
        M->MultiplyPoint(in, w);
        if (inside(w))
        {
          ids.push_back(static_cast<size_t>(i));
        }
      }
    };

    if (vtkF3DRenderer* fren = vtkF3DRenderer::SafeDownCast(ren))
    {
      if (vtkF3DMetaImporter* imp = fren->GetMetaImporter())
      {
        for (const auto& cs : imp->GetColoringActorsAndMappers())
        {
          processActor(cs.Actor);
        }
        for (const auto& ps : imp->GetPointSpritesActorsAndMappers())
        {
          processActor(ps.Actor);
        }
      }
    }

    if (ids.empty())
    {
      return nullptr;
    }
    // f3d keeps BOTH a coloring actor and a point-sprite actor for the same point
    // cloud, so a point's index lands in `ids` twice; dedup so the caller's toggle
    // logic doesn't insert-then-erase it back out (which selected nothing).
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());

    // Hand back a malloc'd array (freed by f3d_ext_free_ids -> free()).
    size_t* out = static_cast<size_t*>(std::malloc(ids.size() * sizeof(size_t)));
    if (!out)
    {
      return nullptr;
    }
    std::copy(ids.begin(), ids.end(), out);
    *count = ids.size();
    return out;
  }

  void f3d_ext_free_ids(size_t* ids)
  {
    std::free(ids);
  }

  void* f3d_ext_get_renderer(f3d_window_t* window)
  {
    return static_cast<void*>(renderer_of(window));
  }

  void* f3d_ext_get_render_window(f3d_window_t* window)
  {
    f3d::detail::window_impl* impl = impl_of(window);
    return impl ? static_cast<void*>(impl->GetRenderWindow()) : nullptr;
  }

} // extern "C"
