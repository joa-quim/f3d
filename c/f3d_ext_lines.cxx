/**
 * f3d_ext_lines.cxx — polyline overlays for f3d_ext.
 *
 * libf3d's public mesh API (f3d_scene_add_mesh / mesh_t) only builds polygon
 * cells (SetPolys), so it cannot draw lines. This adds a vtkPolyData of line
 * cells + a flat-shaded vtkActor straight onto the renderer through the f3d_ext
 * hatch — the same route the cube axes / colorbar use — so callers can draw
 * coastlines, tracks, contours, etc. ON TOP of a surface or image.
 *
 * Each f3d_ext_add_lines call returns an id; a per-window registry keeps the
 * actors so they can be removed one-by-one (remove_lines) or all at once
 * (clear_lines). Lines may be a single colour (rgb) or per-vertex coloured
 * (vert_rgb, for colour-by-value). `overlay` pulls the lines toward the camera
 * with a polygon offset so coplanar lines on a surface are not lost to z-fight.
 *
 * BUILD: same c_api target as the other f3d_ext sources; vtkPolyDataMapper /
 * vtkCellArray are in VTK RenderingCore / CommonDataModel (already linked).
 */

#include "f3d_ext.h"

#include "window.h"      // f3d::window (public)
#include "window_impl.h" // f3d::detail::window_impl (PRIVATE)

#include <vtkActor.h>
#include <vtkCellArray.h>
#include <vtkNew.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkRendererCollection.h>
#include <vtkSmartPointer.h>
#include <vtkUnsignedCharArray.h>

#include <algorithm>
#include <map>

namespace
{
f3d::detail::window_impl* impl_of(f3d_window_t* window)
{
  if (!window)
  {
    return nullptr;
  }
  return static_cast<f3d::detail::window_impl*>(reinterpret_cast<f3d::window*>(window));
}

vtkRenderer* renderer_of(f3d_window_t* window)
{
  f3d::detail::window_impl* impl = impl_of(window);
  if (!impl || !impl->GetRenderWindow())
  {
    return nullptr;
  }
  vtkRendererCollection* rens = impl->GetRenderWindow()->GetRenderers();
  return rens ? rens->GetFirstRenderer() : nullptr;
}

struct LineCtx
{
  vtkRenderer* renderer = nullptr;
  int counter = 0;
  std::map<int, vtkSmartPointer<vtkActor>> actors;
};

std::map<f3d_window_t*, LineCtx>& registry()
{
  static std::map<f3d_window_t*, LineCtx> r;
  return r;
}
} // namespace

extern "C"
{

  int f3d_ext_add_lines(f3d_window_t* window, const double* points, size_t n_points,
    const unsigned int* line_sizes, size_t n_lines, const double* rgb,
    const unsigned char* vert_rgb, double width, int overlay)
  {
    vtkRenderer* ren = renderer_of(window);
    if (!ren || !points || n_points == 0)
    {
      return 0;
    }

    // Points.
    vtkNew<vtkPoints> pts;
    pts->SetNumberOfPoints(static_cast<vtkIdType>(n_points));
    for (size_t i = 0; i < n_points; ++i)
    {
      pts->SetPoint(static_cast<vtkIdType>(i), points[3 * i], points[3 * i + 1], points[3 * i + 2]);
    }

    // Line cells: either the caller's polyline breakdown, or one big polyline.
    vtkNew<vtkCellArray> lines;
    const bool haveSizes = line_sizes && n_lines > 0;
    const size_t nl = haveSizes ? n_lines : 1;
    size_t off = 0;
    for (size_t l = 0; l < nl; ++l)
    {
      const size_t s = haveSizes ? line_sizes[l] : n_points;
      if (s < 2 || off + s > n_points)
      {
        off += s; // skip a degenerate / out-of-range run but keep the offset consistent
        continue;
      }
      lines->InsertNextCell(static_cast<vtkIdType>(s));
      for (size_t k = 0; k < s; ++k)
      {
        lines->InsertCellPoint(static_cast<vtkIdType>(off++));
      }
    }
    if (lines->GetNumberOfCells() == 0)
    {
      return 0;
    }

    vtkNew<vtkPolyData> pd;
    pd->SetPoints(pts);
    pd->SetLines(lines);

    vtkNew<vtkPolyDataMapper> mapper;
    mapper->SetInputData(pd);

    // Per-vertex colour (overrides the single colour) for colour-by-value lines.
    if (vert_rgb)
    {
      vtkNew<vtkUnsignedCharArray> colors;
      colors->SetNumberOfComponents(3);
      colors->SetNumberOfTuples(static_cast<vtkIdType>(n_points));
      std::copy(vert_rgb, vert_rgb + 3 * n_points, colors->Begin());
      pd->GetPointData()->SetScalars(colors);
      mapper->SetScalarModeToUsePointData();
      mapper->ScalarVisibilityOn();
    }
    else
    {
      mapper->ScalarVisibilityOff();
    }

    // overlay: pull lines toward the camera so coplanar lines on a surface are not
    // lost to z-fighting / draw on top.
    if (overlay)
    {
      mapper->SetResolveCoincidentTopologyToPolygonOffset();
      mapper->SetRelativeCoincidentTopologyLineOffsetParameters(-1.0, -1.0);
    }

    vtkSmartPointer<vtkActor> actor = vtkSmartPointer<vtkActor>::New();
    actor->SetMapper(mapper);
    actor->GetProperty()->SetLineWidth(width > 0.0 ? width : 1.0);
    if (!vert_rgb)
    {
      const double def[3] = { 1.0, 1.0, 0.0 }; // yellow
      const double* c = rgb ? rgb : def;
      actor->GetProperty()->SetColor(c[0], c[1], c[2]);
    }
    actor->GetProperty()->LightingOff(); // flat, full-strength colour (no shading)
    actor->PickableOff();
    ren->AddViewProp(actor);

    LineCtx& ctx = registry()[window];
    ctx.renderer = ren;
    const int id = ++ctx.counter;
    ctx.actors[id] = actor;
    return id;
  }

  void f3d_ext_remove_lines(f3d_window_t* window, int id)
  {
    auto it = registry().find(window);
    if (it == registry().end())
    {
      return;
    }
    auto ait = it->second.actors.find(id);
    if (ait != it->second.actors.end())
    {
      // Remove via the CURRENT renderer, never the stored one (a freed/reused window
      // pointer would leave it->second.renderer dangling -> use-after-free).
      if (vtkRenderer* ren = renderer_of(window))
      {
        ren->RemoveViewProp(ait->second);
      }
      it->second.actors.erase(ait);
    }
  }

  void f3d_ext_clear_lines(f3d_window_t* window)
  {
    auto it = registry().find(window);
    if (it == registry().end())
    {
      return;
    }
    if (vtkRenderer* ren = renderer_of(window))
    {
      for (auto& kv : it->second.actors)
      {
        ren->RemoveViewProp(kv.second);
      }
    }
    registry().erase(it);
  }

} // extern "C"
