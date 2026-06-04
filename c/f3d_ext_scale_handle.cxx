/**
 * f3d_ext_scale_handle.cxx — Fledermaus-style interaction gizmo for f3d_ext.
 *
 * A floating widget pinned to the camera FOCAL POINT (the rotation centre). Three
 * draggable handles:
 *   - vertical cone (arrowhead on a vertical axis) -> VERTICAL SCALE
 *       (render.model_scale z). The shaft stays a fixed length; the arrowhead cone
 *       stretches/shrinks to signal the current exaggeration.
 *   - two horizontal cones (left/right arrows)     -> TILT (camera Elevation about
 *       the horizontal screen axis through the focal point).
 *   - a flat compass ring (annulus) + N marker     -> AZIMUTH (camera Azimuth about
 *       the vertical axis through the focal point).
 * A billboard label on the axis shows the current vertical exaggeration.
 *
 * Mirrors the two established f3d_ext patterns:
 *  - drawing: actors added through the renderer hatch (like f3d_ext_cube_axes), kept
 *    in a per-window registry, removed via the CURRENT renderer (never a stored one).
 *  - dragging: high-priority vtkCallbackCommand observers on the render-window
 *    interactor (like f3d_ext_drag_scale), aborting the event when a handle is grabbed
 *    so f3d's own rotate/pan does not also run. Vertical scale is driven through the
 *    render.model_scale OPTION + window->render() so it survives the per-render push.
 *
 * The gizmo follows the focal point and is sized from the camera distance (constant
 * apparent size) on every render via a StartEvent observer on the renderer.
 *
 * BUILD: same c_api target; needs VTK::FiltersSources (cone/line/cylinder sources).
 */

#include "f3d_ext.h"

#include "options.h"     // f3d::options (public)
#include "window.h"      // f3d::window (public)
#include "window_impl.h" // f3d::detail::window_impl (PRIVATE)

#include <vtkActor.h>
#include <vtkBillboardTextActor3D.h>
#include <vtkCallbackCommand.h>
#include <vtkCamera.h>
#include <vtkCommand.h>
#include <vtkConeSource.h>
#include <vtkCylinderSource.h>
#include <vtkLight.h>
#include <vtkLineSource.h>
#include <vtkMatrix4x4.h>
#include <vtkNew.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkRenderer.h>
#include <vtkRendererCollection.h>
#include <vtkSmartPointer.h>
#include <vtkTextProperty.h>
#include <vtkTransform.h>
#include <vtkTransformPolyDataFilter.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <vector>

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

enum class Grab
{
  None,
  VScale,
  Tilt,
  Azimuth
};

// Local-space layout (before placement translate/scale). Units are gizmo-relative.
// z=0 sits at the focal point (rotation centre); the body floats up at z=kBodyZ with a
// shaft dropping down to z=0, like the Fledermaus widget (controls float above the data,
// a line drops to the data point). Floating the controls keeps them visible AND pickable
// (vtkPropPicker cannot pick a prop occluded by the terrain).
constexpr double kBodyZ = 1.4;      // height of the floating control body above focal pt
constexpr double kConeR = 0.047;    // vertical arrowhead base radius
constexpr double kConeH0 = 0.11;    // vertical arrowhead height at scale 1
constexpr double kRingR = 0.41;     // ring radius (finger-ring band) — shared by both rings
constexpr double kRingH = 0.08;     // ring band width — shared by both rings
// Horizontal-axis length is pinned to the data bbox (world units): half the larger XY
// extent, so the axis reaches ~the data edge and stays there regardless of zoom. When
// the bounds are unknown, fall back to this multiple of the gizmo scale.
constexpr double kHaxisFallback = 3.10;
// The horizontal (tilt) axis sits at the mid level of the surface = the focal point
// (z=0 local), with a line from the centre out to its arrow tip, mirroring the vertical
// axis. The vertical shaft passes through the same origin, so the two axes connect there.
constexpr double kHaxisZ = 0.0;

