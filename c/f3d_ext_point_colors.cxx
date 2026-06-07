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

#include "options.h"     // f3d::options (public)
#include "window.h"      // f3d::window (public)
#include "window_impl.h" // f3d::detail::window_impl (PRIVATE)

#include "vtkF3DMetaImporter.h" // imported point actors (PRIVATE vtkext module)
#include "vtkF3DRenderer.h"     // f3d's concrete renderer (PRIVATE vtkext module)

#include <vtkActor.h>
#include <vtkCallbackCommand.h>
#include <vtkCommand.h>
#include <vtkDataArray.h>
#include <vtkMapper.h>
#include <vtkNew.h>
#include <vtkPointData.h>
#include <vtkPointGaussianMapper.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkProperty.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkRenderer.h>
#include <vtkRendererCollection.h>
#include <vtkSmartPointer.h>
#include <vtkUnsignedCharArray.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

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

// Per-window state for the Shift+'+' / Shift+'-' point resize keys. Targets the live
// representation: `model.point_sprites.size` when sprites are on, else `render.point_size`
// for a plain point cloud; the 'o' key cycles the sprite TYPE but never the size, so this
// gives the missing live size control for BOTH modes.
struct SpriteKeyCtx
{
  f3d::window* window = nullptr;   // for render() (re-applies the option to the renderer)
  f3d::options* options = nullptr; // owns model.point_sprites.size
  vtkRenderWindowInteractor* rwi = nullptr;
  vtkSmartPointer<vtkCallbackCommand> keyCmd;
  unsigned long keyTag = 0;
  double size = 10.0;   // current sprite size (model.point_sprites.size)
  double psize = 1.0;   // current plain-point size (render.point_size)
  double factor = 1.25; // multiply / divide per key press
};
std::map<f3d_window_t*, SpriteKeyCtx>& spriteKeyRegistry()
{
  static std::map<f3d_window_t*, SpriteKeyCtx> r;
  return r;
}

// Shift + '+' grows / Shift + '-' shrinks the sprites. Higher priority than the style and
// aborts when it acts, so plain (un-shifted) '+'/'-' keep their f3d meaning.
void SpriteKeyCB(vtkObject* caller, unsigned long, void* clientData, void*)
{
  SpriteKeyCtx* c = static_cast<SpriteKeyCtx*>(clientData);
  vtkRenderWindowInteractor* rwi = vtkRenderWindowInteractor::SafeDownCast(caller);
  if (!c || !rwi)
  {
    return;
  }
  if (c->keyCmd)
  {
    c->keyCmd->SetAbortFlagOnExecute(0); // clear any abort from a prior handled press
  }
  // Require Shift so plain '+' / '-' stay free (the user keeps those for zoom). Match on the
  // SHIFTED character the layout produces: on a US layout Shift+'=' -> '+', on a PT layout
  // the '+' key is unshifted so Shift+'+' -> '*'; Shift+'-' -> '_' on both. Accept the
  // ASCII KeyCode and the X keysym.
  if (!c->options || !c->window || !rwi->GetShiftKey())
  {
    return;
  }
  const char kc = rwi->GetKeyCode();
  const char* key = rwi->GetKeySym();
  const bool inc = kc == '*' || kc == '+' ||
    (key && (std::strcmp(key, "asterisk") == 0 || std::strcmp(key, "plus") == 0 ||
              std::strcmp(key, "equal") == 0));
  const bool dec = kc == '_' ||
    (key && (std::strcmp(key, "underscore") == 0 || std::strcmp(key, "minus") == 0));
  if (!inc && !dec)
  {
    return;
  }
  // Resize whichever representation is live: sprites (model.point_sprites.size) when the
  // point-sprite mode is on, else the plain point cloud (render.point_size). Without this the
  // keys looked dead on a plain cloud (the default) because point_sprites.size is ignored
  // until sprites are enabled.
  bool sprites = false;
  try
  {
    const std::string en = c->options->getAsString("model.point_sprites.enable");
    sprites = (en == "true" || en == "1");
  }
  catch (...)
  {
  }
  if (sprites)
  {
    c->size = std::clamp(inc ? c->size * c->factor : c->size / c->factor, 0.05, 1000.0);
    c->options->set("model.point_sprites.size", c->size);
  }
  else
  {
    c->psize = std::clamp(inc ? c->psize * c->factor : c->psize / c->factor, 0.1, 1000.0);
    c->options->set("render.point_size", c->psize);
  }
  c->window->render();
  if (c->keyCmd)
  {
    c->keyCmd->SetAbortFlagOnExecute(1);
  }
}

