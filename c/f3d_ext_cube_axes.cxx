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
#include <vtkCoordinate.h>
#include <vtkCubeAxesActor.h>
#include <vtkLookupTable.h>
#include <vtkMapper.h>
#include <vtkNew.h>
#include <vtkPlaneSource.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkRenderer.h>
#include <vtkRendererCollection.h>
#include <vtkScalarBarActor.h>
#include <vtkScalarBarRepresentation.h>
#include <vtkScalarBarWidget.h>
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

vtkRenderWindowInteractor* interactor_of(f3d_window_t* window)
{
  f3d::detail::window_impl* impl = impl_of(window);
  if (!impl || !impl->GetRenderWindow())
  {
    return nullptr;
  }
  return impl->GetRenderWindow()->GetInteractor();
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

std::map<f3d_window_t*, vtkSmartPointer<vtkScalarBarActor>>& cbarRegistry()
{
  static std::map<f3d_window_t*, vtkSmartPointer<vtkScalarBarActor>> r;
  return r;
}

// When the bar is made draggable it is owned by a vtkScalarBarWidget (drag-move +
// corner-resize via its border representation) instead of being added as a plain prop.
std::map<f3d_window_t*, vtkSmartPointer<vtkScalarBarWidget>>& cbarWidgetRegistry()
{
  static std::map<f3d_window_t*, vtkSmartPointer<vtkScalarBarWidget>> r;
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
    // Remove a prior prop via the CURRENT renderer, never the stored one: if the window
    // pointer was freed and a new engine reused the address, c.renderer dangles (a freed
    // renderer) -> use-after-free crash. RemoveViewProp of an absent prop is a safe no-op.
    if (c.axes)
    {
      ren->RemoveViewProp(c.axes); // re-enable: refresh
    }
    if (c.floor)
    {
      ren->RemoveViewProp(c.floor);
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

  int f3d_ext_enable_image_axes(f3d_window_t* window, const char* xfmt, const char* yfmt)
  {
    vtkRenderer* ren = renderer_of(window);
    if (!ren || !ren->GetActiveCamera())
    {
      return 0;
    }

    AxesCtx& c = registry()[window];
    if (c.axes)
    {
      ren->RemoveViewProp(c.axes); // re-enable: refresh (current renderer, never stored)
    }
    if (c.floor)
    {
      ren->RemoveViewProp(c.floor);
      c.floor = nullptr;
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
    // StaticTriad pins the axes to the fixed (xmin,ymin,zmin) corner: viewed
    // top-down with +Y up that is the bottom-left, so X runs along the bottom and
    // Y along the left edge — a 2-D map frame. The plane is flat (z extent 0).
    axes->SetFlyModeToStaticTriad();
    axes->DrawXGridlinesOff();
    axes->DrawYGridlinesOff();
    axes->DrawZGridlinesOff();

    // X (bottom) + Y (left) only; the Z axis is meaningless for a flat image.
    axes->SetXAxisVisibility(1);
    axes->SetYAxisVisibility(1);
    axes->SetZAxisVisibility(0);
    axes->SetXAxisLabelVisibility(1);
    axes->SetYAxisLabelVisibility(1);
    axes->SetZAxisLabelVisibility(0);
    axes->SetXAxisTickVisibility(1);
    axes->SetYAxisTickVisibility(1);
    axes->SetZAxisTickVisibility(0);
    axes->SetXAxisMinorTickVisibility(0);
    axes->SetYAxisMinorTickVisibility(0);

    // Ticks point OUTWARD, away from the figure (user preference).
    axes->SetTickLocationToOutside();

    // Caller-chosen decimal precision so adjacent labels stay unique.
    if (xfmt && xfmt[0])
    {
      axes->SetXLabelFormat(xfmt);
    }
    if (yfmt && yfmt[0])
    {
      axes->SetYLabelFormat(yfmt);
    }

    for (int i = 0; i < 3; ++i)
    {
      axes->GetTitleTextProperty(i)->SetColor(1.0, 1.0, 1.0);
      axes->GetLabelTextProperty(i)->SetColor(1.0, 1.0, 1.0);
    }
    ren->AddViewProp(axes);
    c.axes = axes;
    c.renderer = ren;
    return 1;
  }

  int f3d_ext_enable_colorbar(f3d_window_t* window, const unsigned char* rgb, int ncolors,
    double vmin, double vmax, const char* title, const char* fmt, int draggable)
  {
    vtkRenderer* ren = renderer_of(window);
    if (!ren || ncolors < 2 || !rgb)
    {
      return 0;
    }

    auto& reg = cbarRegistry();
    auto it = reg.find(window);
    if (it != reg.end() && it->second)
    {
      ren->RemoveViewProp(it->second); // re-enable: refresh
    }
    // Tear down any prior widget so re-enabling does not leak/stack observers.
    auto& wreg = cbarWidgetRegistry();
    auto wit = wreg.find(window);
    if (wit != wreg.end())
    {
      if (wit->second)
      {
        wit->second->SetEnabled(0);
      }
      wreg.erase(wit);
    }

    // Build a lookup table from the (ordered) RGB palette spanning [vmin, vmax].
    vtkNew<vtkLookupTable> lut;
    lut->SetNumberOfTableValues(ncolors);
    lut->SetTableRange(vmin, vmax);
    for (int i = 0; i < ncolors; ++i)
    {
      lut->SetTableValue(i, rgb[3 * i] / 255.0, rgb[3 * i + 1] / 255.0, rgb[3 * i + 2] / 255.0, 1.0);
    }
    lut->Build();

    vtkSmartPointer<vtkScalarBarActor> bar = vtkSmartPointer<vtkScalarBarActor>::New();
    bar->SetLookupTable(lut);
    if (title && title[0])
    {
      bar->SetTitle(title);
    }
    bar->SetNumberOfLabels(5);
    if (fmt && fmt[0])
    {
      bar->SetLabelFormat(fmt);
    }
    bar->SetOrientationToVertical();
    bar->GetPositionCoordinate()->SetCoordinateSystemToNormalizedViewport();
    bar->GetPositionCoordinate()->SetValue(0.90, 0.30);
    bar->SetWidth(0.05);   // slimmer + shorter than the old 0.08 x 0.76 (was too big)
    bar->SetHeight(0.45);
    // Fixed, readable label/title font. Without UnconstrainedFontSize the actor scales
    // the text to fit the (now small) box -> the labels shrink to near-invisible.
    bar->SetUnconstrainedFontSize(true);
    bar->GetTitleTextProperty()->SetColor(1.0, 1.0, 1.0);
    bar->GetTitleTextProperty()->SetFontSize(26);
    bar->GetTitleTextProperty()->BoldOn();
    bar->GetTitleTextProperty()->ShadowOff();
    bar->GetLabelTextProperty()->SetColor(1.0, 1.0, 1.0);
    bar->GetLabelTextProperty()->SetFontSize(26);
    bar->GetLabelTextProperty()->ShadowOff();

    reg[window] = bar;

    // Draggable: hand the bar to a vtkScalarBarWidget bound to the interactor. The widget
    // adds the bar to the renderer through its representation and handles move/resize via a
    // selectable border, so we do NOT AddViewProp it ourselves. Falls back to a static prop
    // when there is no interactor.
    vtkRenderWindowInteractor* rwi = interactor_of(window);
    if (draggable && rwi)
    {
      vtkSmartPointer<vtkScalarBarWidget> widget = vtkSmartPointer<vtkScalarBarWidget>::New();
      widget->SetInteractor(rwi);
      widget->SetScalarBarActor(bar);
      if (vtkScalarBarRepresentation* srep =
            vtkScalarBarRepresentation::SafeDownCast(widget->GetRepresentation()))
      {
        // Seed the widget rectangle from the bar's current viewport placement.
        srep->GetPositionCoordinate()->SetValue(0.90, 0.30);
        srep->GetPosition2Coordinate()->SetValue(0.05, 0.45);
      }
      widget->SetEnabled(1);
      wreg[window] = widget;
    }
    else
    {
      ren->AddViewProp(bar);
    }
    return 1;
  }

  void f3d_ext_disable_colorbar(f3d_window_t* window)
  {
    auto& wreg = cbarWidgetRegistry();
    auto wit = wreg.find(window);
    if (wit != wreg.end())
    {
      if (wit->second)
      {
        wit->second->SetEnabled(0); // detaches the bar from the renderer
      }
      wreg.erase(wit);
    }

    auto& reg = cbarRegistry();
    auto it = reg.find(window);
    if (it != reg.end())
    {
      if (it->second)
      {
        if (vtkRenderer* ren = renderer_of(window))
        {
          ren->RemoveViewProp(it->second); // only added when NOT draggable; harmless otherwise
        }
      }
      reg.erase(it);
    }
  }

} // extern "C"
