/**
 * f3d_ext_interactor.cxx — rubber-band area point selection for f3d_ext.
 *
 * Right-button drag draws a selection rectangle (a 2D overlay) and, on release,
 * runs the frustum area pick (f3d_ext_area_pick_points, in f3d_ext_pick.cxx). The
 * module keeps a persistent selection SET:
 *   - selection is TOGGLE-based: points already selected and re-dragged are
 *     deselected (XOR of the new box into the set);
 *   - Ctrl+Z undoes the last selection change (an undo stack of prior sets);
 * the selected points are highlighted in red on top, and the FULL current
 * selection is handed to the user callback after every change.
 *
 * Implemented with high-priority vtkCallbackCommand observers on the render-window
 * interactor (f3d drives its own interactor style, so a style swap receives no
 * events). Right-button events are aborted while selecting so f3d's zoom/dolly does
 * not run; coexists with the drag-scale and coordinate-readout extensions.
 */

#include "f3d_ext.h"

#include "window.h"      // f3d::window (public)
#include "window_impl.h" // f3d::detail::window_impl (PRIVATE)

#include <vtkActor.h>
#include <vtkActor2D.h>
#include <vtkActorCollection.h>
#include <vtkCallbackCommand.h>
#include <vtkCellArray.h>
#include <vtkCommand.h>
#include <vtkCoordinate.h>
#include <vtkDataSet.h>
#include <vtkMapper.h>
#include <vtkNew.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkPolyDataMapper2D.h>
#include <vtkProperty.h>
#include <vtkProperty2D.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkRenderer.h>
#include <vtkRendererCollection.h>
#include <vtkSmartPointer.h>

#include <cstring>
#include <map>
#include <set>
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

struct RubberCtx
{
  f3d_window_t* window = nullptr;
  vtkRenderWindowInteractor* rwi = nullptr;
  vtkRenderer* renderer = nullptr;
  f3d_ext_pick_callback_t cb = nullptr;
  void* user = nullptr;
  vtkSmartPointer<vtkActor2D> box;
  vtkSmartPointer<vtkPoints> boxPts;
  vtkSmartPointer<vtkActor> highlight;
  vtkSmartPointer<vtkPoints> hlPts;
  vtkSmartPointer<vtkPolyData> hlPoly;
  vtkSmartPointer<vtkCallbackCommand> cmd;
  unsigned long tags[4] = { 0, 0, 0, 0 };
  bool selecting = false;
  int x0 = 0;
  int y0 = 0;
  std::set<size_t> selection;                // current accumulated selection
  std::vector<std::vector<size_t>> undo;      // prior selection states
};

std::map<f3d_window_t*, RubberCtx>& registry()
{
  static std::map<f3d_window_t*, RubberCtx> r;
  return r;
}

void setBox(RubberCtx* c, int x0, int y0, int x1, int y1)
{
  c->boxPts->SetPoint(0, x0, y0, 0.0);
  c->boxPts->SetPoint(1, x1, y0, 0.0);
  c->boxPts->SetPoint(2, x1, y1, 0.0);
  c->boxPts->SetPoint(3, x0, y1, 0.0);
  c->boxPts->Modified();
}

// First pickable data actor with point geometry (skips the highlight overlay).
vtkActor* dataActor(vtkRenderer* ren, vtkActor* skip)
{
  vtkActorCollection* actors = ren->GetActors();
  if (!actors)
  {
    return nullptr;
  }
  actors->InitTraversal();
  vtkActor* a = nullptr;
  while ((a = actors->GetNextActor()))
  {
    if (a == skip || !a->GetPickable() || !a->GetVisibility() || !a->GetMapper())
    {
      continue;
    }
    vtkDataSet* ds = vtkDataSet::SafeDownCast(a->GetMapper()->GetInput());
    if (ds && ds->GetNumberOfPoints() > 0)
    {
      return a;
    }
  }
  return nullptr;
}

