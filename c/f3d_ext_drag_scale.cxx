/**
 * f3d_ext_drag_scale.cxx — Ctrl+left-drag vertical-scale gesture for f3d_ext.
 *
 * Two facts shape this:
 *  1. f3d owns its interactor style and drives interaction through it, so replacing
 *     the interactor style receives no events. We instead attach high-priority
 *     vtkCallbackCommand observers directly on the render-window interactor, where
 *     f3d InvokeEvent()s all mouse events.
 *  2. window_impl re-pushes opt.render.model_scale to the renderer on every f3d
 *     render, so setting the renderer scale directly is immediately clobbered. We
 *     therefore set the OPTION render.model_scale and trigger an f3d render
 *     (window::render), which is the path that persists.
 *
 * While Ctrl is held, left-button drag maps vertical motion to render.model_scale's
 * z, and aborts the event so f3d's own rotate does not also run.
 */

#include "f3d_ext.h"

#include "options.h" // f3d::options (public)
#include "window.h"  // f3d::window (public)

#include <vtkCallbackCommand.h>
#include <vtkCommand.h>
#include <vtkNew.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkSmartPointer.h>

// window_impl.h is PRIVATE; only needed to reach the render window's interactor.
#include "window_impl.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

namespace
{
vtkRenderWindowInteractor* interactor_of(f3d_window_t* window)
{
  if (!window)
  {
    return nullptr;
  }
  auto* impl = static_cast<f3d::detail::window_impl*>(reinterpret_cast<f3d::window*>(window));
  if (!impl || !impl->GetRenderWindow())
  {
    return nullptr;
  }
  return impl->GetRenderWindow()->GetInteractor();
}

struct DragCtx
{
  vtkRenderWindowInteractor* rwi = nullptr;
  f3d::window* window = nullptr;
  f3d::options* options = nullptr;
  vtkSmartPointer<vtkCallbackCommand> cmd;
  unsigned long tags[3] = { 0, 0, 0 };
  double sensitivity = 0.01;
  bool scaling = false;
  int startY = 0;
  double startSz = 1.0;
  double curSz = 1.0;
};

std::map<f3d_window_t*, DragCtx>& registry()
{
  static std::map<f3d_window_t*, DragCtx> r;
  return r;
}

void DragCB(vtkObject* caller, unsigned long eid, void* clientData, void*)
{
  DragCtx* c = static_cast<DragCtx*>(clientData);
  vtkRenderWindowInteractor* rwi = vtkRenderWindowInteractor::SafeDownCast(caller);
  if (!c || !rwi)
  {
    return;
  }

  bool handled = false;
  if (eid == vtkCommand::LeftButtonPressEvent)
  {
    if (rwi->GetControlKey())
    {
      c->scaling = true;
      c->startY = rwi->GetEventPosition()[1];
      c->startSz = c->curSz;
      handled = true;
    }
  }
  else if (eid == vtkCommand::MouseMoveEvent)
  {
    if (c->scaling && c->options && c->window)
    {
      const double dy = static_cast<double>(rwi->GetEventPosition()[1] - c->startY);
      double sz = std::max(1e-6, c->startSz * std::exp(c->sensitivity * dy));
      c->curSz = sz;
      // Set the option (survives window_impl's per-render push) then render through
      // f3d so the push applies it.
      c->options->set("render.model_scale", std::vector<double>{ 1.0, 1.0, sz });
      c->window->render();
      handled = true;
    }
  }
  else if (eid == vtkCommand::LeftButtonReleaseEvent)
  {
    if (c->scaling)
    {
      c->scaling = false;
      handled = true;
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

  int f3d_ext_enable_vertical_scale_drag(
    f3d_window_t* window, f3d_options_t* options, double sensitivity)
  {
    vtkRenderWindowInteractor* rwi = interactor_of(window);
    if (!rwi || !window || !options)
    {
      return 0;
    }

    DragCtx& c = registry()[window];
    if (c.cmd)
    {
      for (unsigned long t : c.tags)
      {
        if (t)
        {
          rwi->RemoveObserver(t);
        }
      }
    }
    c.rwi = rwi;
    c.window = reinterpret_cast<f3d::window*>(window);
    c.options = reinterpret_cast<f3d::options*>(options);
    c.sensitivity = (sensitivity > 0.0) ? sensitivity : 0.01;
    c.scaling = false;
    c.curSz = 1.0;

    vtkNew<vtkCallbackCommand> cmd;
    cmd->SetCallback(DragCB);
    cmd->SetClientData(&c);
    c.cmd = cmd;

    c.tags[0] = rwi->AddObserver(vtkCommand::LeftButtonPressEvent, cmd, 10.0);
    c.tags[1] = rwi->AddObserver(vtkCommand::MouseMoveEvent, cmd, 10.0);
    c.tags[2] = rwi->AddObserver(vtkCommand::LeftButtonReleaseEvent, cmd, 10.0);
    return 1;
  }

  void f3d_ext_disable_vertical_scale_drag(f3d_window_t* window)
  {
    auto it = registry().find(window);
    if (it != registry().end())
    {
      if (it->second.rwi)
      {
        for (unsigned long t : it->second.tags)
        {
          if (t)
          {
            it->second.rwi->RemoveObserver(t);
          }
        }
      }
      registry().erase(it);
    }
  }

} // extern "C"
