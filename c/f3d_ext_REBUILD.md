# Rebuilding f3d with the `f3d_ext` C extensions

These three files add the `f3d_ext_*` symbols (area pick, rubber-band point
selection, renderer / render-window escape hatch) **into the existing
`f3d_c_api.dll`** — no separate library:

- `c/f3d_ext.h`              — public C API
- `c/f3d_ext_pick.cxx`       — hardware-selector area pick + escape hatches
- `c/f3d_ext_interactor.cxx` — rubber-band interactor style + observer

`c/CMakeLists.txt` is already patched to compile them on the `c_api` target
(adds the two sources, the `f3d_ext.h` header, the `library/private` include
path, and links VTK `CommonCore CommonDataModel RenderingCore RenderingOpenGL2
InteractionStyle`).

They depend on the **private** header `library/private/window_impl.h` and on VTK,
so they MUST be compiled inside the f3d tree (not as a sidecar against the binary).

## Why these features need a source build
`f3d_window_t*` is a `reinterpret_cast` of `f3d::window*`; the concrete object is
`f3d::detail::window_impl`, which exposes `GetRenderWindow()` / `GetRenderer()` —
but only via the private header. The stock public API / shipped DLL hides them,
so the VTK renderer, interactor, hardware selector and rubber-band style are
unreachable without compiling in-tree.

## Build steps (Windows, matching the shipped 3.5.0-103 raytracing build)

1. Configure (reuse the SAME options the current DLL was built with — raytracing,
   bundled VTK, etc. — so the new DLL stays ABI-compatible with everything else):

   ```pwsh
   cd C:\programs\compa_libs\f3d_GIT
   cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
     -DCMAKE_BUILD_TYPE=Release `
     -DF3D_BUILD_APPLICATION=OFF `
     -DBUILD_SHARED_LIBS=ON `
     -DF3D_BINDINGS_C=ON `
     -DVTK_DIR="<same VTK build used for the 3.5.0-103 DLL>"
   ```
   (Match whatever VTK / plugin flags the existing build used. `InteractionStyle`
   must be present in that VTK — it is in any default VTK build.)

2. Build just the C API target (pulls in `libf3d`):

   ```pwsh
   cmake --build build --config Release --target c_api
   ```

3. Confirm the new symbols are exported:

   ```pwsh
   dumpbin /exports build\c\Release\f3d_c_api.dll | Select-String f3d_ext
   # expect: f3d_ext_area_pick_points, f3d_ext_free_ids,
   #         f3d_ext_enable_rubber_band_pick, f3d_ext_disable_rubber_band_pick,
   #         f3d_ext_get_renderer, f3d_ext_get_render_window
   ```

4. Drop the rebuilt DLL where `F3D.jl` loads it (replace in place). `F3D.jl` uses
   `const libf3d = joinpath(ensure_f3d(), _lib_filename())`, i.e.:

   ```
   C:\Users\j\.julia\dev\F3D.jl\src\lib\F3D-3.5.0-103-gec0d94c6-Windows-x86_64-raytracing\bin\f3d_c_api.dll
   ```
   Back up the old one, copy the new `f3d_c_api.dll` (and any newly-required VTK
   DLLs, if the module set changed) into that `bin\` folder.

5. Restart Julia and verify:

   ```julia
   using Libdl
   h = Libdl.dlopen(F3D.libf3d)
   Libdl.dlsym(h, :f3d_ext_enable_rubber_band_pick; throw_error=false) !== nothing  # true
   ```

   Then `onpick` in `examples/gmt_solids.jl` `view_points` goes live:
   ```julia
   view_points(D; onpick = rows -> (global SEL = D.data[rows, :]))
   # press 'r', drag a box -> SEL holds the selected rows
   ```

## Notes
- Picked ids are point ids in the rendered polydata = original `D.data` row index
  for a non-vertex-split point cloud (what `view_points` builds). +1 done on the
  Julia side.
- Hardware selection is a VISIBLE pick (occluded points excluded), so a tilted 3D
  view selects only what you can see — which is usually what you want.
- If a static VTK is used, the modules listed are linked into `f3d_c_api.dll`
  directly and no extra VTK DLLs are needed; with shared VTK, ship the new
  `vtkInteractionStyle*` DLL alongside.
- Upstreamable: the same files could go to f3d as PRs adding these to the C API.
