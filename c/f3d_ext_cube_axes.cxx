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
 * The look is selected by F3D_EXT_CUBE_AXES_* flags. The minimal default is the
 * cube edges + X/Y tick labels + a semi-transparent bottom floor plane; the wall
 * gridlines (DrawX/Y/ZGridlines) and the Z elevation labels are opt-in.
 *
 * BUILD: same c_api target as the other f3d_ext sources; vtkCubeAxesActor is in
 * VTK RenderingAnnotation (already linked for the coordinate readout).
 */

#include "f3d_ext.h"

#include "window.h"      // f3d::window (public)
#include "window_impl.h" // f3d::detail::window_impl (PRIVATE)

#include "vtkF3DMetaImporter.h" // imported-geometry actors (PRIVATE vtkext module)
#include "vtkF3DRenderer.h"     // f3d's concrete renderer (PRIVATE vtkext module)

#include <vtkActor.h>
#include <vtkActorCollection.h>
#include <vtkBoundingBox.h>
#include <vtkCamera.h>
#include <vtkCubeAxesActor.h>
#include <vtkMapper.h>
#include <vtkNew.h>
#include <vtkPlaneSource.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
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

// Add an actor's (transformed) bounds to bbox if they are valid.
void addActorBounds(vtkBoundingBox& bbox, vtkActor* a)
{
  if (!a || !a->GetMapper())
  {
    return;
  }
  double b[6];
  a->GetBounds(b);
  if (b[1] >= b[0]) // valid/non-empty
  {
    bbox.AddBounds(b);
  }
}

// Bounds of the IMPORTED GEOMETRY only — exactly the data extent. Uses the f3d
// meta-importer's geometry actors, so it excludes f3d's own helper props (floor
// grid, skybox, orientation gizmo) AND our cube-axes/floor overlays that share
// the renderer; those would inflate a plain GetActors() union. GetBounds() on the
// rendered actors already reflects any actor transform (e.g. render.model_scale),
// so the cube matches the data as drawn. Falls back to a filtered actor union if
// the renderer is not a vtkF3DRenderer or has no importer.
bool dataBounds(vtkRenderer* ren, vtkCubeAxesActor* skip, double out[6])
{
  vtkBoundingBox bbox;

  if (vtkF3DRenderer* fren = vtkF3DRenderer::SafeDownCast(ren))
  {
    if (vtkF3DMetaImporter* imp = fren->GetMetaImporter())
    {
      for (const auto& cs : imp->GetColoringActorsAndMappers())
      {
        addActorBounds(bbox, cs.Actor);
      }
      for (const auto& ps : imp->GetPointSpritesActorsAndMappers())
      {
        addActorBounds(bbox, ps.Actor);
      }
    }
  }

  if (!bbox.IsValid())
  {
    // Fallback: union the renderer's data actors, skipping our own cube axes.
    vtkActorCollection* actors = ren->GetActors();
    if (actors)
    {
      actors->InitTraversal();
      vtkActor* a = nullptr;
      while ((a = actors->GetNextActor()))
      {
        if (reinterpret_cast<vtkProp*>(a) == reinterpret_cast<vtkProp*>(skip))
        {
          continue;
        }
        addActorBounds(bbox, a);
      }
    }
  }

  if (!bbox.IsValid())
  {
    return false;
  }
  bbox.GetBounds(out);
  return true;
}

struct AxesCtx
{
  vtkRenderer* renderer = nullptr;
  vtkSmartPointer<vtkCubeAxesActor> axes;
  vtkSmartPointer<vtkActor> floor;
};

// A semi-transparent plane at z = zmin spanning the x/y data extent = "floor".
vtkSmartPointer<vtkActor> makeFloor(const double b[6])
{
  vtkNew<vtkPlaneSource> plane;
  plane->SetOrigin(b[0], b[2], b[4]);
  plane->SetPoint1(b[1], b[2], b[4]);
  plane->SetPoint2(b[0], b[3], b[4]);

  vtkNew<vtkPolyDataMapper> mapper;
  mapper->SetInputConnection(plane->GetOutputPort());

  vtkSmartPointer<vtkActor> actor = vtkSmartPointer<vtkActor>::New();
  actor->SetMapper(mapper);
  actor->GetProperty()->SetColor(0.8, 0.8, 0.8);
  actor->GetProperty()->SetOpacity(0.25);
  actor->GetProperty()->LightingOff();
  actor->PickableOff();
  return actor;
}

std::map<f3d_window_t*, AxesCtx>& registry()
{
  static std::map<f3d_window_t*, AxesCtx> r;
  return r;
}
} // namespace

