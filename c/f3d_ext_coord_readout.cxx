/**
 * f3d_ext_coord_readout.cxx — live mouse-coordinate readout for f3d_ext.
 *
 * f3d drives its own interactor style, so we hook a vtkCallbackCommand observer on
 * the render-window interactor (where f3d InvokeEvent()s mouse moves) rather than
 * replacing the style. On each move we pick the world point under the cursor
 * (vtkCellPicker) and write its X/Y/Z into a vtkCornerAnnotation pinned bottom-left.
 * The observer is passive (never aborts), so camera interaction is unchanged.
 * Closes gap #8 (move events + pick + custom text overlay) inside the DLL.
 *
 * BUILD: same c_api target as the other f3d_ext sources; needs VTK
 * RenderingAnnotation (vtkCornerAnnotation) in addition to RenderingCore
 * (vtkCellPicker) — see c/CMakeLists.txt.
 */

#include "f3d_ext.h"

#include "window.h"      // f3d::window (public)
#include "window_impl.h" // f3d::detail::window_impl (PRIVATE)

#include <vtkCallbackCommand.h>
#include <vtkCamera.h>
#include <vtkCommand.h>
#include <vtkCornerAnnotation.h>
#include <vtkInteractorObserver.h>
#include <vtkNew.h>
#include <vtkCellPicker.h>
#include <vtkPointPicker.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkRenderer.h>
#include <vtkRendererCollection.h>
#include <vtkSmartPointer.h>
#include <vtkTextProperty.h>

#include <cstdio>
#include <cstdlib>
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