struct GizmoCtx
{
  // drawing
  vtkRenderer* renderer = nullptr;
  vtkSmartPointer<vtkActor> shaft;        // vertical axis line (not a handle)
  vtkSmartPointer<vtkActor> vcone;        // VERTICAL SCALE handle (stretches)
  vtkSmartPointer<vtkConeSource> vconeSrc;
  vtkSmartPointer<vtkActor> shaftH;       // horizontal axis line (not a handle)
  vtkSmartPointer<vtkActor> harrow;       // TILT handle: ring at the horizontal tip
  vtkSmartPointer<vtkActor> ring;         // AZIMUTH handle
  vtkSmartPointer<vtkLineSource> shaftSrc;
  vtkSmartPointer<vtkLineSource> shaftHSrc; // horizontal axis line (world-coord points)
  vtkSmartPointer<vtkBillboardTextActor3D> label;
  vtkSmartPointer<vtkLight> light;        // fixed-direction light for the ring
  vtkSmartPointer<vtkCallbackCommand> placeCmd; // StartEvent: follow focal point
  unsigned long placeTag = 0;

  // dragging
  vtkRenderWindowInteractor* rwi = nullptr;
  f3d::window* window = nullptr;
  f3d::options* options = nullptr;
  vtkSmartPointer<vtkCallbackCommand> dragCmd;
  unsigned long dragTags[3] = { 0, 0, 0 };

  double sensitivity = 0.01; // vertical-scale exp factor per pixel
  double rotSpeed = 0.5;     // degrees per pixel for tilt/azimuth
  Grab grab = Grab::None;
  int lastX = 0, lastY = 0;
  int startY = 0;
  double startSz = 1.0;
  double curSz = 1.0;

  // current placement (world)
  double centre[3] = { 0, 0, 0 };
  double scale = 1.0;
  double right[3] = { 1, 0, 0 }; // camera screen-right (horizontal axis direction)
  double haxisLen = 0.0;         // world half-extent of the data (axis length); 0 = unknown
};

std::map<f3d_window_t*, GizmoCtx>& registry()
{
  static std::map<f3d_window_t*, GizmoCtx> r;
  return r;
}

vtkSmartPointer<vtkActor> makeActor(vtkPolyDataAlgorithm* src, double r, double g, double b)
{
  vtkNew<vtkPolyDataMapper> m;
  m->SetInputConnection(src->GetOutputPort());
  // Draw the gizmo ON TOP of the surface: a large negative polygon/line depth offset
  // pulls it toward the camera so it wins the depth test against the terrain. This is a
  // per-mapper bias in f3d's single renderer (no overlay) -> stable, no flicker on motion.
  vtkMapper::SetResolveCoincidentTopologyToPolygonOffset();
  m->SetRelativeCoincidentTopologyPolygonOffsetParameters(0.0, -66000.0);
  m->SetRelativeCoincidentTopologyLineOffsetParameters(0.0, -66000.0);
  vtkSmartPointer<vtkActor> a = vtkSmartPointer<vtkActor>::New();
  a->SetMapper(m);
  a->GetProperty()->SetColor(r, g, b);
  a->GetProperty()->LightingOff();
  return a;
}

// Shared look for both ring bands: a mid grey, lit but with high ambient so the band
// stays clearly visible from every angle (no dark/flickering faces) while the fixed light
// still gives it a little shading.
void ringLook(vtkActor* a)
{
  a->GetProperty()->SetColor(0.78, 0.78, 0.82);
  a->GetProperty()->LightingOn();
  a->GetProperty()->SetAmbient(0.6);
  a->GetProperty()->SetDiffuse(0.5);
  a->GetProperty()->SetSpecular(0.12);
  a->GetProperty()->SetSpecularPower(15.0);
}

// Update the vertical arrowhead so its base stays anchored at the shaft top while its
// height tracks the current vertical exaggeration (curSz). Local space.
void updateVCone(GizmoCtx& c)
{
  if (!c.vconeSrc)
  {
    return;
  }
  double h = kConeH0 * std::clamp(c.curSz, 0.15, 8.0);
  double base = kBodyZ;              // shaft top / body top
  c.vconeSrc->SetHeight(h);
  c.vconeSrc->SetRadius(kConeR);
  c.vconeSrc->SetDirection(0.0, 0.0, 1.0);
  c.vconeSrc->SetCenter(0.0, 0.0, base + 0.5 * h); // base fixed at shaft top
}

void updateLabel(GizmoCtx& c)
{
  if (!c.label)
  {
    return;
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "z x %.2f", c.curSz);
  c.label->SetInput(buf);
}