// Rebuild the red highlight overlay from the current selection set.
void rebuildHighlight(RubberCtx* c)
{
  vtkActor* src = dataActor(c->renderer, c->highlight);
  c->hlPts->Reset();
  vtkNew<vtkCellArray> verts;
  if (src && !c->selection.empty())
  {
    vtkDataSet* ds = vtkDataSet::SafeDownCast(src->GetMapper()->GetInput());
    const vtkIdType npts = ds ? ds->GetNumberOfPoints() : 0;
    for (size_t id : c->selection)
    {
      if (static_cast<vtkIdType>(id) < npts)
      {
        double pt[3];
        ds->GetPoint(static_cast<vtkIdType>(id), pt);
        const vtkIdType nid = c->hlPts->InsertNextPoint(pt);
        verts->InsertNextCell(1, &nid);
      }
    }
    c->highlight->SetUserMatrix(src->GetMatrix());
  }
  c->hlPoly->SetVerts(verts);
  c->hlPoly->Modified();
  c->highlight->SetVisibility(c->selection.empty() ? 0 : 1);
}

// Hand the full current selection to the user callback.
void emitSelection(RubberCtx* c)
{
  if (!c->cb)
  {
    return;
  }
  if (c->selection.empty())
  {
    c->cb(nullptr, 0, c->user);
    return;
  }
  std::vector<size_t> ids(c->selection.begin(), c->selection.end());
  c->cb(ids.data(), ids.size(), c->user);
}

