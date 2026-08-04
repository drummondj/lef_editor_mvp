# LEF Layout Editor MVP — Flutter Plugin

`lef_editor_plugin`, a `plugin_ffi`-template Flutter plugin that binds to
the backend's C API (`../backend/src/api/api.hpp`) via Dart FFI and renders
`le_render_pixel_buffer`'s output as a platform `Texture`. See
`../backend/README.md` for the project brief and `../backend/CLAUDE.md` for
the backend's own architecture.

## Requirements (inherited from backend/README.md)

- Target platforms: macOS (dev, working end-to-end, texture included) and
  Linux (deploy, blocked upstream — see Open design questions).
- Keep responses and docs concise.
- `backend/src/api/api.hpp` is the single source of truth for the FFI
  surface — never hand-edit generated bindings, regenerate via the `ffigen`
  skill instead.

## Architecture

Two separate paths call into the C API — not one:

- **Dart FFI** (`lib/lef_editor_plugin.dart`'s `LeEditor` class, wrapping
  ffigen-generated `lib/lef_editor_plugin_bindings_generated.dart`) for
  everything except pixel delivery: `le_create`/`le_destroy`, `le_read_lef`,
  design enumeration/selection, `le_set_pan`/`le_set_scale`/
  `le_set_viewport_size`, `le_fit_scene` (`LeEditor.fitScene()` — fits pan/
  scale to the selected Design's content bbox, see Scene::fit_to_content),
  plus a `renderPixelBuffer()` that copies a frame into Dart memory for
  previewing/testing without a texture. Cheap, called directly from Dart.
- **Native platform texture code** (macOS: `LeApiBridge`/`LeTexture` in
  `macos/Classes/`, implementing `FlutterTexture`; Linux: `FlLeTexture` in
  `linux/lef_texture.cc`, implementing `FlPixelBufferTexture`) calls
  `le_render_pixel_buffer` itself, inside the platform's own pull-based
  texture callback (`copyPixelBuffer` on macOS, `copy_pixels` on Linux) —
  not via Dart FFI. Both platforms only pull a frame after being told a new
  one is available (see below), which matches `le_render_pixel_buffer`'s
  own "cheap unless something changed" caching (see api.hpp), so the native
  callback can call it every pull with no extra throttling logic.
- The Dart-owned `LeHandle*` has to reach that native code too, and Dart FFI
  can't reach the engine's texture registrar itself (only platform embedder
  code can) — so a small `MethodChannel('lef_editor_plugin')` exists purely
  to shuttle a handle address and a texture id back and forth
  (`LeEditor.createTexture()`/`LeTexture` on the Dart side; `LefEditorPlugin`
  on macOS, `lef_editor_plugin_register_with_registrar` on Linux — same
  three methods on both: `createTexture`/`markTextureFrameAvailable`/
  `disposeTexture`). The actual per-frame pixel pull never crosses this
  channel.
- `LePixelBuffer.data` is only valid until the next
  `le_render_pixel_buffer`/`le_destroy` call on the same handle (see
  api.hpp's own comment). `LeEditor.renderPixelBuffer()` copies it into a
  Dart `Uint8List` immediately; the Linux texture hands the pointer to
  Flutter directly instead (`FlPixelBufferTexture`'s own contract allows
  this — the buffer only needs to survive until the *next* render-thread
  tick, which matches api.hpp's validity window exactly, see
  `lef_texture.cc`'s comment); the macOS texture instead copies+converts
  into a `CVPixelBuffer` (see Native linking below for why conversion, not
  just a copy, is required there).
- **Lifetime and thread safety are real, open constraints, not just
  disclaimers** — see the doc comment on `LeTexture` in
  `lib/lef_editor_plugin.dart`: a `LeTexture` must be disposed before the
  `LeEditor` it came from, and a `LeEditor` setter (including `fitScene`)
  racing a pending, not-yet-rendered `markFrameAvailable` on the engine's
  raster thread is a genuine data race (backend's Scene/Pipeline/Renderer
  aren't documented as thread-safe). Not solved at the plugin layer — a
  real fix needs a lock inside the C API itself.

## Open design questions

- **Linux is a real, tracked gap, not just unwired.** `src/CMakeLists.txt`
  and `linux/CMakeLists.txt` both have an `LE_LINK_BACKEND` option (default
  `OFF`) that mirrors the macOS link recipe, but turning it on won't
  actually work yet: backend's `Renderer::default_typeface()` is
  CoreText-backed (macOS only — see backend/CLAUDE.md's `render` entry; a
  Linux fontconfig/FreeType `SkFontMgr` is a tracked, not-yet-done backend
  gap), and no Linux Skia checkout exists on this dev machine either (see
  backend/CLAUDE.md's "Open gaps"). The Linux texture/plugin code itself
  (`linux/lef_editor_plugin.cc`, `linux/lef_texture.cc`) is written against
  the real vendored Flutter Linux embedder headers but has **never been
  compiled** — there's no Linux toolchain/GTK on this dev machine, so it's
  unverified structurally-correct-by-inspection, not proven. Revisit
  `LE_LINK_BACKEND` once both backend gaps land, and build it for real on
  an actual Linux machine before trusting it further.
- **Packaging.** Backend depends on a machine-specific Skia checkout plus
  Homebrew Boost/spdlog/fmt/harfbuzz/icu4c/jpeg/png/webp (see backend
  CLAUDE.md's "Open gaps") — none of that is redistributable as-is, and the
  macOS podspec/Linux CMake both hardcode this machine's paths as defaults.
  Not a blocker for local dev; is a blocker before this plugin could ship
  to a machine that isn't this one.

## Native linking (macOS — done and verified; Linux — gated, unverified)

`api.hpp`'s `le_*()` functions are implemented in `backend/src/api/api.cpp`
and compiled by backend's *own* CMake build (`../backend/build/libapi.a`
etc.) — this plugin links that output directly rather than recompiling it,
mirrored by hand in two places since there's no automated sharing between a
CMakeLists.txt and a podspec:

- `macos/lef_editor_plugin.podspec` — `pod_target_xcconfig`'s
  `OTHER_LDFLAGS` force-loads `libapi.a` (its `le_*()` symbols are never
  referenced by this plugin's own sources — Dart only finds them via
  `dlsym` at runtime — so without `-force_load` the linker drops the whole
  object file) plus `librender.a`/`libio.a`/`liblef.a`/`libskia.a` and the
  Homebrew/system libs backend's own `CMakeLists.txt` needs. Verified
  end-to-end: `flutter build macos` in `example/` succeeds, `nm -gU` on the
  resulting `lef_editor_plugin.framework` shows all nine `le_*` symbols
  exported, and the example app renders `testcell.lef`'s boundary and M1
  pin through the full `LeEditor` → `LeTexture` → `Texture` widget chain —
  confirmed visually (screenshot), not just "didn't crash."
  - **Gotcha:** CocoaPods' `OTHER_LDFLAGS` merging silently drops anything
    it recognizes as the C++ runtime library — neither a plain `-lc++` flag
    nor an explicit `libc++.tbd` path survives (no error, just absent from
    the merged `.xcconfig`). The fix ended up being
    `macos/Classes/LeApiBridge.mm` (see below) simply *being* an
    Objective-C++ file — its mere presence makes Xcode pick the C++ linker
    driver for the whole target (this plugin's other sources are plain C).
  - **Gotcha:** `__dir__` inside the podspec resolves through CocoaPods'
    `.symlinks/plugins/...` path, not this repo's real layout — wrap it in
    `File.realpath` before computing `../../backend`, or the relative walk
    silently lands inside `.symlinks` instead.
  - Requires `backend/build/{libapi,librender,libio}.a` and
    `backend/src/lefdef/lef/lib/liblef.a` to already be built (`build-test`
    skill, step 1) and a Skia checkout at `$SKIA_DIR` or
    `/Users/john/Projects/synthosilicon/skia/skia` (backend's own default).
- `src/CMakeLists.txt` / `linux/CMakeLists.txt` (Linux) — same recipe via
  `LE_LINK_BACKEND` (`--whole-archive` instead of `-force_load`;
  `pkg_check_modules` instead of hardcoded Homebrew paths). Structurally
  validated (`cmake -S src -B <dir> -DLE_LINK_BACKEND=ON` gets past syntax
  and cache-variable setup and fails only at `find_package(PkgConfig)`,
  which this macOS dev machine doesn't have) but not build-verified — see
  Open design questions above.

### Why macOS needs a pixel copy+swizzle, not just a copy

`FlutterTexture.copyPixelBuffer`'s own doc (`FlutterTexture.h`) restricts
the returned `CVPixelBuffer` to `kCVPixelFormatType_32BGRA` or a YpCbCr
variant — never `32RGBA`. api.hpp's `LePixelBuffer` is deliberately
platform-neutral RGBA8888 (see its own comment). So macOS's
`LeApiBridge.mm` swizzles R/B per pixel while copying into a new
`CVPixelBuffer`; Linux's `FlPixelBufferTexture` wants RGBA directly (see
`fl_pixel_buffer_texture.h`'s own doc), so `lef_texture.cc` hands off
api.hpp's buffer pointer as-is, no copy or swizzle needed — found the
asymmetry by reading both platforms' actual headers, not assumed.

Two more macOS specifics, both found by trial (the failure mode gives no
useful compile-time signal):

- `CVPixelBufferCreate` **must** be called with
  `kCVPixelBufferIOSurfacePropertiesKey` in its attributes dictionary — a
  plain (non-IOSurface-backed) pixel buffer isn't rejected by
  `CVPixelBufferCreate` itself, but the engine's Metal compositor can't
  wrap it into a texture; the only symptom is "Could not create Metal
  texture from pixel buffer: CVReturn -6660" in the system log and the
  `Texture` widget staying permanently blank, with `copyPixelBuffer` itself
  reporting success throughout.
- Swift files in a CocoaPods **framework** target do not automatically see
  the target's own C headers the way a bridging header would (and
  `SWIFT_OBJC_BRIDGING_HEADER` is outright rejected for framework targets:
  "using bridging headers with framework targets is unsupported"). Rather
  than fight Clang's module dependency scanner (which also separately
  rejects a `../../` relative include escaping the pod's module root, even
  though the same relative include compiles fine in a plain non-modular
  `.c`/`.mm` translation unit), `LeApiBridge.h`/`.mm` exists specifically
  so `LeTexture.swift` only ever sees a small, clean Objective-C interface
  — ordinary same-target ObjC↔Swift interop, which has always worked with
  no extra plumbing.

## Layout

- `pubspec.yaml`, `ffigen.yaml` — `ffigen.yaml` points `entry-points` at
  `../backend/src/api/api.hpp` directly (not a copy) with `compiler-opts:
  [-x, c]` and `ignore-source-errors: true` — see the `ffigen` skill for
  why both are required, not optional niceties. `pubspec.yaml` declares
  `pluginClass: LefEditorPlugin` for both `macos:` and `linux:` alongside
  `ffiPlugin: true` — a plugin can be both at once; Flutter's tool treats
  it as method-channel-registered (bundling the FFI shared library
  alongside it), not as two competing registrations.
- `lib/lef_editor_plugin.dart` — the public Dart API: `LeEditor` (FFI
  wrapper + `createTexture()`), `LeFrame` (a `renderPixelBuffer()` result),
  `LeTexture` (a live platform texture — see its own doc comment for the
  lifetime/thread-safety constraints). `lib/lef_editor_plugin_bindings_generated.dart`
  — ffigen output, never hand-edit.
- `src/lef_editor_plugin.h` — `#include`s `../backend/src/api/api.hpp`
  directly (so this TU fails to compile if that header ever stops being
  C-parseable); `src/lef_editor_plugin.c` — empty, exists only because the
  CMake target needs a source file (see Native linking above for where the
  real symbols come from).
- `macos/Classes/` — `LefEditorPlugin.swift` (method channel),
  `LeTexture.swift` (`FlutterTexture`), `LeApiBridge.h`/`.mm` (the
  Objective-C++ shim both depend on — see Native linking above for why it
  exists as its own class rather than Swift calling api.hpp directly),
  `lef_editor_plugin.c` (the FFI forwarder, unrelated to the texture path).
- `linux/` — `lef_editor_plugin.cc`/`include/lef_editor_plugin/lef_editor_plugin.h`
  (method channel), `lef_texture.cc`/`.h` (`FlPixelBufferTexture`) — same
  protocol as macOS, **unverified** (see Open design questions).
- `example/` — loads a bundled `testcell.lef` fixture
  (`backend/src/api/tests/fixtures/testcell.lef`, copied to
  `example/assets/`), selects its Design, creates a texture, and displays
  it — the actual proof this plugin works, not just that it builds.

## Skills

- `ffigen` — regenerate `lib/lef_editor_plugin_bindings_generated.dart`
  from `backend/src/api/api.hpp`.
- `build-test` — build the backend native code this plugin links against,
  then `flutter pub get`/`analyze`/`test`, plus the macOS example app.