void cross3(const double a[3], const double b[3], double o[3])
{
  o[0] = a[1] * b[2] - a[2] * b[1];
  o[1] = a[2] * b[0] - a[0] * b[2];
  o[2] = a[0] * b[1] - a[1] * b[0];
}
bool normalize3(double v[3])
{
  const double n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  if (n < 1e-9)
  {
    return false;
  }
  v[0] /= n;
  v[1] /= n;
  v[2] /= n;
  return true;
}

// Place the gizmo. Vertical parts (shaft, cone, ring) are world-aligned (vertical = world
// +Z), so they stay ~screen-up and lean with the view inclination. The horizontal axis is
// oriented along the camera SCREEN-RIGHT vector (c.right) so it always lies left-right in
// the window and never spins with azimuth — baked into a UserMatrix (local +X -> right).
void placeAll(GizmoCtx& c)
{
  const double* p = c.centre;
  const double s = c.scale;
  vtkActor* world[] = { c.shaft, c.vcone, c.ring };
  for (vtkActor* a : world)
  {
    if (a)
    {
      a->SetUserMatrix(nullptr);
      a->SetScale(s);
      a->SetPosition(p[0], p[1], p[2]);
    }
  }

  // Horizontal axis basis: X = camera screen-right, Y/Z complete an orthonormal frame.
  double X[3] = { c.right[0], c.right[1], c.right[2] };
  double up[3] = { 0.0, 0.0, 1.0 };
  double Y[3], Z[3];
  cross3(up, X, Y);
  if (!normalize3(Y))
  {
    double alt[3] = { 0.0, 1.0, 0.0 };
    cross3(alt, X, Y);
    normalize3(Y);
  }
  cross3(X, Y, Z);
  normalize3(Z);

  // Axis LENGTH is in world units (pinned to the data bbox), independent of zoom; the tip
  // ring is gizmo-scaled (s) so it stays the same apparent size as the compass ring.
  const double L = (c.haxisLen > 1e-9) ? c.haxisLen : s * kHaxisFallback;
  double tip[3] = { p[0] + L * X[0], p[1] + L * X[1], p[2] + L * X[2] };

  if (c.shaftHSrc)
  {
    c.shaftHSrc->SetPoint1(p[0], p[1], p[2]);
    c.shaftHSrc->SetPoint2(tip[0], tip[1], tip[2]);
    c.shaftHSrc->Modified();
  }
  if (c.shaftH)
  {
    c.shaftH->SetUserMatrix(nullptr); // line points are already world coords
    c.shaftH->SetScale(1.0);
    c.shaftH->SetPosition(0.0, 0.0, 0.0);
  }
  if (c.harrow)
  {
    vtkNew<vtkMatrix4x4> M; // s-scaled ring oriented (axis = X) and placed at the tip
    M->Identity();
    for (int i = 0; i < 3; ++i)
    {
      M->SetElement(i, 0, s * X[i]);
      M->SetElement(i, 1, s * Y[i]);
      M->SetElement(i, 2, s * Z[i]);
      M->SetElement(i, 3, tip[i]);
    }
    c.harrow->SetUserMatrix(M);
  }

  if (c.label)
  {
    c.label->SetPosition(p[0], p[1], p[2] + s * (kBodyZ + kConeH0 * c.curSz + 0.5));
  }
}

// StartEvent on the renderer: keep the gizmo at the focal point, sized from camera
// distance so it keeps a roughly constant apparent size as the user zooms.
void PlaceCB(vtkObject* caller, unsigned long, void* clientData, void*)
{
  GizmoCtx* c = static_cast<GizmoCtx*>(clientData);
  vtkRenderer* ren = vtkRenderer::SafeDownCast(caller);
  if (!c || !ren || !ren->GetActiveCamera())
  {
    return;
  }
  vtkCamera* cam = ren->GetActiveCamera();
  cam->GetFocalPoint(c->centre);
  double d = cam->GetDistance();
  if (cam->GetParallelProjection())
  {
    d = cam->GetParallelScale() * 2.0;
  }
  c->scale = std::max(1e-6, 0.085 * d);

  // Screen-right = worldUp x viewPlaneNormal: a horizontal vector that always points to
  // the right of the window, so the horizontal axis stays left-right (never spins with
  // azimuth). Keep the previous value when looking straight down (degenerate cross).
  double pos[3], foc[3];
  cam->GetPosition(pos);
  cam->GetFocalPoint(foc);
  double up[3] = { 0.0, 0.0, 1.0 };
  double vpn[3] = { pos[0] - foc[0], pos[1] - foc[1], pos[2] - foc[2] };
  double r[3];
  cross3(up, vpn, r);
  if (normalize3(r))
  {
    c->right[0] = r[0];
    c->right[1] = r[1];
    c->right[2] = r[2];
  }
  placeAll(*c);
}

