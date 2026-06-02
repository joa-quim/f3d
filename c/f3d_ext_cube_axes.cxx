/**
 * f3d_ext_cube_axes.cxx — labelled bounding-box axes (gap #2) for f3d_ext.
 *
 * Adds a vtkCubeAxesActor around the data, giving numbered/labelled X/Y/Z tick
 * axes (lon/lat/elevation style) that the stock libf3d cannot do — the built-in
 * `render.axes_grid` option crashes this build, and `ui.axis` is only an
 * orientation gizmo. Reached through the f3d_ext renderer hatch. The bounds are
 * captured from the data actors at enable time (re-enable to refresh after the
 * geometry or scale changes).
 *
 * BUILD: same c_api target as the other f3d_ext sources; vtkCubeAxesActor is in
 * VTK RenderingAnnotation (already linked for the coordinate readout).
 */

#include "f3d_ext.h"

#include "window.h"      // f3d::window (public)
#include "window_impl.h" // f3d::detail::window_impl (PRIVATE)

#include <vtkActor.h>
#include <vtkActorCollection.h>
#include <vtkCamera.h>
#include <vtkCubeAxesActor.h>
#include <vtkMapper.h>
#include <vtkNew.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkRenderer.h>
#include <vtkRendererCollection.h>
#include <vtkSmartPointer.h>
#include <vtkTextProperty.h>

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

// Union of the bounds of the pickable data actors (skips the cube axes / overlays).
bool dataBounds(vtkRenderer* ren, vtkCubeAxesActor* skip, double out[6])
{
  bool any = false;
  out[0] = out[2] = out[4] = 1e300;
  out[1] = out[3] = out[5] = -1e300;
  vtkActorCollection* actors = ren->GetActors();
  if (!actors)
  {
    return false;
  }
  actors->InitTraversal();
  vtkActor* a = nullptr;
  while ((a = actors->GetNextActor()))
  {
    if (reinterpret_cast<vtkProp*>(a) == reinterpret_cast<vtkProp*>(skip) || !a->GetMapper())
    {
      continue;
    }
    double b[6];
    a->GetBounds(b);
    if (b[1] < b[0])
    {
      continue; // invalid/empty
    }
    out[0] = std::min(out[0], b[0]);
    out[1] = std::max(out[1], b[1]);
    out[2] = std::min(out[2], b[2]);
    out[3] = std::max(out[3], b[3]);
    out[4] = std::min(out[4], b[4]);
    out[5] = std::max(out[5], b[5]);
    any = true;
  }
  return any;
}

struct AxesCtx
{
  vtkRenderer* renderer = nullptr;
  vtkSmartPointer<vtkCubeAxesActor> axes;
};

std::map<f3d_window_t*, AxesCtx>& registry()
{
  static std::map<f3d_window_t*, AxesCtx> r;
  return r;
}
} // namespace

extern "C"
{

  int f3d_ext_enable_cube_axes(f3d_window_t* window)
  {
    vtkRenderer* ren = renderer_of(window);
    if (!ren || !ren->GetActiveCamera())
    {
      return 0;
    }

    AxesCtx& c = registry()[window];
    if (c.axes && c.renderer)
    {
      c.renderer->RemoveViewProp(c.axes); // re-enable: refresh
    }

    double b[6];
    if (!dataBounds(ren, c.axes, b))
    {
      return 0;
    }

    vtkNew<vtkCubeAxesActor> axes;
    axes->SetBounds(b);
    axes->SetCamera(ren->GetActiveCamera());
    axes->SetXTitle("X");
    axes->SetYTitle("Y");
    axes->SetZTitle("Z");
    axes->SetFlyModeToOuterEdges();
    axes->SetGridLineLocation(vtkCubeAxesActor::VTK_GRID_LINES_FURTHEST);
    axes->DrawXGridlinesOn();
    axes->DrawYGridlinesOn();
    axes->DrawZGridlinesOn();
    for (int i = 0; i < 3; ++i)
    {
      axes->GetTitleTextProperty(i)->SetColor(1.0, 1.0, 1.0);
      axes->GetLabelTextProperty(i)->SetColor(1.0, 1.0, 1.0);
    }
    ren->AddViewProp(axes);

    c.renderer = ren;
    c.axes = axes;
    return 1;
  }

  void f3d_ext_disable_cube_axes(f3d_window_t* window)
  {
    auto it = registry().find(window);
    if (it != registry().end())
    {
      if (it->second.renderer && it->second.axes)
      {
        it->second.renderer->RemoveViewProp(it->second.axes);
      }
      registry().erase(it);
    }
  }

} // extern "C"