extern "C"
{

  int f3d_ext_enable_cube_axes(f3d_window_t* window, int flags)
  {
    if (flags == 0)
    {
      flags = F3D_EXT_CUBE_AXES_DEFAULT;
    }
    const bool wantEdges = (flags & F3D_EXT_CUBE_AXES_EDGES) != 0;
    const bool wantFloor = (flags & F3D_EXT_CUBE_AXES_FLOOR) != 0;
    const bool wantGrid = (flags & F3D_EXT_CUBE_AXES_GRID) != 0;
    const bool wantZLabels = (flags & F3D_EXT_CUBE_AXES_ZLABELS) != 0;

    vtkRenderer* ren = renderer_of(window);
    if (!ren || !ren->GetActiveCamera())
    {
      return 0;
    }

    AxesCtx& c = registry()[window];
    if (c.renderer && c.axes)
    {
      c.renderer->RemoveViewProp(c.axes); // re-enable: refresh
    }
    if (c.renderer && c.floor)
    {
      c.renderer->RemoveViewProp(c.floor);
    }

    // Bounds = exact union of the data actors (skips our own overlays). The
    // actor bounds are already model_scale-transformed, so the cube is tight.
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
    // HARD RULE: the Y-axis annotation must NEVER be drawn in a plane that does not
    // contain the X axis — X and Y must stay COPLANAR (same horizontal floor). OuterEdges
    // (the VTK default) picks each axis' edge independently, so X can land on z=zmin while
    // Y rides up to z=zmax -> different planes -> RULE VIOLATION. A TRIAD mode draws X, Y, Z
    // as the three edges meeting at ONE corner, so X and Y are always coplanar (Z vertical).
    // HARD RULES (user, firm): X and Y annotations MUST be coplanar AND on the BOTTOM
    // (z=zmin) floor, ALWAYS, regardless of camera. Only StaticTriad guarantees both: it
    // pins the three axes to the FIXED (xmin,ymin,zmin) bottom corner, so X and Y always
    // lie on the bottom floor (coplanar) and Z goes up — camera-independent (never flips
    // to the top like the camera-following Closest/Furthest triad modes, never splits the
    // planes like OuterEdges).
    axes->SetFlyModeToStaticTriad();
    axes->SetGridLineLocation(vtkCubeAxesActor::VTK_GRID_LINES_FURTHEST);

    // Walls = gridlines on every face. Off in the minimal default.
    if (wantGrid)
    {
      axes->DrawXGridlinesOn();
      axes->DrawYGridlinesOn();
      axes->DrawZGridlinesOn();
    }
    else
    {
      axes->DrawXGridlinesOff();
      axes->DrawYGridlinesOff();
      axes->DrawZGridlinesOff();
    }

    // Edges = the three labelled cube-axis edges. X/Y always carry tick labels in
    // the minimal look; Z (elevation) labels are opt-in. Without EDGES the actor
    // exists only to host the (optional) wall gridlines.
    axes->SetXAxisVisibility(wantEdges);
    axes->SetYAxisVisibility(wantEdges);
    axes->SetZAxisVisibility(wantEdges);
    axes->SetXAxisLabelVisibility(wantEdges);
    axes->SetYAxisLabelVisibility(wantEdges);
    axes->SetXAxisTickVisibility(wantEdges);
    axes->SetYAxisTickVisibility(wantEdges);
    const bool zlab = wantEdges && wantZLabels;
    axes->SetZAxisLabelVisibility(zlab);
    axes->SetZAxisTickVisibility(zlab);

    for (int i = 0; i < 3; ++i)
    {
      axes->GetTitleTextProperty(i)->SetColor(1.0, 1.0, 1.0);
      axes->GetLabelTextProperty(i)->SetColor(1.0, 1.0, 1.0);
    }
    ren->AddViewProp(axes);
    c.axes = axes;

    if (wantFloor)
    {
      vtkSmartPointer<vtkActor> floor = makeFloor(b);
      ren->AddViewProp(floor);
      c.floor = floor;
    }
    else
    {
      c.floor = nullptr;
    }

    c.renderer = ren;
    return 1;
  }

  void f3d_ext_disable_cube_axes(f3d_window_t* window)
  {
    auto it = registry().find(window);
    if (it != registry().end())
    {
      if (it->second.renderer)
      {
        if (it->second.axes)
        {
          it->second.renderer->RemoveViewProp(it->second.axes);
        }
        if (it->second.floor)
        {
          it->second.renderer->RemoveViewProp(it->second.floor);
        }
      }
      registry().erase(it);
    }
  }

} // extern "C"