// Project a world point to display (bottom-left origin) coords via the renderer.
void worldToDisplay(vtkRenderer* ren, double wx, double wy, double wz, double out[2])
{
  ren->SetWorldPoint(wx, wy, wz, 1.0);
  ren->WorldToDisplay();
  double d[3];
  ren->GetDisplayPoint(d);
  out[0] = d[0];
  out[1] = d[1];
}

double dist2(const double a[2], double bx, double by)
{
  const double dx = a[0] - bx;
  const double dy = a[1] - by;
  return dx * dx + dy * dy;
}

// Hit-test the click (display coords) against the handles by projecting each handle's
// world position to the screen. f3d's render passes defeat both vtkPropPicker (GL) and
// vtkCellPicker (ray) here, so we do our own deterministic screen-space test. Priority:
// the central cone, then the horizontal arrows, then the surrounding ring.
Grab hitTest(GizmoCtx& c, vtkRenderer* ren, int x, int y)
{
  if (!ren)
  {
    return Grab::None;
  }
  const double* p = c.centre;
  const double s = c.scale;
  const double coneH = kConeH0 * std::clamp(c.curSz, 0.15, 8.0);

  // Vertical cone (centre of mass) and a base-edge point to size the grab radius.
  double dCone[2], dConeEdge[2];
  worldToDisplay(ren, p[0], p[1], p[2] + s * (kBodyZ + 0.5 * coneH), dCone);
  worldToDisplay(ren, p[0] + s * kConeR, p[1], p[2] + s * kBodyZ, dConeEdge);
  double rCone = std::sqrt(dist2(dCone, dConeEdge[0], dConeEdge[1]));
  rCone = std::max(rCone * 1.4, 12.0);
  if (dist2(dCone, x, y) <= rCone * rCone)
  {
    return Grab::VScale;
  }

  // Tip ring of the horizontal axis (centre + L*right, L pinned to the data bbox).
  {
    const double L = (c.haxisLen > 1e-9) ? c.haxisLen : s * kHaxisFallback;
    double tx = p[0] + L * c.right[0];
    double ty = p[1] + L * c.right[1];
    double tz = p[2] + L * c.right[2];
    double dA[2], dAedge[2];
    worldToDisplay(ren, tx, ty, tz, dA);
    worldToDisplay(ren, tx, ty, tz + s * kRingR, dAedge);
    double rA = std::sqrt(dist2(dA, dAedge[0], dAedge[1]));
    rA = std::max(rA * 2.5, 16.0);
    if (dist2(dA, x, y) <= rA * rA)
    {
      return Grab::Tilt;
    }
  }

  // Compass ring: click distance from the ring centre must fall near the projected
  // ring radius (the band projects to an ellipse, so allow a generous band).
  double dCtr[2], dRim[2];
  worldToDisplay(ren, p[0], p[1], p[2] + s * kBodyZ, dCtr);
  worldToDisplay(ren, p[0] + s * kRingR, p[1], p[2] + s * kBodyZ, dRim);
  const double r = std::sqrt(dist2(dCtr, x, y));
  const double rRim = std::sqrt(dist2(dCtr, dRim[0], dRim[1]));
  if (r >= 0.45 * rRim && r <= 1.25 * rRim)
  {
    return Grab::Azimuth;
  }
  return Grab::None;
}

