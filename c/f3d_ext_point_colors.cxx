/**
 * f3d_ext_point_colors.cxx — colour + round shape for point clouds (gap #9).
 *
 * Two unrelated point-cloud fixes the stock API cannot do, both reached through the
 * f3d_ext renderer / meta-importer hatch:
 *
 *  - f3d_ext_color_point_sprites(): point SPRITES use vtkPointGaussianMapper, which
 *    ignores texture coordinates — so the 1xN palette-texture + per-point u-texcoord
 *    trick that colours plain GL_POINTS leaves every splat flat grey. This bakes a
 *    per-point RGB(A) unsigned-char colour array straight onto the sprite polydata,
 *    switches the mapper to direct scalar colours, and turns Emissive ON so the
 *    colour shows at full strength (f3d sets it off, which makes the splats read very
 *    dim). The splat SHAPE is the stock model.point_sprites.type option. NOTE: f3d's
 *    custom point-splat mapper ignores vtkPointGaussianMapper::SetSplatShaderCode set
 *    after the first render, so the splat shape cannot be overridden from here.
 *
 *  - f3d_ext_round_points(): the PLAIN points path (point_sprites disabled) honours
 *    the palette texture (colour-by-value works) but renders SQUARE GL_POINTS. Turning
 *    on vtkProperty::RenderPointsAsSpheres makes them round; LightingOff keeps the
 *    disc a flat, full-strength colour (no 3D shading) — i.e. round, flat, coloured
 *    points without the gaussian mapper at all.
 *
 * BUILD: same c_api target as the other f3d_ext sources.
 */

#include "f3d_ext.h"

#include "window.h"      // f3d::window (public)
#include "window_impl.h" // f3d::detail::window_impl (PRIVATE)

#include "vtkF3DMetaImporter.h" // imported point actors (PRIVATE vtkext module)
#include "vtkF3DRenderer.h"     // f3d's concrete renderer (PRIVATE vtkext module)

#include <vtkActor.h>
#include <vtkPointData.h>
#include <vtkPointGaussianMapper.h>
#include <vtkPolyData.h>
#include <vtkProperty.h>
#include <vtkRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkRendererCollection.h>
#include <vtkUnsignedCharArray.h>

#include <cstring>

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

vtkF3DMetaImporter* importer_of(f3d_window_t* window)
{
  f3d::detail::window_impl* impl = impl_of(window);
  if (!impl || !impl->GetRenderWindow())
  {
    return nullptr;
  }
  vtkRendererCollection* rens = impl->GetRenderWindow()->GetRenderers();
  vtkRenderer* ren = rens ? rens->GetFirstRenderer() : nullptr;
  vtkF3DRenderer* fren = vtkF3DRenderer::SafeDownCast(ren);
  return fren ? fren->GetMetaImporter() : nullptr;
}
} // namespace

extern "C"
{

  int f3d_ext_color_point_sprites(
    f3d_window_t* window, const unsigned char* rgb, size_t n_points, int n_comp)
  {
    if (!rgb || n_points == 0 || (n_comp != 3 && n_comp != 4))
    {
      return 0;
    }
    vtkF3DMetaImporter* imp = importer_of(window);
    if (!imp)
    {
      return 0;
    }

    int applied = 0;
    for (const auto& ps : imp->GetPointSpritesActorsAndMappers())
    {
      vtkPointGaussianMapper* m = ps.Mapper;
      if (!m)
      {
        continue;
      }
      vtkPolyData* pd = vtkPolyData::SafeDownCast(m->GetInput());
      if (!pd || static_cast<size_t>(pd->GetNumberOfPoints()) != n_points)
      {
        continue; // not the matching point cloud
      }

      vtkNew<vtkUnsignedCharArray> colors;
      colors->SetNumberOfComponents(n_comp);
      colors->SetName("f3d_ext_point_colors");
      colors->SetNumberOfTuples(static_cast<vtkIdType>(n_points));
      std::memcpy(colors->GetPointer(0), rgb, n_points * static_cast<size_t>(n_comp));
      pd->GetPointData()->SetScalars(colors);

      // Direct per-point colours, shown at full strength (f3d sets Emissive off, which
      // makes the lit splat read very dim). f3d's ConfigurePointSprites runs once
      // (gated by a flag) so these mapper settings persist across the per-render push.
      m->SetScalarVisibility(true);
      m->SetScalarModeToUsePointData();
      m->SetColorModeToDirectScalars();
      m->EmissiveOn();
      applied = 1;
    }
    return applied;
  }

  int f3d_ext_round_points(f3d_window_t* window, int on, int unlit)
  {
    vtkF3DMetaImporter* imp = importer_of(window);
    if (!imp)
    {
      return 0;
    }
    int applied = 0;
    // The plain-points geometry lives on the "coloring" actors (point_sprites off).
    for (const auto& cs : imp->GetColoringActorsAndMappers())
    {
      if (!cs.Actor)
      {
        continue;
      }
      vtkProperty* p = cs.Actor->GetProperty();
      p->SetRenderPointsAsSpheres(on != 0);
      if (on)
      {
        // Flat, full-strength colour disc: no 3D sphere shading.
        unlit ? p->LightingOff() : p->LightingOn();
      }
      applied = 1;
    }
    return applied;
  }

} // extern "C"
