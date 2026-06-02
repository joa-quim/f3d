#ifndef F3D_EXT_C_API_H
#define F3D_EXT_C_API_H

/**
 * f3d_ext — out-of-tree C extensions to the libf3d C API.
 *
 * These reach libf3d internals (the concrete f3d::detail::window_impl, its
 * vtkRenderWindow / vtkRenderer) that the public API hides, to provide features
 * the stock API cannot: GPU rubber-band / area point picking, and a raw
 * renderer / render-window escape hatch for callers that link VTK and want to
 * attach their own actors (cube axes, text overlays, ...).
 *
 * Because they include the PRIVATE header window_impl.h, these files must be
 * COMPILED INSIDE the f3d source tree (add library/private to the include path,
 * link the f3d library target). See the CMake note at the top of f3d_ext_pick.cxx.
 */

#include "export.h"       // F3D_EXPORT
#include "options_c_api.h" // f3d_options_t
#include "window_c_api.h"  // f3d_window_t

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

  /**
   * @brief Area (rubber-band) pick: ids of the points inside a display-space box.
   *
   * Runs a GPU hardware selection on the window's renderer over the pixel
   * rectangle [x0,x1] x [y0,y1] (VTK display coords: origin BOTTOM-LEFT, +y up).
   * Order of the corners does not matter. Because it is a hardware selection,
   * points hidden behind nearer geometry are NOT returned (true visible pick).
   *
   * The returned ids are point ids within the rendered polydata. For a mesh added
   * straight from an f3d_mesh_t whose points are NOT vertex-split (e.g. a point
   * cloud), id == the original input point index. If several point-actors are in
   * the scene the ids of all of them are merged (use the renderer escape hatch and
   * a per-prop selection if you need to tell them apart).
   *
   * Render the window at least once before calling.
   *
   * @param window Window handle.
   * @param x0,y0,x1,y1 Rectangle corners in display pixels.
   * @param count Out: number of ids returned (0 on empty / error).
   * @return Heap array of `*count` point ids, or NULL if none / on error.
   *         Free it with f3d_ext_free_ids().
   */
  F3D_EXPORT size_t* f3d_ext_area_pick_points(
    f3d_window_t* window, int x0, int y0, int x1, int y1, size_t* count);

  /**
   * @brief Free an id array returned by f3d_ext_area_pick_points().
   */
  F3D_EXPORT void f3d_ext_free_ids(size_t* ids);

  /**
   * @brief Callback invoked after a rubber-band drag completes.
   *
   * @param ids   Point ids inside the dragged box (NULL if none); valid only for
   *              the duration of the call — copy what you need.
   * @param count Number of ids.
   * @param user_data The pointer passed to f3d_ext_enable_rubber_band_pick().
   */
  typedef void (*f3d_ext_pick_callback_t)(const size_t* ids, size_t count, void* user_data);

  /**
   * @brief Turn the window into a rubber-band point selector.
   *
   * Installs a rubber-band interactor style on the window's interactor. The user
   * presses 'r' to arm selection mode, then left-click-drags a box; on release the
   * points inside it are picked (via f3d_ext_area_pick_points) and @p cb is invoked
   * with their ids. 'r' again returns to normal camera interaction within the style;
   * call f3d_ext_disable_rubber_band_pick() to fully restore f3d's own style.
   *
   * Call AFTER the engine has an interactor (e.g. after f3d_engine_get_interactor)
   * and before f3d_interactor_start(). The previous style + picker are remembered
   * and restored on disable.
   *
   * @return 1 on success, 0 if the window has no interactor yet / on error.
   */
  F3D_EXPORT int f3d_ext_enable_rubber_band_pick(
    f3d_window_t* window, f3d_ext_pick_callback_t cb, void* user_data);

  /**
   * @brief Restore the interactor style + picker that were active before
   *        f3d_ext_enable_rubber_band_pick(). No-op if it was never enabled.
   */
  F3D_EXPORT void f3d_ext_disable_rubber_band_pick(f3d_window_t* window);

  /**
   * @brief Raw escape hatch: the window's internal vtkRenderer.
   *
   * Returned as void* (cast to vtkRenderer* in a TU that includes VTK). Use it to
   * AddActor a vtkCubeAxesActor / vtkTextActor / picker, etc. Lifetime is owned by
   * f3d — do not delete it. NULL on error.
   */
  F3D_EXPORT void* f3d_ext_get_renderer(f3d_window_t* window);

  /**
   * @brief Raw escape hatch: the window's internal vtkRenderWindow (as void*).
   *        Owned by f3d, do not delete. NULL on error.
   */
  F3D_EXPORT void* f3d_ext_get_render_window(f3d_window_t* window);

  /**
   * @brief Turn Ctrl + left-drag into a vertical-scale (exaggeration) gesture.
   *
   * Installs an interactor style that, while Ctrl is held, maps left-button
   * vertical drag to the model's Z scale (`render.model_scale` z component) via a
   * GPU transform — the underlying coordinates are unchanged. All other gestures
   * (rotate / pan / zoom, keys) keep their normal f3d behaviour. The exaggeration
   * is cumulative across drags and persists until disabled.
   *
   * Call AFTER the engine has an interactor (e.g. after f3d_engine_get_interactor)
   * and before f3d_interactor_start(). The previous style is remembered and
   * restored on disable.
   *
   * Sets the `render.model_scale` option (so the change survives f3d's per-render
   * option push), so pass the engine's options handle.
   *
   * @param window Window handle.
   * @param options Options handle (from f3d_engine_get_options) to update.
   * @param sensitivity Scale change per pixel of vertical drag (exp factor);
   *                    pass <= 0 for the default (0.01).
   * @return 1 on success, 0 if the window has no interactor yet.
   */
  F3D_EXPORT int f3d_ext_enable_vertical_scale_drag(
    f3d_window_t* window, f3d_options_t* options, double sensitivity);

  /**
   * @brief Restore the interactor style that was active before
   *        f3d_ext_enable_vertical_scale_drag(). No-op if never enabled.
   *        Note: this restores camera control but does NOT reset the model scale;
   *        set `render.model_scale` back to (1,1,1) to undo the exaggeration.
   */
  F3D_EXPORT void f3d_ext_disable_vertical_scale_drag(f3d_window_t* window);

  /**
   * @brief Show a live readout of the world coordinate under the mouse cursor.
   *
   * Installs an interactor style that, on every mouse move, picks the world point
   * under the cursor and writes its X/Y/Z into a text box pinned to the bottom-left
   * of the view. The box shows a blank line when the cursor is not over any
   * geometry. All camera gestures keep their normal f3d behaviour.
   *
   * Call AFTER the engine has an interactor and before f3d_interactor_start(). The
   * previous style is remembered and restored on disable.
   *
   * Note: this installs its own interactor style, so it cannot be active at the
   * same time as f3d_ext_enable_vertical_scale_drag() on the same window.
   *
   * @param window Window handle.
   * @return 1 on success, 0 if the window has no interactor/renderer yet.
   */
  F3D_EXPORT int f3d_ext_enable_coord_readout(f3d_window_t* window);

  /**
   * @brief Remove the coordinate readout overlay and restore the prior interactor
   *        style. No-op if never enabled.
   */
  F3D_EXPORT void f3d_ext_disable_coord_readout(f3d_window_t* window);

  /**
   * @brief Add labelled bounding-box axes (numbered X/Y/Z tick axes) around the
   *        data — the cube-axes that stock libf3d lacks (gap #2).
   *
   * Bounds are captured from the data actors at call time; call again to refresh
   * after the geometry or scale changes. Camera-following labels update on render.
   *
   * @param window Window handle.
   * @return 1 on success, 0 if there is no renderer/camera/data yet.
   */
  F3D_EXPORT int f3d_ext_enable_cube_axes(f3d_window_t* window);

  /**
   * @brief Remove the labelled cube axes. No-op if never enabled.
   */
  F3D_EXPORT void f3d_ext_disable_cube_axes(f3d_window_t* window);

#ifdef __cplusplus
}
#endif

#endif // F3D_EXT_C_API_H