void DragCB(vtkObject* caller, unsigned long eid, void* clientData, void*)
{
  GizmoCtx* c = static_cast<GizmoCtx*>(clientData);
  vtkRenderWindowInteractor* rwi = vtkRenderWindowInteractor::SafeDownCast(caller);
  if (!c || !rwi || !c->window)
  {
    return;
  }
  vtkRenderer* ren = c->renderer;
  vtkCamera* cam = (ren && ren->GetActiveCamera()) ? ren->GetActiveCamera() : nullptr;
  bool handled = false;

  if (eid == vtkCommand::LeftButtonPressEvent)
  {
    const int x = rwi->GetEventPosition()[0];
    const int y = rwi->GetEventPosition()[1];
    c->lastX = x;
    c->lastY = y;

    // Keep the legacy Ctrl+left-drag vertical-scale gesture working.
    if (rwi->GetControlKey())
    {
      c->grab = Grab::VScale;
      c->startY = y;
      c->startSz = c->curSz;
      handled = true;
    }
    else
    {
      c->grab = hitTest(*c, ren, x, y);
      if (c->grab == Grab::VScale)
      {
        c->startY = y;
        c->startSz = c->curSz;
      }
      handled = (c->grab != Grab::None); // miss -> let f3d rotate/pan
    }
  }
  else if (eid == vtkCommand::MouseMoveEvent)
  {
    const int x = rwi->GetEventPosition()[0];
    const int y = rwi->GetEventPosition()[1];
    if (c->grab == Grab::VScale && c->options)
    {
      const double dy = static_cast<double>(y - c->startY);
      double sz = std::max(1e-6, c->startSz * std::exp(c->sensitivity * dy));
      c->curSz = sz;
      updateVCone(*c);
      updateLabel(*c);
      c->options->set("render.model_scale", std::vector<double>{ 1.0, 1.0, sz });
      c->window->render();
      handled = true;
    }
    else if (c->grab == Grab::Tilt && cam)
    {
      const double dy = static_cast<double>(y - c->lastY);
      cam->Elevation(-dy * c->rotSpeed);
      cam->OrthogonalizeViewUp();
      c->window->render();
      handled = true;
    }
    else if (c->grab == Grab::Azimuth && cam)
    {
      // PURE heading rotation about the WORLD vertical (+Z) through the focal point.
      // cam->Azimuth() turns about the camera view-up, which drifts off vertical once
      // the view is tilted -> it would also change the inclination. Rotating position
      // AND view-up about world +Z keeps the elevation/inclination fixed.
      const double dx = static_cast<double>(x - c->lastX);
      const double ang = -dx * c->rotSpeed;
      double fp[3], pos[3], vu[3];
      cam->GetFocalPoint(fp);
      cam->GetPosition(pos);
      cam->GetViewUp(vu);
      vtkNew<vtkTransform> tp;
      tp->Translate(fp[0], fp[1], fp[2]);
      tp->RotateZ(ang);
      tp->Translate(-fp[0], -fp[1], -fp[2]);
      double npos[3];
      tp->TransformPoint(pos, npos);
      vtkNew<vtkTransform> tv;
      tv->RotateZ(ang);
      double nvu[3];
      tv->TransformVector(vu, nvu);
      cam->SetPosition(npos);
      cam->SetViewUp(nvu);
      c->window->render();
      handled = true;
    }
    c->lastX = x;
    c->lastY = y;
  }
  else if (eid == vtkCommand::LeftButtonReleaseEvent)
  {
    if (c->grab != Grab::None)
    {
      c->grab = Grab::None;
      handled = true;
    }
  }

  if (c->dragCmd)
  {
    c->dragCmd->SetAbortFlagOnExecute(handled ? 1 : 0);
  }
}