vtkRenderWindowInteractor* interactor_of(f3d_window_t* window)
{
  f3d::detail::window_impl* impl = impl_of(window);
  if (!impl || !impl->GetRenderWindow())
  {
    return nullptr;
  }
  return impl->GetRenderWindow()->GetInteractor();
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

struct ReadoutCtx
{
  vtkRenderWindowInteractor* rwi = nullptr;
  vtkRenderer* renderer = nullptr;
  vtkSmartPointer<vtkCellPicker> picker;
  vtkSmartPointer<vtkPointPicker> ppicker; // fallback for point clouds (no cells)
  vtkSmartPointer<vtkCornerAnnotation> annotation;
  vtkSmartPointer<vtkCallbackCommand> cmd;
  unsigned long tag = 0;
};

std::map<f3d_window_t*, ReadoutCtx>& registry()
{
  static std::map<f3d_window_t*, ReadoutCtx> r;
  return r;
}

void MoveCB(vtkObject* caller, unsigned long, void* clientData, void*)
{
  ReadoutCtx* c = static_cast<ReadoutCtx*>(clientData);
  vtkRenderWindowInteractor* rwi = vtkRenderWindowInteractor::SafeDownCast(caller);
  if (!c || !rwi || !c->renderer || !c->picker || !c->annotation)
  {
    return;
  }
  const int* p = rwi->GetEventPosition();
  char buf[160];
  double w[3];
  bool hit = false;
  if (c->picker->Pick(p[0], p[1], 0.0, c->renderer)) // surfaces (cells)
  {
    c->picker->GetPickPosition(w);
    hit = true;
  }
  else if (c->ppicker && c->ppicker->Pick(p[0], p[1], 0.0, c->renderer)) // point clouds
  {
    c->ppicker->GetPickPosition(w);
    hit = true;
  }
  if (hit)
  {
    std::snprintf(buf, sizeof(buf), "X: %.6g   Y: %.6g   Z: %.6g", w[0], w[1], w[2]);
    c->annotation->SetText(0, buf); // 0 = lower-left corner
  }
  else
  {
    c->annotation->SetText(0, " ");
  }
  rwi->Render();
}

// --- middle-click to set the rotation centre -------------------------------------
// f3d uses middle-DRAG for pan; we add middle-CLICK (press+release without moving) to
// pick the point under the cursor, make it the camera focal point (rotation centre)
// and pan so it sits at the screen centre — by shifting the camera position by the same
// vector as the focal point, the view direction is preserved. Passive (low priority, no
// abort) so middle-drag panning is untouched.
struct FocusCtx
{
  vtkRenderWindowInteractor* rwi = nullptr;
  vtkObject* style = nullptr; // f3d delivers middle-button events to the STYLE, not the rwi
  vtkRenderer* renderer = nullptr;
  vtkSmartPointer<vtkCellPicker> picker;
  vtkSmartPointer<vtkPointPicker> ppicker;
  vtkSmartPointer<vtkCallbackCommand> cmd;
  unsigned long tagP = 0, tagR = 0, tagM = 0;
  int px = 0, py = 0;       // press position (click vs drag test)
  int lastx = 0, lasty = 0; // previous move position (pan delta)
  bool panning = false;
};

std::map<f3d_window_t*, FocusCtx>& focusRegistry()
{
  static std::map<f3d_window_t*, FocusCtx> r;
  return r;
}

// Translate the camera so the world point under (lastx,lasty) moves under (curx,cury)
// at the focal-plane depth — the standard trackball pan, done by hand because f3d's
// built-in interactor style maps middle-drag to dolly (zoom), not pan.
void panCamera(vtkRenderer* ren, int lastx, int lasty, int curx, int cury)
{
  vtkCamera* cam = ren->GetActiveCamera();
  if (!cam)
  {
    return;
  }
  double fp[3];
  cam->GetFocalPoint(fp);
  ren->SetWorldPoint(fp[0], fp[1], fp[2], 1.0);
  ren->WorldToDisplay();
  const double dz = ren->GetDisplayPoint()[2];
  ren->SetDisplayPoint(static_cast<double>(curx), static_cast<double>(cury), dz);
  ren->DisplayToWorld();
  double wn[4];
  std::copy_n(ren->GetWorldPoint(), 4, wn);
  ren->SetDisplayPoint(static_cast<double>(lastx), static_cast<double>(lasty), dz);
  ren->DisplayToWorld();
  double wo[4];
  std::copy_n(ren->GetWorldPoint(), 4, wo);
  const double mx = wo[0] / wo[3] - wn[0] / wn[3];
  const double my = wo[1] / wo[3] - wn[1] / wn[3];
  const double mz = wo[2] / wo[3] - wn[2] / wn[3];
  double pos[3];
  cam->GetPosition(pos);
  cam->SetFocalPoint(fp[0] + mx, fp[1] + my, fp[2] + mz);
  cam->SetPosition(pos[0] + mx, pos[1] + my, pos[2] + mz);
}

void FocusCB(vtkObject*, unsigned long eid, void* clientData, void*)
{
  FocusCtx* c = static_cast<FocusCtx*>(clientData);
  // The event fires on the STYLE (caller), but the event position + render live on the
  // interactor, so use the stored rwi rather than the caller.
  vtkRenderWindowInteractor* rwi = c ? c->rwi : nullptr;
  if (!c || !rwi || !c->renderer)
  {
    return;
  }
  const int* p = rwi->GetEventPosition();
  bool handled = false;

  if (eid == vtkCommand::MiddleButtonPressEvent)
  {
    c->px = c->lastx = p[0];
    c->py = c->lasty = p[1];
    c->panning = true;
    handled = true; // swallow so f3d does not StartDolly (its middle-drag = zoom)
  }
  else if (eid == vtkCommand::MouseMoveEvent)
  {
    if (c->panning) // middle held -> PAN
    {
      panCamera(c->renderer, c->lastx, c->lasty, p[0], p[1]);
      c->lastx = p[0];
      c->lasty = p[1];
      c->renderer->ResetCameraClippingRange();
      rwi->Render();
      handled = true; // only abort while panning, so the coord readout still runs otherwise
    }
  }
  else if (eid == vtkCommand::MiddleButtonReleaseEvent)
  {
    const bool click = std::abs(p[0] - c->px) <= 3 && std::abs(p[1] - c->py) <= 3;
    c->panning = false;
    handled = true;
    if (click) // middle CLICK (no drag) -> set rotation centre + recentre
    {
      double w[3];
      bool hit = false;
      if (c->picker && c->picker->Pick(p[0], p[1], 0.0, c->renderer))
      {
        c->picker->GetPickPosition(w);
        hit = true;
      }
      else if (c->ppicker && c->ppicker->Pick(p[0], p[1], 0.0, c->renderer))
      {
        c->ppicker->GetPickPosition(w);
        hit = true;
      }
      if (hit)
      {
        vtkCamera* cam = c->renderer->GetActiveCamera();
        if (cam)
        {
          double f0[3], p0[3];
          cam->GetFocalPoint(f0);
          cam->GetPosition(p0);
          cam->SetFocalPoint(w);
          cam->SetPosition(p0[0] + (w[0] - f0[0]), p0[1] + (w[1] - f0[1]), p0[2] + (w[2] - f0[2]));
          c->renderer->ResetCameraClippingRange();
          rwi->Render();
        }
      }
    }
  }

  if (c->cmd)
  {
    c->cmd->SetAbortFlagOnExecute(handled ? 1 : 0);
  }
}
} // namespace

