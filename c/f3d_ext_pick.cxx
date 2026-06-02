/**
 * f3d_ext_pick.cxx — implementation of the f3d_ext area-pick + renderer escape hatch.
 *
 * BUILD (must compile inside the f3d tree so the private header resolves).
 * The C-API target in c/CMakeLists.txt is `c_api` (links `libf3d` PRIVATE, so VTK
 * is NOT transitively visible and must be added explicitly). Append to that file:
 *
 *     # 1. add the source + (optional) installed header
 *     list(APPEND F3D_C_SOURCE_FILES   ${CMAKE_CURRENT_SOURCE_DIR}/f3d_ext_pick.cxx)
 *     list(APPEND F3D_C_PUBLIC_HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/f3d_ext.h)
 *     #    (or, after add_library:  target_sources(c_api PRIVATE f3d_ext_pick.cxx) )
 *
 *     # 2. private headers (window_impl.h & its includes) + VTK modules used here
 *     target_include_directories(c_api PRIVATE ${CMAKE_SOURCE_DIR}/library/private)
 *     find_package(VTK COMPONENTS RenderingCore RenderingOpenGL2 CommonCore CommonDataModel)
 *     target_link_libraries(c_api PRIVATE
 *       VTK::RenderingCore VTK::RenderingOpenGL2 VTK::CommonCore VTK::CommonDataModel)
 *
 * F3D_EXPORT already resolves to dllexport here (c_api defines libf3d_EXPORTS), so
 * the f3d_ext_* symbols land in f3d_c_api.dll alongside the stock C API.
 */

#include "f3d_ext.h"

#include "window.h"        // f3d::window  (public)
#include "window_impl.h"   // f3d::detail::window_impl  (PRIVATE)

#include <vtkActor.h>
#include <vtkActorCollection.h>
#include <vtkDataSet.h>
#include <vtkExtractSelectedFrustum.h>
#include <vtkIdTypeArray.h>
#include <vtkMapper.h>
#include <vtkNew.h>
#include <vtkPlanes.h>
#include <vtkPointData.h>
#include <vtkRenderWindow.h>
#include <vtkRenderedAreaPicker.h>
#include <vtkRenderer.h>
#include <vtkRendererCollection.h>
#include <vtkSmartPointer.h>

#include <algorithm>
#include <cstdlib>
#include <vector>

namespace
{
// f3d_window_t* is a reinterpret_cast of f3d::window* (see engine_c_api.cxx). The
// concrete object engine creates is always a window_impl, so the downcast is safe.
f3d::detail::window_impl* impl_of(f3d_window_t* window)
{
  if (!window)
  {
    return nullptr;
  }
  f3d::window* win = reinterpret_cast<f3d::window*>(window);
  return static_cast<f3d::detail::window_impl*>(win);
}

vtkRenderer* renderer_of(f3d_window_t* window)
{
  f3d::detail::window_impl* impl = impl_of(window);
  if (!impl)
  {
    return nullptr;
  }
  vtkRenderWindow* rw = impl->GetRenderWindow();
  if (!rw)
  {
    return nullptr;
  }
  return rw->GetRenderers() ? rw->GetRenderers()->GetFirstRenderer() : nullptr;
}
} // namespace

extern "C"
{

  size_t* f3d_ext_area_pick_points(
    f3d_window_t* window, int x0, int y0, int x1, int y1, size_t* count)
  {
    if (count)
    {
      *count = 0;
    }
    vtkRenderer* ren = renderer_of(window);
    if (!ren || !count)
    {
      return nullptr;
    }

    const double xmin = std::min(x0, x1);
    const double ymin = std::min(y0, y1);
    const double xmax = std::max(x0, x1);
    const double ymax = std::max(y0, y1);

    // Geometric frustum pick: works regardless of f3d's custom render passes (a
    // GPU vtkHardwareSelector returns nothing through them). CPU-side, it selects
    // every point inside the dragged rectangle's view frustum (occluded included).
    vtkNew<vtkRenderedAreaPicker> picker;
    picker->AreaPick(xmin, ymin, xmax, ymax, ren);
    vtkPlanes* frustum = picker->GetFrustum();
    if (!frustum)
    {
      return nullptr;
    }

    std::vector<size_t> ids;
    vtkActorCollection* actors = ren->GetActors();
    if (actors)
    {
      actors->InitTraversal();
      vtkActor* actor = nullptr;
      while ((actor = actors->GetNextActor()))
      {
        if (!actor->GetPickable() || !actor->GetVisibility() || !actor->GetMapper())
        {
          continue;
        }
        vtkDataSet* ds = vtkDataSet::SafeDownCast(actor->GetMapper()->GetInput());
        if (!ds || ds->GetNumberOfPoints() == 0)
        {
          continue;
        }
        vtkNew<vtkExtractSelectedFrustum> ext;
        ext->SetFieldType(0); // 0 = vtkSelectionNode::POINT
        ext->PreserveTopologyOff();
        ext->SetFrustum(frustum);
        ext->SetInputData(ds);
        ext->Update();
        vtkDataSet* out = vtkDataSet::SafeDownCast(ext->GetOutput());
        if (!out || !out->GetPointData())
        {
          continue;
        }
        vtkIdTypeArray* orig =
          vtkIdTypeArray::SafeDownCast(out->GetPointData()->GetArray("vtkOriginalPointIds"));
        if (!orig)
        {
          continue;
        }
        const vtkIdType nv = orig->GetNumberOfValues();
        ids.reserve(ids.size() + static_cast<size_t>(nv));
        for (vtkIdType i = 0; i < nv; ++i)
        {
          ids.push_back(static_cast<size_t>(orig->GetValue(i)));
        }
      }
    }

    if (ids.empty())
    {
      return nullptr;
    }

    // Hand back a malloc'd array (freed by f3d_ext_free_ids -> free()).
    size_t* out = static_cast<size_t*>(std::malloc(ids.size() * sizeof(size_t)));
    if (!out)
    {
      return nullptr;
    }
    std::copy(ids.begin(), ids.end(), out);
    *count = ids.size();
    return out;
  }

  void f3d_ext_free_ids(size_t* ids)
  {
    std::free(ids);
  }

  void* f3d_ext_get_renderer(f3d_window_t* window)
  {
    return static_cast<void*>(renderer_of(window));
  }

  void* f3d_ext_get_render_window(f3d_window_t* window)
  {
    f3d::detail::window_impl* impl = impl_of(window);
    return impl ? static_cast<void*>(impl->GetRenderWindow()) : nullptr;
  }

} // extern "C"
