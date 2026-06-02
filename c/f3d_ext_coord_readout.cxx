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
#include <vtkCommand.h>
#include <vtkCornerAnnotation.h>
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

} // extern "C"