// ---- Vertical-scale sync for point sprites ---------------------------------------------
// f3d applies render.model_scale as an ACTOR transform, which is fine for the plain-points
// representation but anisotropically distorts gaussian/sphere/circle splats (the splat
// covariance picks up the model-view scale). Instead of scaling the sprite actor, we bake
// the current model_scale into the sprite POINT COORDINATES (from a cached original), so the
// sprites sit at the stretched locations while staying perfectly round. Re-applied when the
// 'o' key cycles into a sprite mode (and once on enable).
struct SpriteZCtx
{
  f3d_window_t* whandle = nullptr;
  f3d::window* window = nullptr;
  f3d::options* options = nullptr;
  vtkRenderWindowInteractor* rwi = nullptr;
  vtkRenderer* renderer = nullptr;
  vtkSmartPointer<vtkCallbackCommand> keyCmd;
  unsigned long keyTag = 0;
  vtkSmartPointer<vtkCallbackCommand> renderCmd; // StartEvent: auto re-bake on scale change
  unsigned long renderTag = 0;
  vtkSmartPointer<vtkCallbackCommand> endCmd; // EndEvent: re-assert sprite colours
  unsigned long endTag = 0;
  bool inReassert = false; // guards the corrective re-render against recursion
  double lastScale[3] = { 1.0, 1.0, 1.0 }; // last baked model_scale (change detection)
  std::map<vtkPolyData*, vtkSmartPointer<vtkPoints>> orig; // unscaled point cache
  std::map<vtkPolyData*, vtkSmartPointer<vtkUnsignedCharArray>> colorOrig; // gap#9 colour cache
};
std::map<f3d_window_t*, SpriteZCtx>& spriteZRegistry()
{
  static std::map<f3d_window_t*, SpriteZCtx> r;
  return r;
}

// Master per-point colour cache, keyed by sprite polydata. Seeded by
// f3d_ext_color_point_sprites the moment colours are applied — INCLUDING the case where the
// cloud starts as plain points (sprites disabled): the sprite polydata exists but is hidden,
// so the colour array is set on it now and stashed here. When the user later presses 'o' to
// enable sprites, f3d's ConfigurePointSprites wipes the array before reassertSpriteColors can
// see it visible (so its own colorOrig cache stays empty) — reassert then falls back to THIS
// cache to restore the colours. Without it, point-cloud-start + 'o' shows grey splats.
std::map<vtkPolyData*, vtkSmartPointer<vtkUnsignedCharArray>>& spriteColorMaster()
{
  static std::map<vtkPolyData*, vtkSmartPointer<vtkUnsignedCharArray>> r;
  return r;
}

// Read up to three doubles from the render.model_scale string ("sx, sy, sz"); missing
// components default to 1.
void readModelScale(f3d::options* options, double s[3])
{
  s[0] = s[1] = s[2] = 1.0;
  if (!options)
  {
    return;
  }
  std::string str;
  try
  {
    str = options->getAsString("render.model_scale");
  }
  catch (...)
  {
    return; // option not set yet -> identity
  }
  const char* p = str.c_str();
  for (int i = 0; i < 3 && *p;)
  {
    while (*p && !(std::isdigit((unsigned char)*p) || *p == '-' || *p == '+' || *p == '.'))
    {
      ++p;
    }
    if (!*p)
    {
      break;
    }
    char* end = nullptr;
    const double v = std::strtod(p, &end);
    if (end == p)
    {
      break;
    }
    s[i++] = v;
    p = end;
  }
}

