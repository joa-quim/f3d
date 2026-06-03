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
   * @brief Install a rubber-band point selector on the window (DISARMED).
   *
   * Adds high-priority observers to the window's interactor. Selection mode starts
   * OFF: right-drag keeps its normal f3d behaviour until the user ARMS it — press
   * Ctrl+B (toggle) or call f3d_ext_set_rubber_band_armed(window, 1). While armed,
   * right-click-drag a box; on release the enclosed points are picked (via
   * f3d_ext_area_pick_points, toggle/XOR into a persistent selection), highlighted
   * with the @p r,@p g,@p b overlay colour, and @p cb is invoked with the FULL
   * current selection. Ctrl+Z undoes the last change. This is intended for POINT
   * CLOUDS — on a surface the frustum pick also returns occluded points, so callers
   * should not arm it for solid meshes.
   *
   * Call AFTER the engine has an interactor (f3d_engine_get_interactor) and before
   * f3d_interactor_start().
   *
   * @param r,g,b Overlay colour for the selected points, each in [0,1]. Use a neutral
   *              tone (e.g. light grey 0.83,0.83,0.83) so it does not clash with the
   *              points' own colours.
   * @return 1 on success, 0 if the window has no interactor yet / on error.
   */
  F3D_EXPORT int f3d_ext_enable_rubber_band_pick(
    f3d_window_t* window, f3d_ext_pick_callback_t cb, void* user_data, double r, double g, double b);

  /**
   * @brief Arm (1) or disarm (0) rubber-band selection mode programmatically.
   *        Equivalent to the Ctrl+B toggle. No-op if the selector is not enabled.
   */
  F3D_EXPORT void f3d_ext_set_rubber_band_armed(f3d_window_t* window, int armed);

  /**
   * @brief Whether rubber-band selection mode is currently armed (1) or not (0).
   */
  F3D_EXPORT int f3d_ext_get_rubber_band_armed(f3d_window_t* window);

  /**
   * @brief Remove the rubber-band selector observers + overlays. No-op if it was
   *        never enabled.
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
   * @brief Enable middle-CLICK to set the rotation centre: a middle-button click
   *        (press+release without dragging) picks the point under the cursor, makes
   *        it the camera focal point and pans so it is centred. Middle-DRAG still
   *        pans (this observer is passive). No-op if there is no interactor/renderer.
   * @return 1 on success, 0 otherwise.
   */
  F3D_EXPORT int f3d_ext_enable_focus_pick(f3d_window_t* window);

  /**
   * @brief Remove the middle-click focal-centre observers. No-op if never enabled.
   */
  F3D_EXPORT void f3d_ext_disable_focus_pick(f3d_window_t* window);

  /**
   * @brief Component flags for f3d_ext_enable_cube_axes (combine with bitwise OR).
   *
   * The default minimal look (F3D_EXT_CUBE_AXES_DEFAULT) is the bounding-cube
   * edges + X/Y tick labels + a bottom floor plane. Walls (gridlines on every
   * face) and Z (elevation) tick labels are opt-in.
   */
