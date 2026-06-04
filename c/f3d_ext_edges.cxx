/**
 * f3d_ext_edges.cxx — per-actor and per-cell edge (wireframe) control (gap #6).
 *
 * Stock libf3d's `render.show_edges` is GLOBAL: edges are drawn on every imported
 * actor or none. Two features here, both reached through the f3d_ext renderer /
 * meta-importer hatch:
 *
 *  - f3d_ext_set_edge_visibility(): show/hide a coloured wireframe for ONE imported
 *    actor (or all), addressed by index into the coloring-actor list.
 *  - f3d_ext_add_cell_edges(): a wireframe over a SUBSET of one actor's cells.
 *
 * Both draw a SEPARATE flat-shaded (LightingOff) wireframe actor over the target
 * geometry rather than toggling the imported actor's own vtkProperty::EdgeVisibility.
 * f3d configures the coloring actors as PBR (SetInterpolationToPBR), and the native
 * edge pass ignores the property's EdgeColor under PBR — edges come out a dim, lit
 * grey with no colour control. A dedicated LightingOff wireframe actor gives a crisp,
 * caller-chosen colour and is untouched by f3d's per-render option push. The overlay
 * shares the source points and mirrors the source actor's transform at call time, so
 * it lines up with a model_scale exaggeration (re-call after the scale changes).
 *
 * Per-window registries keep the actors so they can be toggled / removed:
 *   - one wireframe per actor index for f3d_ext_set_edge_visibility (re-call replaces);
 *   - an id-keyed map for f3d_ext_add_cell_edges (remove one / clear all).
 *
 * BUILD: same c_api target as the other f3d_ext sources. Uses only CommonDataModel +
 * RenderingCore (already linked) — the cell subset is built by hand (GetCellPoints),
 * so no FiltersExtraction module is needed.
 */

#include "f3d_ext.h"

#include "window.h"      // f3d::window (public)
#include "window_impl.h" // f3d::detail::window_impl (PRIVATE)

#include "vtkF3DMetaImporter.h" // imported coloring actors (PRIVATE vtkext module)
#include "vtkF3DRenderer.h"     // f3d's concrete renderer (PRIVATE vtkext module)

#include <vtkActor.h>
#include <vtkCellArray.h>
#include <vtkIdList.h>
#include <vtkNew.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkRendererCollection.h>
#include <vtkSmartPointer.h>

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

vtkF3DMetaImporter* importer_of(f3d_window_t* window)
{
  vtkF3DRenderer* fren = vtkF3DRenderer::SafeDownCast(renderer_of(window));
  return fren ? fren->GetMetaImporter() : nullptr;
}

// Build a flat-shaded wireframe actor from a source actor's polydata. If cell_ids is
// null, ALL cells are outlined; otherwise just the listed (in-range) cells. Shares the
// source points and mirrors the source actor's transform. Returns null on bad input.
vtkSmartPointer<vtkActor> build_wireframe(vtkActor* srcActor, vtkPolyDataMapper* srcMapper,
  const size_t* cell_ids, size_t n_cells, double r, double g, double b, double width)
{
  vtkPolyData* src = srcMapper ? vtkPolyData::SafeDownCast(srcMapper->GetInput()) : nullptr;
  if (!src || !src->GetPoints())
  {
    return nullptr;
  }

  vtkNew<vtkCellArray> polys;
  vtkNew<vtkIdList> ptIds;
  const vtkIdType nSrcCells = src->GetNumberOfCells();
  if (cell_ids)
  {
    for (size_t i = 0; i < n_cells; ++i)
    {
      const vtkIdType cid = static_cast<vtkIdType>(cell_ids[i]);
      if (cid < 0 || cid >= nSrcCells)
      {
        continue;
      }
      src->GetCellPoints(cid, ptIds);
      polys->InsertNextCell(ptIds);
    }
  }
  else
  {
    for (vtkIdType cid = 0; cid < nSrcCells; ++cid)
    {
      src->GetCellPoints(cid, ptIds);
      polys->InsertNextCell(ptIds);
    }
  }
  if (polys->GetNumberOfCells() == 0)
  {
    return nullptr;
  }

  vtkNew<vtkPolyData> wf;
  wf->SetPoints(src->GetPoints()); // share points
  wf->SetPolys(polys);

  vtkNew<vtkPolyDataMapper> mapper;
  mapper->SetInputData(wf);
  mapper->ScalarVisibilityOff();
  // Pull the wireframe toward the camera so it is not lost to z-fighting on the surface.
  mapper->SetResolveCoincidentTopologyToPolygonOffset();
  mapper->SetRelativeCoincidentTopologyLineOffsetParameters(-1.0, -1.0);
  mapper->SetRelativeCoincidentTopologyPolygonOffsetParameters(-1.0, -1.0);

  vtkSmartPointer<vtkActor> actor = vtkSmartPointer<vtkActor>::New();
  actor->SetMapper(mapper);
  vtkProperty* p = actor->GetProperty();
  p->SetRepresentationToWireframe();
  const bool haveColor = (r >= 0.0 && g >= 0.0 && b >= 0.0);
  p->SetColor(haveColor ? r : 1.0, haveColor ? g : 1.0, haveColor ? b : 1.0);
  p->SetLineWidth(width > 0.0 ? width : 1.0);
  p->LightingOff();
  actor->PickableOff();

  if (srcActor)
  {
    double v[3];
    srcActor->GetScale(v);
    actor->SetScale(v);
    srcActor->GetPosition(v);
    actor->SetPosition(v);
    srcActor->GetOrientation(v);
    actor->SetOrientation(v);
    srcActor->GetOrigin(v);
    actor->SetOrigin(v);
  }
  return actor;
}