// Bake model_scale into the sprite point coordinates (z = z0 * sz, from the cached original).
// Only touches VISIBLE sprite actors (skips work when in plain-points mode). Does NOT render
// -- the caller decides (so it is safe to call from a render StartEvent). Updates lastScale.
void bakeSpriteZ(SpriteZCtx& c)
{
  vtkF3DMetaImporter* imp = importer_of(c.whandle);
  if (!imp || !c.options)
  {
    return;
  }
  double s[3];
  readModelScale(c.options, s);
  for (const auto& ps : imp->GetPointSpritesActorsAndMappers())
  {
    vtkPointGaussianMapper* m = ps.Mapper;
    if (!m || (ps.Actor && !ps.Actor->GetVisibility()))
    {
      continue; // hidden -> plain-points mode, nothing to place
    }
    vtkPolyData* pd = vtkPolyData::SafeDownCast(m->GetInput());
    if (!pd || !pd->GetPoints())
    {
      continue;
    }
    vtkPoints* pts = pd->GetPoints();
    auto it = c.orig.find(pd);
    if (it == c.orig.end())
    {
      vtkSmartPointer<vtkPoints> o = vtkSmartPointer<vtkPoints>::New();
      o->DeepCopy(pts); // capture the unscaled (static-baked) coordinates once
      it = c.orig.emplace(pd, o).first;
    }
    vtkPoints* o = it->second;
    const vtkIdType n = o->GetNumberOfPoints();
    for (vtkIdType k = 0; k < n; ++k)
    {
      double q[3];
      o->GetPoint(k, q);
      pts->SetPoint(k, q[0] * s[0], q[1] * s[1], q[2] * s[2]);
    }
    pts->Modified();
    pd->Modified();
  }
  c.lastScale[0] = s[0];
  c.lastScale[1] = s[1];
  c.lastScale[2] = s[2];
}

// Bake + render: used on enable and on the 'o' toggle (not inside a render pass).
void syncSpriteZ(SpriteZCtx& c)
{
  bakeSpriteZ(c);
  if (c.window)
  {
    c.window->render();
  }
}

// Renderer StartEvent: auto re-place the sprites whenever the vertical scale changed since
// the last bake (i.e. during a vertical-scale drag), so no manual points<->sprites toggle is
// needed. Cheap when idle (the scale-equality check short-circuits); does NOT call render --
// it edits the points before this same frame draws them.
void SpriteZRenderCB(vtkObject*, unsigned long, void* clientData, void*)
{
  SpriteZCtx* c = static_cast<SpriteZCtx*>(clientData);
  if (!c || !c->options)
  {
    return;
  }
  double s[3];
  readModelScale(c->options, s);
  if (s[0] != c->lastScale[0] || s[1] != c->lastScale[1] || s[2] != c->lastScale[2])
  {
    bakeSpriteZ(*c); // updates lastScale; no render (this StartEvent is already a render)
  }
}

// Re-assert the gap#9 direct-scalar colours on the sprite mappers. f3d's ConfigurePointSprites
// runs EmissiveOff() on every 'o' type change, which drops the per-point colours to near-black;
// re-enable Emissive + the direct-scalar mode whenever our colour array is present but the flags
// were reset. Returns true if anything was changed (so the caller can request a redraw).
bool reassertSpriteColors(SpriteZCtx& c)
{
  vtkF3DMetaImporter* imp = importer_of(c.whandle);
  if (!imp)
  {
    return false;
  }
  bool changed = false;
  for (const auto& ps : imp->GetPointSpritesActorsAndMappers())
  {
    vtkPointGaussianMapper* m = ps.Mapper;
    if (!m || (ps.Actor && !ps.Actor->GetVisibility()))
    {
      continue; // hidden (plain-points mode) -> nothing to colour
    }
    vtkPolyData* pd = vtkPolyData::SafeDownCast(m->GetInput());
    if (!pd)
    {
      continue;
    }
    vtkPointData* pdata = pd->GetPointData();
    const bool has = pdata->HasArray("f3d_ext_point_colors");
    if (has)
    {
      // First sighting: stash a copy so we can restore it later. f3d's coloring config DELETES
      // the array from the polydata when the 'o' cycle re-enables sprites (confirmed by trace).
      if (c.colorOrig.find(pd) == c.colorOrig.end())
      {
        vtkSmartPointer<vtkUnsignedCharArray> save = vtkSmartPointer<vtkUnsignedCharArray>::New();
        save->DeepCopy(pdata->GetArray("f3d_ext_point_colors"));
        save->SetName("f3d_ext_point_colors");
        c.colorOrig.emplace(pd, save);
      }
    }
    else
    {
      // Array was wiped: re-add it from the cache. Prefer our own colorOrig copy; fall back to
      // the master cache seeded by f3d_ext_color_point_sprites (covers the plain-points-start
      // case, where the array was set on the hidden actor and never seen here while visible).
      vtkUnsignedCharArray* src = nullptr;
      auto it = c.colorOrig.find(pd);
      if (it != c.colorOrig.end())
      {
        src = it->second;
      }
      else
      {
        auto mit = spriteColorMaster().find(pd);
        if (mit != spriteColorMaster().end())
        {
          src = mit->second;
        }
      }
      if (!src)
      {
        continue; // never coloured -> nothing to restore
      }
      vtkSmartPointer<vtkUnsignedCharArray> restored = vtkSmartPointer<vtkUnsignedCharArray>::New();
      restored->DeepCopy(src);
      restored->SetName("f3d_ext_point_colors");
      pdata->SetScalars(restored);
      changed = true;
    }

    // Re-assert the direct-scalar colouring mode (f3d may have reset these too).
    vtkDataArray* active = pdata->GetScalars();
    const char* aname = active ? active->GetName() : nullptr;
    const bool wrong = !m->GetScalarVisibility() ||
      m->GetColorMode() != VTK_COLOR_MODE_DIRECT_SCALARS ||
      m->GetScalarMode() != VTK_SCALAR_MODE_USE_POINT_DATA || !aname ||
      std::strcmp(aname, "f3d_ext_point_colors") != 0;
    if (wrong)
    {
      pdata->SetActiveScalars("f3d_ext_point_colors");
      m->SetScalarVisibility(true);
      m->SetScalarModeToUsePointData();
      m->SetColorModeToDirectScalars();
      m->EmissiveOn();
      changed = true;
    }
  }
  return changed;
}