void RubberCB(vtkObject* caller, unsigned long eid, void* clientData, void*)
{
  RubberCtx* c = static_cast<RubberCtx*>(clientData);
  vtkRenderWindowInteractor* rwi = vtkRenderWindowInteractor::SafeDownCast(caller);
  if (!c || !rwi)
  {
    return;
  }

  bool handled = false;
  if (eid == vtkCommand::RightButtonPressEvent)
  {
    c->x0 = rwi->GetEventPosition()[0];
    c->y0 = rwi->GetEventPosition()[1];
    c->selecting = true;
    setBox(c, c->x0, c->y0, c->x0, c->y0);
    c->box->SetVisibility(1);
    rwi->Render();
    handled = true;
  }
  else if (eid == vtkCommand::MouseMoveEvent)
  {
    if (c->selecting)
    {
      setBox(c, c->x0, c->y0, rwi->GetEventPosition()[0], rwi->GetEventPosition()[1]);
      rwi->Render();
      handled = true;
    }
  }
  else if (eid == vtkCommand::RightButtonReleaseEvent)
  {
    if (c->selecting)
    {
      c->selecting = false;
      const int x1 = rwi->GetEventPosition()[0];
      const int y1 = rwi->GetEventPosition()[1];
      c->box->SetVisibility(0);

      size_t count = 0;
      size_t* ids = f3d_ext_area_pick_points(c->window, c->x0, c->y0, x1, y1, &count);

      // Save current state for undo, then TOGGLE the newly picked ids in/out.
      c->undo.push_back(std::vector<size_t>(c->selection.begin(), c->selection.end()));
      for (size_t i = 0; i < count; ++i)
      {
        auto it = c->selection.find(ids[i]);
        if (it == c->selection.end())
        {
          c->selection.insert(ids[i]);
        }
        else
        {
          c->selection.erase(it);
        }
      }
      f3d_ext_free_ids(ids);

      rebuildHighlight(c);
      rwi->Render();
      emitSelection(c);
      handled = true;
    }
  }
  else if (eid == vtkCommand::KeyPressEvent)
  {
    // Ctrl+Z -> undo the last selection change.
    const char* sym = rwi->GetKeySym();
    if (rwi->GetControlKey() && sym && (std::strcmp(sym, "z") == 0 || std::strcmp(sym, "Z") == 0))
    {
      if (!c->undo.empty())
      {
        c->selection = std::set<size_t>(c->undo.back().begin(), c->undo.back().end());
        c->undo.pop_back();
        rebuildHighlight(c);
        rwi->Render();
        emitSelection(c);
      }
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

  int f3d_ext_enable_rubber_band_pick(
    f3d_window_t* window, f3d_ext_pick_callback_t cb, void* user_data)
  {
    vtkRenderWindowInteractor* rwi = interactor_of(window);
    vtkRenderer* ren = renderer_of(window);
    if (!rwi || !ren)
    {
      return 0;
    }

    RubberCtx& c = registry()[window];
    if (c.cmd) // re-enable: drop previous observers + overlays
    {
      for (unsigned long t : c.tags)
      {
        if (t)
        {
          rwi->RemoveObserver(t);
        }
      }
      if (c.renderer && c.box)
      {
        c.renderer->RemoveViewProp(c.box);
      }
      if (c.renderer && c.highlight)
      {
        c.renderer->RemoveViewProp(c.highlight);
      }
    }

    // Display-space rectangle outline overlay.
    vtkNew<vtkPoints> pts;
    pts->SetNumberOfPoints(4);
    vtkNew<vtkCellArray> lines;
    const vtkIdType loop[5] = { 0, 1, 2, 3, 0 };
    lines->InsertNextCell(5, loop);
    vtkNew<vtkPolyData> pd;
    pd->SetPoints(pts);
    pd->SetLines(lines);
    vtkNew<vtkCoordinate> coord;
    coord->SetCoordinateSystemToDisplay();
    vtkNew<vtkPolyDataMapper2D> mapper;
    mapper->SetInputData(pd);
    mapper->SetTransformCoordinate(coord);
    vtkNew<vtkActor2D> box;
    box->SetMapper(mapper);
    box->GetProperty()->SetColor(1.0, 1.0, 0.0);
    box->GetProperty()->SetLineWidth(1.5);
    box->SetVisibility(0);
    ren->AddViewProp(box);

    // Red highlight overlay for the selected points (drawn on top).
    vtkNew<vtkPoints> hlpts;
    vtkNew<vtkPolyData> hlpoly;
    hlpoly->SetPoints(hlpts);
    vtkNew<vtkPolyDataMapper> hlmapper;
    hlmapper->SetInputData(hlpoly);
    vtkNew<vtkActor> highlight;
    highlight->SetMapper(hlmapper);
    highlight->GetProperty()->SetColor(1.0, 0.0, 0.0);
    highlight->GetProperty()->SetPointSize(10.0);
    highlight->GetProperty()->SetRepresentationToPoints();
    highlight->GetProperty()->LightingOff();
    highlight->PickableOff();
    highlight->SetVisibility(0);
    ren->AddViewProp(highlight);

    c.window = window;
    c.rwi = rwi;
    c.renderer = ren;
    c.cb = cb;
    c.user = user_data;
    c.box = box;
    c.boxPts = pts;
    c.highlight = highlight;
    c.hlPts = hlpts;
    c.hlPoly = hlpoly;
    c.selecting = false;
    c.selection.clear();
    c.undo.clear();

    vtkNew<vtkCallbackCommand> cmd;
    cmd->SetCallback(RubberCB);
    cmd->SetClientData(&c);
    c.cmd = cmd;

    c.tags[0] = rwi->AddObserver(vtkCommand::RightButtonPressEvent, cmd, 10.0);
    c.tags[1] = rwi->AddObserver(vtkCommand::MouseMoveEvent, cmd, 10.0);
    c.tags[2] = rwi->AddObserver(vtkCommand::RightButtonReleaseEvent, cmd, 10.0);
    c.tags[3] = rwi->AddObserver(vtkCommand::KeyPressEvent, cmd, 10.0);
    return 1;
  }

  void f3d_ext_disable_rubber_band_pick(f3d_window_t* window)
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
      if (it->second.renderer && it->second.box)
      {
        it->second.renderer->RemoveViewProp(it->second.box);
      }
      if (it->second.renderer && it->second.highlight)
      {
        it->second.renderer->RemoveViewProp(it->second.highlight);
      }
      registry().erase(it);
    }
  }

} // extern "C"