extern "C"
{

  int f3d_ext_enable_coord_readout(f3d_window_t* window)
  {
    vtkRenderWindowInteractor* rwi = interactor_of(window);
    vtkRenderer* ren = renderer_of(window);
    if (!rwi || !ren)
    {
      return 0;
    }

    ReadoutCtx& c = registry()[window];
    if (c.tag && c.rwi)
    {
      c.rwi->RemoveObserver(c.tag); // re-enable: drop old observer
    }
    if (c.annotation && c.renderer)
    {
      c.renderer->RemoveViewProp(c.annotation);
    }

    vtkNew<vtkCellPicker> picker;
    vtkNew<vtkPointPicker> ppicker; // point clouds have no cells -> cell pick misses
    ppicker->SetTolerance(0.01);
    vtkNew<vtkCornerAnnotation> ann;
    ann->SetMaximumFontSize(18);
    ann->GetTextProperty()->SetColor(1.0, 1.0, 1.0);
    ann->SetText(0, " ");
    ren->AddViewProp(ann);

    vtkNew<vtkCallbackCommand> cmd;
    cmd->SetCallback(MoveCB);
    cmd->SetClientData(&c);

    c.rwi = rwi;
    c.renderer = ren;
    c.picker = picker;
    c.ppicker = ppicker;
    c.annotation = ann;
    c.cmd = cmd;
    c.tag = rwi->AddObserver(vtkCommand::MouseMoveEvent, cmd, 1.0);
    return 1;
  }

  void f3d_ext_disable_coord_readout(f3d_window_t* window)
  {
    auto it = registry().find(window);
    if (it != registry().end())
    {
      if (it->second.rwi && it->second.tag)
      {
        it->second.rwi->RemoveObserver(it->second.tag);
      }
      if (it->second.renderer && it->second.annotation)
      {
        it->second.renderer->RemoveViewProp(it->second.annotation);
      }
      registry().erase(it);
    }
  }

  int f3d_ext_enable_focus_pick(f3d_window_t* window)
  {
    vtkRenderWindowInteractor* rwi = interactor_of(window);
    vtkRenderer* ren = renderer_of(window);
    if (!rwi || !ren)
    {
      return 0;
    }
    // f3d delivers middle-button events to its interactor STYLE (interactor_impl adds its
    // OnMiddleButton callbacks there), NOT to the interactor — so observe the style for the
    // middle buttons. Mouse-move IS delivered to the interactor (the coord readout proves
    // it), so the pan move observer stays on the interactor.
    vtkObject* style = rwi->GetInteractorStyle();
    if (!style)
    {
      return 0;
    }
    FocusCtx& c = focusRegistry()[window];
    if (c.style)
    {
      if (c.tagP) { c.style->RemoveObserver(c.tagP); }
      if (c.tagR) { c.style->RemoveObserver(c.tagR); }
    }
    if (c.rwi && c.tagM) { c.rwi->RemoveObserver(c.tagM); }
    vtkNew<vtkCellPicker> picker;
    vtkNew<vtkPointPicker> ppicker;
    ppicker->SetTolerance(0.01);
    vtkNew<vtkCallbackCommand> cmd;
    cmd->SetCallback(FocusCB);
    cmd->SetClientData(&c);
    c.rwi = rwi;
    c.style = style;
    c.renderer = ren;
    c.picker = picker;
    c.ppicker = ppicker;
    c.cmd = cmd;
    // HIGH priority (10.0): run BEFORE f3d's own middle-button handler (added at default
    // priority 0.0) and abort it, so f3d's middle-drag dolly is replaced by our pan +
    // click-to-centre.
    c.tagP = style->AddObserver(vtkCommand::MiddleButtonPressEvent, cmd, 10.0);
    c.tagR = style->AddObserver(vtkCommand::MiddleButtonReleaseEvent, cmd, 10.0);
    c.tagM = rwi->AddObserver(vtkCommand::MouseMoveEvent, cmd, 10.0);
    return 1;
  }

  void f3d_ext_disable_focus_pick(f3d_window_t* window)
  {
    auto it = focusRegistry().find(window);
    if (it != focusRegistry().end())
    {
      if (it->second.style)
      {
        if (it->second.tagP) { it->second.style->RemoveObserver(it->second.tagP); }
        if (it->second.tagR) { it->second.style->RemoveObserver(it->second.tagR); }
      }
      if (it->second.rwi && it->second.tagM) { it->second.rwi->RemoveObserver(it->second.tagM); }
      focusRegistry().erase(it);
    }
  }

} // extern "C"
