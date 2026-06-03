/**
 * f3d_ext_ui.cxx — inject custom ImGui widgets into the F3D window (demo).
 *
 * The stock libf3d UI (vtkF3DImguiActor) draws a FIXED set of overlays and
 * exposes no hook to add your own widgets. This flips a visibility flag on the
 * window's UI actor; the actual widget drawing lives in vtkF3DImguiActor::
 * RenderUserWidgets() so the ImGui calls run inside f3d's own (single, global)
 * ImGui context — drawing ImGui from this DLL would hit a different, empty
 * context and crash.
 *
 * The UI actor is private to vtkF3DRenderer but is added to the renderer as a
 * view prop, so it is reachable through the f3d_ext renderer hatch + a
 * SafeDownCast, with no extra accessor on the renderer.
 *
 * BUILD: same c_api target as the other f3d_ext sources.
 */

#include "f3d_ext.h"

#include "window.h"      // f3d::window (public)
#include "window_impl.h" // f3d::detail::window_impl (PRIVATE)

#include "vtkF3DUIActor.h" // the UI overlay actor (PRIVATE vtkext module)

#include <vtkProp.h>
#include <vtkPropCollection.h>
#include <vtkRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkRendererCollection.h>

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

// The UI actor is added as a view prop on the renderer; find it by type.
vtkF3DUIActor* ui_actor_of(f3d_window_t* window)
{
  vtkRenderer* ren = renderer_of(window);
  if (!ren)
  {
    return nullptr;
  }
  vtkPropCollection* props = ren->GetViewProps();
  if (!props)
  {
    return nullptr;
  }
  props->InitTraversal();
  vtkProp* p = nullptr;
  while ((p = props->GetNextProp()))
  {
    if (vtkF3DUIActor* ui = vtkF3DUIActor::SafeDownCast(p))
    {
      return ui;
    }
  }
  return nullptr;
}
} // namespace

extern "C"
{

  int f3d_ext_show_demo_ui(f3d_window_t* window, int on)
  {
    vtkF3DUIActor* ui = ui_actor_of(window);
    if (!ui)
    {
      return 0;
    }
    ui->SetUserWidgetsVisibility(on != 0);
    return 1;
  }

} // extern "C"