// Per-actor edge wireframes: window -> (actor index -> overlay actor).
std::map<f3d_window_t*, std::map<int, vtkSmartPointer<vtkActor>>>& edgeRegistry()
{
  static std::map<f3d_window_t*, std::map<int, vtkSmartPointer<vtkActor>>> r;
  return r;
}

// Cell-subset wireframes: window -> (id -> overlay actor).
struct CellEdgeCtx
{
  int counter = 0;
  std::map<int, vtkSmartPointer<vtkActor>> actors;
};
std::map<f3d_window_t*, CellEdgeCtx>& cellRegistry()
{
  static std::map<f3d_window_t*, CellEdgeCtx> r;
  return r;
}
} // namespace

extern "C"
{

  int f3d_ext_set_edge_visibility(
    f3d_window_t* window, int actor_index, int on, double r, double g, double b, double width)
  {
    vtkRenderer* ren = renderer_of(window);
    vtkF3DMetaImporter* imp = importer_of(window);
    if (!ren || !imp)
    {
      return 0;
    }

    const auto& coloring = imp->GetColoringActorsAndMappers();
    const int n = static_cast<int>(coloring.size());
    if (actor_index >= n || (actor_index < 0 && actor_index != -1))
    {
      return 0;
    }

    auto& perActor = edgeRegistry()[window];
    int applied = 0;
    for (int i = 0; i < n; ++i)
    {
      if (actor_index != -1 && i != actor_index)
      {
        continue;
      }
      // Replace any existing wireframe for this actor.
      auto it = perActor.find(i);
      if (it != perActor.end())
      {
        ren->RemoveViewProp(it->second);
        perActor.erase(it);
      }
      if (on)
      {
        vtkSmartPointer<vtkActor> wf = build_wireframe(
          coloring[i].Actor, coloring[i].Mapper, nullptr, 0, r, g, b, width);
        if (wf)
        {
          ren->AddViewProp(wf);
          perActor[i] = wf;
        }
      }
      ++applied;
    }
    return applied;
  }

  int f3d_ext_add_cell_edges(f3d_window_t* window, int actor_index, const size_t* cell_ids,
    size_t n_cells, double r, double g, double b, double width)
  {
    vtkRenderer* ren = renderer_of(window);
    vtkF3DMetaImporter* imp = importer_of(window);
    if (!ren || !imp || actor_index < 0 || !cell_ids || n_cells == 0)
    {
      return 0;
    }
    const auto& coloring = imp->GetColoringActorsAndMappers();
    if (actor_index >= static_cast<int>(coloring.size()))
    {
      return 0;
    }

    vtkSmartPointer<vtkActor> actor = build_wireframe(
      coloring[actor_index].Actor, coloring[actor_index].Mapper, cell_ids, n_cells, r, g, b, width);
    if (!actor)
    {
      return 0;
    }
    ren->AddViewProp(actor);

    CellEdgeCtx& ctx = cellRegistry()[window];
    const int id = ++ctx.counter;
    ctx.actors[id] = actor;
    return id;
  }

  void f3d_ext_remove_cell_edges(f3d_window_t* window, int id)
  {
    auto it = cellRegistry().find(window);
    if (it == cellRegistry().end())
    {
      return;
    }
    auto ait = it->second.actors.find(id);
    if (ait != it->second.actors.end())
    {
      if (vtkRenderer* ren = renderer_of(window))
      {
        ren->RemoveViewProp(ait->second);
      }
      it->second.actors.erase(ait);
    }
  }

  void f3d_ext_clear_cell_edges(f3d_window_t* window)
  {
    auto it = cellRegistry().find(window);
    if (it == cellRegistry().end())
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
    cellRegistry().erase(it);
  }

} // extern "C"