#define F3D_EXT_CUBE_AXES_EDGES 0x01   /**< cube outer edges + X/Y axis tick labels */
#define F3D_EXT_CUBE_AXES_FLOOR 0x02   /**< bottom face: a semi-transparent floor plane */
#define F3D_EXT_CUBE_AXES_GRID 0x04    /**< gridlines on all faces (the "walls") */
#define F3D_EXT_CUBE_AXES_ZLABELS 0x08 /**< Z (elevation) axis tick labels too */
#define F3D_EXT_CUBE_AXES_DEFAULT                                                                    \
  (F3D_EXT_CUBE_AXES_EDGES | F3D_EXT_CUBE_AXES_FLOOR | F3D_EXT_CUBE_AXES_ZLABELS)

  /**
   * @brief Add labelled bounding-box axes (numbered X/Y/Z tick axes) around the
   *        data — the cube-axes that stock libf3d lacks (gap #2).
   *
   * @p flags selects which components are drawn (see F3D_EXT_CUBE_AXES_*). Pass
   * F3D_EXT_CUBE_AXES_DEFAULT for the minimal look (edges + X/Y labels + floor,
   * NO walls). The cube is set to the exact data bounds.
   *
   * Bounds are captured from the data actors at call time; call again to refresh
   * after the geometry or scale changes. Camera-following labels update on render.
   *
   * @param window Window handle.
   * @param flags  Bitwise OR of F3D_EXT_CUBE_AXES_* (0 also means DEFAULT).
   * @return 1 on success, 0 if there is no renderer/camera/data yet.
   */
  F3D_EXPORT int f3d_ext_enable_cube_axes(f3d_window_t* window, int flags);

  /**
   * @brief Remove the labelled cube axes. No-op if never enabled.
   */
  F3D_EXPORT void f3d_ext_disable_cube_axes(f3d_window_t* window);

  /**
   * @brief Add a 2-D map frame for a flat (z=0) image viewed top-down: an X axis
   *        along the bottom and a Y axis along the left, with OUTWARD tick marks
   *        and caller-supplied printf label formats. The Z axis, floor and walls
   *        are not drawn. Shares the cube-axes registry, so f3d_ext_disable_cube_axes
   *        removes it.
   *
   * @param window Window handle.
   * @param xfmt   printf format for X (longitude) tick labels, e.g. "%.2f".
   * @param yfmt   printf format for Y (latitude) tick labels, e.g. "%.2f".
   *               A null/empty format falls back to the VTK default.
   * @return 1 on success, 0 if there is no renderer/camera/data yet.
   */
  F3D_EXPORT int f3d_ext_enable_image_axes(
    f3d_window_t* window, const char* xfmt, const char* yfmt);

  /**
   * @brief Add a vertical colour scale (scalar bar) on the right of the window,
   *        built from an ordered RGB palette mapped onto the value range
   *        [@p vmin, @p vmax]. The stock libf3d scalar bar only works through its
   *        scivis scalar pipeline; the GMT viewers colour via a palette texture, so
   *        this draws the matching bar directly.
   *
   * @param window  Window handle.
   * @param rgb     Ordered palette, @p ncolors * 3 bytes (R,G,B per entry, low->high).
   * @param ncolors Number of palette entries (>= 2).
   * @param vmin    Value at the bottom of the bar.
   * @param vmax    Value at the top of the bar.
   * @param title   Bar title (null/empty for none).
   * @param fmt     printf format for the tick labels, e.g. "%.1f" (null = default).
   * @return 1 on success, 0 if there is no renderer or the palette is invalid.
   */
  F3D_EXPORT int f3d_ext_enable_colorbar(f3d_window_t* window, const unsigned char* rgb,
    int ncolors, double vmin, double vmax, const char* title, const char* fmt);

  /**
   * @brief Remove the colour scale. No-op if never enabled.
   */
  F3D_EXPORT void f3d_ext_disable_colorbar(f3d_window_t* window);

  /**
   * @brief Give point SPRITES per-point colours (gap #9).
   *
   * The point-sprite path uses vtkPointGaussianMapper, which ignores texture
   * coordinates — so the 1xN palette-texture + per-point u-texcoord trick that
   * colours plain GL_POINTS leaves every splat flat grey. This bakes a per-point
   * RGB (n_comp==3) or RGBA (n_comp==4) unsigned-char colour array directly onto
   * the point-sprite polydata and switches the gaussian mapper to direct scalar
   * colours, so colour-by-value works for round sprites too.
   *
   * Colours are shown at full strength (Emissive on). The splat SHAPE is the stock
   * `model.point_sprites.type` option ("sphere" shaded ball / "circle" ring /
   * "gaussian" soft blob) — note f3d's point-splat mapper ignores a splat-shader
   * override set after the first render, so the shape cannot be changed from here
   * (for round FLAT points use the plain-points path + f3d_ext_round_points instead).
   * Enable point sprites and render the window once before calling (the sprite
   * actors are built lazily on import/first render). @p rgb is read during the call
   * only — the caller keeps ownership.
   *
   * @param window   Window handle.
   * @param rgb      Interleaved unsigned-char colours, n_points*n_comp bytes.
   * @param n_points Number of points; must match the sprite polydata point count.
   * @param n_comp   3 (RGB) or 4 (RGBA).
   * @return 1 if applied to at least one sprite actor, 0 on error / no match.
   */
  F3D_EXPORT int f3d_ext_color_point_sprites(
    f3d_window_t* window, const unsigned char* rgb, size_t n_points, int n_comp);

  /**
   * @brief Render PLAIN points (point sprites disabled) as round discs (gap #9).
   *
   * The plain-points path honours the palette texture (colour-by-value works) but
   * draws SQUARE GL_POINTS. This sets vtkProperty::RenderPointsAsSpheres on the
   * imported point actors so they render round. With @p unlit != 0 lighting is turned
   * off so each disc is a flat, full-strength colour (no 3D sphere shading) — i.e.
   * round, flat, coloured points without the gaussian point-sprite mapper. Pass
   * @p on == 0 to revert to square points. Render once before calling.
   *
   * @param window Window handle.
   * @param on     Non-zero: round points; zero: square points.
   * @param unlit  Non-zero (and on): flat colour (LightingOff); zero: lit spheres.
   * @return 1 if applied to at least one point actor, 0 on error / no match.
   */
  F3D_EXPORT int f3d_ext_round_points(f3d_window_t* window, int on, int unlit);

#ifdef __cplusplus
}
#endif

#endif // F3D_EXT_C_API_H