// Renderer EndEvent: ConfigurePointSprites (which ran during this render's UpdateActors, AFTER
// the StartEvent) may have reset our colour flags. Re-assert them now (takes effect next frame)
// and trigger one corrective redraw, guarded against recursion.
void SpriteZEndCB(vtkObject*, unsigned long, void* clientData, void*)
{
  SpriteZCtx* c = static_cast<SpriteZCtx*>(clientData);
  if (!c || c->inReassert)
  {
    return;
  }
  if (reassertSpriteColors(*c) && c->window)
  {
    c->inReassert = true;
    c->window->render(); // redraw with colours restored
    c->inReassert = false;
  }
}

// 'o' cycles f3d's point-sprite TYPE; after it toggles, re-place the now-visible sprites at
// the current vertical scale (the scale itself did not change, so the StartEvent guard would
// otherwise skip it). Low priority, never aborts so f3d's own 'o' handler still runs.
void SpriteZKeyCB(vtkObject* caller, unsigned long, void* clientData, void*)
{
  SpriteZCtx* c = static_cast<SpriteZCtx*>(clientData);
  vtkRenderWindowInteractor* rwi = vtkRenderWindowInteractor::SafeDownCast(caller);
  if (!c || !rwi)
  {
    return;
  }
  const char* key = rwi->GetKeySym();
  const char kc = rwi->GetKeyCode();
  if (kc == 'o' || kc == 'O' || (key && (std::strcmp(key, "o") == 0 || std::strcmp(key, "O") == 0)))
  {
    syncSpriteZ(*c);
  }
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

      // Stash a copy so reassertSpriteColors can restore it after f3d wipes the array on the
      // first 'o' toggle (critical when the cloud started as plain points — see comment on
      // spriteColorMaster). DeepCopy: the live array may be replaced by f3d.
      {
        vtkSmartPointer<vtkUnsignedCharArray> save = vtkSmartPointer<vtkUnsignedCharArray>::New();
        save->DeepCopy(colors);
        save->SetName("f3d_ext_point_colors");
        spriteColorMaster()[pd] = save;
      }

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

  int f3d_ext_enable_sprite_size_keys(
    f3d_window_t* window, f3d_options_t* options, double size, double factor)
  {
    vtkRenderWindowInteractor* rwi = interactor_of(window);
    if (!window || !options || !rwi)
    {
      return 0;
    }
    SpriteKeyCtx& c = spriteKeyRegistry()[window];
    if (c.keyTag && c.rwi)
    {
      c.rwi->RemoveObserver(c.keyTag); // re-enable: drop the old observer
      c.keyTag = 0;
    }
    c.window = reinterpret_cast<f3d::window*>(window);
    c.options = reinterpret_cast<f3d::options*>(options);
    c.rwi = rwi;
    c.size = (size > 0.0) ? size : 10.0;
    c.factor = (factor > 1.0) ? factor : 1.25;
    // Seed the plain-point size from the live render.point_size (the Julia `pointsize` kwarg),
    // so the first Shift+'+' grows from what the user actually sees, not a hard-coded 1.0.
    try
    {
      c.psize = std::stod(c.options->getAsString("render.point_size"));
    }
    catch (...)
    {
      c.psize = 1.0;
    }
    if (c.psize <= 0.0)
    {
      c.psize = 1.0;
    }

    vtkNew<vtkCallbackCommand> keyCmd;
    keyCmd->SetCallback(SpriteKeyCB);
    keyCmd->SetClientData(&c);
    c.keyCmd = keyCmd;
    c.keyTag = rwi->AddObserver(vtkCommand::KeyPressEvent, keyCmd, 5.0);
    return 1;
  }

  void f3d_ext_disable_sprite_size_keys(f3d_window_t* window)
  {
    auto& reg = spriteKeyRegistry();
    auto it = reg.find(window);
    if (it == reg.end())
    {
      return;
    }
    if (it->second.keyTag && it->second.rwi)
    {
      it->second.rwi->RemoveObserver(it->second.keyTag);
    }
    reg.erase(it);
  }

  int f3d_ext_enable_sprite_zscale_sync(f3d_window_t* window, f3d_options_t* options)
  {
    vtkRenderWindowInteractor* rwi = interactor_of(window);
    if (!window || !options || !rwi)
    {
      return 0;
    }
    SpriteZCtx& c = spriteZRegistry()[window];
    if (c.keyTag && c.rwi)
    {
      c.rwi->RemoveObserver(c.keyTag);
      c.keyTag = 0;
    }
    if (c.renderTag && c.renderer)
    {
      c.renderer->RemoveObserver(c.renderTag);
      c.renderTag = 0;
    }
    if (c.endTag && c.renderer)
    {
      c.renderer->RemoveObserver(c.endTag);
      c.endTag = 0;
    }
    c.whandle = window;
    c.window = reinterpret_cast<f3d::window*>(window);
    c.options = reinterpret_cast<f3d::options*>(options);
    c.rwi = rwi;
    c.renderer = renderer_of(window);
    c.orig.clear(); // re-cache against the current sprite polydata

    vtkNew<vtkCallbackCommand> keyCmd;
    keyCmd->SetCallback(SpriteZKeyCB);
    keyCmd->SetClientData(&c);
    c.keyCmd = keyCmd;
    c.keyTag = rwi->AddObserver(vtkCommand::KeyPressEvent, keyCmd, -2.0);

    // Auto re-bake during a vertical-scale drag (no manual points<->sprites toggle needed).
    if (c.renderer)
    {
      vtkNew<vtkCallbackCommand> renderCmd;
      renderCmd->SetCallback(SpriteZRenderCB);
      renderCmd->SetClientData(&c);
      c.renderCmd = renderCmd;
      c.renderTag = c.renderer->AddObserver(vtkCommand::StartEvent, renderCmd);

      // Re-assert the gap#9 sprite colours after each render (ConfigurePointSprites resets
      // EmissiveOff on every 'o' type cycle, which would otherwise drop the per-point colours).
      vtkNew<vtkCallbackCommand> endCmd;
      endCmd->SetCallback(SpriteZEndCB);
      endCmd->SetClientData(&c);
      c.endCmd = endCmd;
      c.endTag = c.renderer->AddObserver(vtkCommand::EndEvent, endCmd);
    }

    syncSpriteZ(c); // place sprites at the current vertical scale now
    return 1;
  }

  void f3d_ext_disable_sprite_zscale_sync(f3d_window_t* window)
  {
    auto& reg = spriteZRegistry();
    auto it = reg.find(window);
    if (it == reg.end())
    {
      return;
    }
    if (it->second.keyTag && it->second.rwi)
    {
      it->second.rwi->RemoveObserver(it->second.keyTag);
    }
    if (it->second.renderTag && it->second.renderer)
    {
      it->second.renderer->RemoveObserver(it->second.renderTag);
    }
    if (it->second.endTag && it->second.renderer)
    {
      it->second.renderer->RemoveObserver(it->second.endTag);
    }
    // Drop this window's polydata from the master colour cache so a freed pd address can't be
    // reused by an unrelated cloud in a later view (keyed by raw pointer).
    for (const auto& kv : it->second.orig)
    {
      spriteColorMaster().erase(kv.first);
    }
    for (const auto& kv : it->second.colorOrig)
    {
      spriteColorMaster().erase(kv.first);
    }
    reg.erase(it);
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