void buildGeometry(GizmoCtx& c)
{
  // Vertical shaft (thin line, not a handle): drops from the floating body (z=kBodyZ)
  // down to the focal point (z=0, the rotation centre).
  vtkNew<vtkLineSource> shaftSrc;
  shaftSrc->SetPoint1(0.0, 0.0, 0.0);
  shaftSrc->SetPoint2(0.0, 0.0, kBodyZ);
  c.shaftSrc = shaftSrc;
  c.shaft = makeActor(shaftSrc, 0.95, 0.95, 0.95);
  c.shaft->GetProperty()->SetLineWidth(2.0);
  c.shaft->PickableOff();

  // Vertical arrowhead = VERTICAL SCALE handle (stretches with exaggeration).
  vtkNew<vtkConeSource> vconeSrc;
  vconeSrc->SetResolution(24);
  c.vconeSrc = vconeSrc;
  c.vcone = makeActor(vconeSrc, 1.0, 0.85, 0.2); // amber
  // Stronger on-top bias than the rings so the compass ring never overlays/hides the cone.
  c.vcone->GetMapper()->SetRelativeCoincidentTopologyPolygonOffsetParameters(0.0, -200000.0);
  c.vcone->PickableOn();
  updateVCone(c);

  // Horizontal axis (thin line, not a handle). Its endpoints are set in world coords by
  // placeAll each frame (centre -> data-edge tip), so build with placeholder points.
  vtkNew<vtkLineSource> shaftHSrc;
  shaftHSrc->SetPoint1(0.0, 0.0, 0.0);
  shaftHSrc->SetPoint2(1.0, 0.0, 0.0);
  c.shaftHSrc = shaftHSrc;
  c.shaftH = makeActor(shaftHSrc, 0.95, 0.95, 0.95);
  c.shaftH->GetProperty()->SetLineWidth(2.0);
  c.shaftH->PickableOff();

  // TILT handle at the axis tip = a ring IDENTICAL to the compass ring (same kRingR/kRingH
  // + ringLook), but with its plane ORTHOGONAL to the axis: build the band with its
  // cylinder axis along local +X (+Y -> +X), centred at the local origin; placeAll's
  // UserMatrix maps local +X -> the axis direction and translates it to the tip.
  vtkNew<vtkCylinderSource> harr;
  harr->SetRadius(kRingR);
  harr->SetHeight(kRingH);
  harr->SetResolution(72);
  harr->CappingOff();
  harr->SetCenter(0.0, 0.0, 0.0);
  vtkNew<vtkTransform> harrXf;
  harrXf->PostMultiply();
  harrXf->RotateZ(-90.0); // +Y axis -> +X axis
  vtkNew<vtkTransformPolyDataFilter> harrTf;
  harrTf->SetInputConnection(harr->GetOutputPort());
  harrTf->SetTransform(harrXf);
  c.harrow = makeActor(harrTf, 0.55, 0.55, 0.6);
  ringLook(c.harrow);
  c.harrow->PickableOn();

  // Compass ring = AZIMUTH handle. A slim open cylinder (no caps) = a "finger-ring"
  // band standing with a vertical axis (like the Fledermaus widget), not a flat washer.
  // vtkCylinderSource's axis is +Y, so rotate it to +Z and lift it to the body height.
  vtkNew<vtkCylinderSource> ringSrc;
  ringSrc->SetRadius(kRingR);
  ringSrc->SetHeight(kRingH);
  ringSrc->SetResolution(72);
  ringSrc->CappingOff();
  ringSrc->SetCenter(0.0, 0.0, 0.0);
  vtkNew<vtkTransform> ringXf;
  ringXf->PostMultiply();
  ringXf->RotateX(90.0);                 // +Y axis -> +Z axis (vertical band)
  ringXf->Translate(0.0, 0.0, kBodyZ);
  vtkNew<vtkTransformPolyDataFilter> ringTf;
  ringTf->SetInputConnection(ringSrc->GetOutputPort());
  ringTf->SetTransform(ringXf);
  c.ring = makeActor(ringTf, 0.55, 0.55, 0.6);
  ringLook(c.ring);
  c.ring->PickableOn();

  // Axis label (vertical exaggeration readout).
  vtkSmartPointer<vtkBillboardTextActor3D> label =
    vtkSmartPointer<vtkBillboardTextActor3D>::New();
  label->GetTextProperty()->SetColor(1.0, 1.0, 1.0);
  label->GetTextProperty()->SetFontSize(18);
  label->GetTextProperty()->SetJustificationToCentered();
  c.label = label;
  updateLabel(c);
}
} // namespace

extern "C"
{

  int f3d_ext_enable_scale_handle(
    f3d_window_t* window, f3d_options_t* options, double sensitivity)
  {
    vtkRenderer* ren = renderer_of(window);
    vtkRenderWindowInteractor* rwi = interactor_of(window);
    if (!ren || !ren->GetActiveCamera() || !rwi || !window || !options)
    {
      return 0;
    }

    GizmoCtx& c = registry()[window];

    // Re-enable: tear down the prior observer + overlay renderer (drops its props/light).
    if (c.placeTag && c.renderer)
    {
      c.renderer->RemoveObserver(c.placeTag);
      c.placeTag = 0;
    }
    vtkActor* old[] = { c.shaft, c.shaftH, c.vcone, c.harrow, c.ring };
    for (vtkActor* a : old)
    {
      if (a)
      {
        ren->RemoveViewProp(a);
      }
    }
    if (c.label)
    {
      ren->RemoveViewProp(c.label);
    }
    if (c.light)
    {
      ren->RemoveLight(c.light);
    }
    for (unsigned long t : c.dragTags)
    {
      if (t)
      {
        rwi->RemoveObserver(t);
      }
    }

    c.renderer = ren;
    c.rwi = rwi;
    c.window = reinterpret_cast<f3d::window*>(window);
    c.options = reinterpret_cast<f3d::options*>(options);
    c.sensitivity = (sensitivity > 0.0) ? sensitivity : 0.01;
    c.grab = Grab::None;
    c.curSz = 1.0;

    // Pin the horizontal-axis length to the data bbox (world units): half the larger XY
    // extent, measured BEFORE the gizmo props are added so they don't inflate it. 0 if the
    // bounds are invalid -> placeAll falls back to a gizmo-scale multiple.
    c.haxisLen = 0.0;
    double b[6];
    ren->ComputeVisiblePropBounds(b);
    if (b[1] >= b[0] && b[3] >= b[2])
    {
      c.haxisLen = 0.5 * std::max(b[1] - b[0], b[3] - b[2]);
    }

    buildGeometry(c);

    ren->AddViewProp(c.shaft);
    ren->AddViewProp(c.shaftH);
    ren->AddViewProp(c.vcone);
    ren->AddViewProp(c.harrow);
    ren->AddViewProp(c.ring);
    ren->AddViewProp(c.label);

    // Fixed-direction light (world-anchored) so the ring bands keep constant shading.
    vtkNew<vtkLight> light;
    light->SetLightTypeToSceneLight();
    light->SetPositional(false);
    light->SetPosition(0.6, 0.4, 1.0);
    light->SetFocalPoint(0.0, 0.0, 0.0);
    light->SetColor(1.0, 1.0, 1.0);
    light->SetIntensity(1.4);
    c.light = light;
    ren->AddLight(light);

    // Place once now, then keep following the focal point every render.
    PlaceCB(ren, 0, &c, nullptr);

    vtkNew<vtkCallbackCommand> placeCmd;
    placeCmd->SetCallback(PlaceCB);
    placeCmd->SetClientData(&c);
    c.placeCmd = placeCmd;
    c.placeTag = ren->AddObserver(vtkCommand::StartEvent, placeCmd);

    vtkNew<vtkCallbackCommand> dragCmd;
    dragCmd->SetCallback(DragCB);
    dragCmd->SetClientData(&c);
    c.dragCmd = dragCmd;
    c.dragTags[0] = rwi->AddObserver(vtkCommand::LeftButtonPressEvent, dragCmd, 10.0);
    c.dragTags[1] = rwi->AddObserver(vtkCommand::MouseMoveEvent, dragCmd, 10.0);
    c.dragTags[2] = rwi->AddObserver(vtkCommand::LeftButtonReleaseEvent, dragCmd, 10.0);

    c.window->render();
    return 1;
  }

  void f3d_ext_disable_scale_handle(f3d_window_t* window)
  {
    auto it = registry().find(window);
    if (it == registry().end())
    {
      return;
    }
    GizmoCtx& c = it->second;
    if (vtkRenderer* ren = renderer_of(window))
    {
      if (c.placeTag)
      {
        ren->RemoveObserver(c.placeTag);
      }
      vtkActor* parts[] = { c.shaft, c.shaftH, c.vcone, c.harrow, c.ring };
      for (vtkActor* a : parts)
      {
        if (a)
        {
          ren->RemoveViewProp(a);
        }
      }
      if (c.label)
      {
        ren->RemoveViewProp(c.label);
      }
      if (c.light)
      {
        ren->RemoveLight(c.light);
      }
    }
    if (c.rwi)
    {
      for (unsigned long t : c.dragTags)
      {
        if (t)
        {
          c.rwi->RemoveObserver(t);
        }
      }
    }
    registry().erase(it);
  }

} // extern "C"
