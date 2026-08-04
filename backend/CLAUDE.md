# LEF Layout Editor MVP — Backend

C++23 backend that reads LEF/DEF and SystemVerilog EDA data into an in-memory
database, then renders it through a layer-based pipeline into Skia commands
consumed by a Flutter plugin. This is an MVP/proof-of-concept: the goal right
now is finding the right architecture for editing hierarchical designs with
millions of objects, not shipping features. See `README.md` for the full
brief and the live plan checklist; see `BENCHMARKS.md` for benchmark history
and design-decision writeups — neither is duplicated here.

## Requirements (non-negotiable)

- Target: Linux servers, little/no GPU. Optimize for memory and CPU, not GPU.
- Tests are written alongside the code they cover, not after.
- Performance decisions must be backed by a benchmark, not intuition.
- C++23. Keep abstractions minimal and justified by present, not hypothetical, needs.
- Keep responses and docs concise — this repo's own README asks for that explicitly.

## Layout

- `src/database/` — the object-pool database. `schema.py` is the source of
  truth (a `cmg.Schema` of `Klass`/`Field` definitions); `generated/` is
  produced from it and must never be hand-edited (see Database codegen below).
  `database.hpp` is the single public include (`#include "generated/root.hpp"`).
- `src/geometry/` — `Geometry`, a Boost.Geometry-backed wrapper (bbox, overlap,
  transform, polygon union/buffer, label placement, overlap-merging) over the
  database's `Point`/`Rect`/`Polygon`/`Path`/`Shape` types. Fully covered by
  `geometry_test.cpp`.
- `src/view_style/` — `ViewLayerSet`/`ViewLayer`: the rendering-purpose layer
  concept distinct from the LEF-mirroring `database` — a `TERMINAL` and
  `OBSTRUCTION` `ViewLayer` per physical `Layer`, plus one `BOUNDARY`
  `ViewLayer` not tied to any physical `Layer`. `ViewLayerSet::build_for_technology`
  builds the full set for a `Technology` once, shared/global. Each physical
  `Layer` gets one color from a default palette (shared by its `TERMINAL`/
  `OBSTRUCTION` `ViewLayer`s — no purpose-based fill pattern yet); see the
  class's own doc comments for the palette/wraparound details. Fully covered
  by `view_style_test.cpp`.
- `src/scene/` — `Scene`, per-handle mutable view state (currently displayed
  `AbstractId`, pan/scale/viewport-size transform, per-`ViewLayer`
  visibility, selection). Distinct from the persistent `Root` database.
  Layer visibility is keyed by `ViewLayerId`, not `LayerId` — a physical
  layer has independently toggleable `TERMINAL`/`OBSTRUCTION` visibility.
  Selection is `std::variant<TerminalId, ObstructionId>` — extend the
  variant as more selectable kinds need it rather than generalizing early.
- `src/pipeline/` — `Pipeline`: `generate_shapes` → `filter_by_viewport_and_size`
  → `filter_by_layer_visibility`, no node/task framework. Each stage is a
  non-static instance method that self-caches (`CachedStage<Key, Value>`,
  keyed on whatever it actually depends on) and chains to the previous
  stage internally — reuse one `Pipeline` instance per `Scene`-equivalent
  lifetime. `generate_shapes` resolves each `Shape` straight to its
  `ViewLayerId` in the same pass (no separate resolve stage), merges each
  Shape's own overlapping rects/polygons via `Geometry::merge_overlapping_fills`,
  and attaches one text label per distinct layer a Terminal has geometry
  on. `filter_by_layer_visibility` groups into `std::map<ViewLayerId, ...>`
  (not `unordered_map`) — deliberate, since `ViewLayerId`'s ordering matches
  LEF-declared layer stacking order, giving correct bottom-up draw order
  for free; don't change this to `unordered_map`. See the class's own doc
  comments for full per-stage rationale, and `BENCHMARKS.md` for current
  numbers and history. Fully covered by `pipeline_test.cpp`.
- `src/render/` — `Renderer`, three stages on top of `Pipeline`'s output,
  same self-caching pattern: `transform_to_pixels` (dbu→pixel, no Y-flip)
  → `build_picture` (Skia `SkPictureRecorder` draw calls) → `rasterize`
  (SkPicture → raw `PixelBuffer`). Takes a `Pipeline&` from the caller
  rather than owning one. `rasterize` uses explicit `kRGBA_8888_SkColorType`
  (not Skia's platform-native `kN32_SkColorType`) so byte layout matches
  between the macOS dev machine and the Linux target, and applies the
  Y-axis flip (`transform_to_pixels` deliberately doesn't) as one
  whole-canvas transform — `build_picture` counter-flips each text label
  locally to keep glyphs upright under that flip, so the two are coupled;
  check both if touching either. `render` is a compiled library
  (`add_library(render STATIC ...)`), not header-only like its siblings —
  isolates `SkFontMgr_mac_ct.h`/`ApplicationServices.h` (legacy Carbon
  `Rect`/`Point`/`Polygon` typedefs collide with `le::` types under `using
  namespace le`) to `render.cpp`; don't change this back to `INTERFACE`.
  Single-threaded — see README's Threading open design question and
  `BENCHMARKS.md` for current warm-path numbers. Fully covered by
  `render_test.cpp`, including real pixel-byte assertions, not just
  "didn't crash". Depends on a machine-specific Skia checkout, not
  committed to this repo — see Open gaps below.
- `src/io/` — format readers. Currently `lef_reader.{hpp,cpp}`, which drives
  the vendored `lefr*` LEF-parser C callbacks and populates `Root` via the
  generated create/get API. Tested against `src/lefdef/lef/TEST/complete.5.8.lef`
  (the vendored parser's own regression fixture) plus small hand-written
  `.lef` files under `src/io/tests/fixtures/` for cases that fixture
  doesn't hit. `LEFReader` only supports a subset of LEF; extend the tests
  as more constructs get support. `orientation_from_parser`/
  `routing_direction_from_parser`/`signal_direction_from_parser` are
  `public` (unlike the rest of `LEFReader`) so they can be unit-tested
  directly — pure, no parser/instance state.
- `src/api/` — `api.hpp`/`api.cpp`, the C API surface a Flutter plugin's
  Dart FFI binds to: an opaque `LeHandle` (`le_create`/`le_destroy`)
  wrapping one `Root`/`ViewLayerSet`/`Scene`/`Pipeline`/`Renderer` per
  handle (reused across calls, not reconstructed per call); `le_read_lef`
  (callable multiple times on one handle — e.g. tech file then macro
  file(s)); `le_design_count`/`le_design_name`/`le_set_current_design`;
  `le_set_pan`/`le_set_scale`/`le_set_viewport_size`; and
  `le_render_pixel_buffer`. `api.hpp` must stay plain C — no `std::` types,
  default arguments, or overloads in any public declaration — so it parses
  cleanly for `ffigen`/Dart FFI; `LeHandle`'s real definition lives only in
  `api.cpp`. Every function null-checks its handle and degrades gracefully
  rather than crashing. Fully covered by `api_test.cpp`, using a small
  hand-written `.lef` fixture. Depends on `database`, `geometry`, `scene`,
  `view_style`, `pipeline`, `render`, `io`.
- `src/lefdef/` — vendored LEF/DEF 6.0.62-p004 C parser source (Si2 distribution).
  Built by its own `Makefile` via `ExternalProject_Add` in the top-level
  `CMakeLists.txt`; only `lef/` is wired into the build so far (`def/` is
  vendored but unbuilt until a DEF reader exists). Never hand-edit — it's
  third-party source, license in `src/lefdef/{lef,def}/LICENSE.TXT`.
- Each module's tests live alongside it in a `tests/` subdirectory (e.g.
  `src/database/tests/database_test.cpp`), hand-written GTest.

## Database codegen (cmg)

Generated code follows the **INDEXED_POOLS** export style from
[cmg](https://github.com/johndru-astrophysics/cmg) — not cmg's default
(`SMART_POINTERS`). Every `Klass` in `schema.py` becomes:

- `XxxData` — a plain data struct.
- `XxxId` — a `{index, generation}` handle (see `generated/ids.hpp`), not a
  pointer, fully ordered (usable as a `std::map` key with no custom comparator).
- Storage in a `Pool<XxxData, XxxId>` (`generated/pool.hpp`) — a generational
  slot array, so erased objects can't alias a reused slot.
- `Root` (`generated/root.hpp`) owns every pool plus an `index_` for
  parent→children and lookup-by-field indices, and exposes
  `create_x`/`get_x`/`get_x_ids`/`for_each_x_id`/`clear_x`/`get_x_size` per class.

To change the schema: edit `src/database/schema.py`, bump `Schema.version`,
then regenerate with the `regen-database` skill rather than editing
`generated/` by hand. Real test coverage lives in each module's own
`tests/` directory, not `generated/` — `cmg` doesn't emit test files.

## Open gaps (tracked in README's Plan checklist)

- `src/lefdef/def` is vendored but not yet wired into `CMakeLists.txt` — add
  an `ExternalProject_Add(def_lib ...)` (mirroring `lef_lib`) when a DEF
  reader module is added.
- `cmg` itself isn't installed in this environment — see the `regen-database`
  skill for setup (or run it via `poetry run cmg` from the local
  `/Users/john/Projects/synthosilicon/cmg` checkout).
- Skia isn't vendored/built by this project — `src/render/`'s
  `CMakeLists.txt` `skia` target points `SKIA_DIR` at a pre-built checkout
  (default `/Users/john/Projects/synthosilicon/skia/skia`, override with
  `-DSKIA_DIR=...`). That checkout must have `out/MacStatic/libskia.a`
  built with `is_component_build=false` (static). Links `libskia.a` +
  Homebrew `harfbuzz`/`icu4c`/`jpeg`/`png`/`z`/`webp`/`webpdemux` + macOS
  `CoreText`/`CoreFoundation`/`CoreGraphics`/`CoreServices` frameworks — no
  GPU (Ganesh/Metal) frameworks needed, only raster (CPU) surface APIs are
  used.
- Linux build needs a fontconfig/FreeType-backed `SkFontMgr` — `render`'s
  default typeface is CoreText-backed (macOS-only) right now.

## Build

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Dependencies: `spdlog`, `fmt`, `Boost` (headers only, for `geometry`) via
`find_package` — installed on this dev machine via Homebrew; GoogleTest and
GoogleBenchmark via `FetchContent` (no system install needed). `src/lefdef/lef`
is built as an `ExternalProject_Add` step that shells out to its own vendored
`Makefile`.

**Gotcha:** that vendored Makefile's `all: install release` target is not
safe under a parallel/inherited `make` jobserver — both traversals touch the
same bison-generated `lef.tab.c`/`liblef.a`, so running it under `-j` races
and fails. The `lef_lib` `ExternalProject_Add` step already forces
`--unset=MAKEFLAGS make -j1` — don't remove that when touching the build.

### Coverage (line + branch)

Off by default (instrumentation has a real perf cost, and this project's
own rule is benchmark first). Opt in at configure time:

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DENABLE_COVERAGE=ON
cmake --build build --target coverage
```

Rebuilds `io`/`backend_tests` with Clang source-based coverage, runs the
tests, and prints a `llvm-cov report --show-branch-summary` table (also
written to `build/coverage/report.txt` and `build/coverage/lcov.info`).
Requires Clang and `llvm-profdata`/`llvm-cov` — resolved via `xcrun`
automatically on macOS.

**Gotcha:** `ENABLE_COVERAGE` is a *cached* option — reconfiguring with e.g.
`-DCMAKE_BUILD_TYPE=Release` alone does **not** reset a previously-set-ON
value back to OFF, and coverage instrumentation forces `-O0` regardless of
`CMAKE_BUILD_TYPE`. Always pass `-DENABLE_COVERAGE=OFF` explicitly (or use a
fresh `build/`) to get back to a normal, uninstrumented build — this
silently produced ~15-20x-inflated benchmark numbers once already.

### Benchmarks

```
cmake --build build --target pipeline_benchmarks
./build/pipeline_benchmarks
```

Build in `-DCMAKE_BUILD_TYPE=Release` for real numbers — Debug timings
aren't meaningful. `src/pipeline/benchmarks/stress_data.hpp` generates a
deliberately unrealistic 1M-shape single-macro LEF file and builds the
`Scene` used to view it; `pipeline_benchmark.cpp` times each `Pipeline`/
`Renderer` stage in isolation plus the full chain under several call
patterns. See `BENCHMARKS.md` for current numbers and full history. Add
`--benchmark_repetitions=5 --benchmark_report_aggregates_only=true` for
stable numbers when comparing two approaches, and
`--benchmark_filter=<regex>` to run a subset.

`src/pipeline/benchmarks/render_preview.cpp` (target `render_preview`) is a
dev-only tool, not a benchmark: `./build/render_preview a.lef [b.lef ...]`
reads every given LEF file into one shared `Root` and writes one PNG per
Design (`preview/<library-name>__<design-name>.png`) via the real
`Renderer::rasterize()` path, so real LEF renders can be visually
sanity-checked without waiting for Flutter texture wiring. Not run by
`ctest` or the `coverage` target.

## Conventions observed in existing code

- Everything lives in `namespace le`.
- Doxygen-style `/// @brief` one-liners on generated public methods — match
  this on hand-written public API.
- No exceptions for expected-missing-data paths — pool lookups return
  nullable pointers (`get(id)` → `T*`) or use `std::optional`/`std::expected`.
- The vendored LEF parser reuses one scratch struct per callback type across
  the whole file and does **not** reset fields to a neutral default between
  calls — always check the matching `has*()` guard (e.g.
  `lefiLayer::hasDirection()`) before trusting a getter, or a value can leak
  forward from a previous element that happened to set it.

## Related prior art

`../../layout_engine/backend` (sibling repo, same author) is an earlier,
more complete implementation of the same idea. This MVP deliberately
restarts the pipeline/rendering architecture decisions rather than
importing that one — treat it as reference/lessons-learned, not code to
copy wholesale.

## Skills

- `regen-database` — regenerate `src/database/generated/` from `schema.py` via `cmg`.
- `build-test` — configure/build/test the CMake project once one exists.
- `cpp-review` — review pending changes for missing test coverage, unnecessary
  allocations/copies/moves, memory safety, and other issues; reports via
  `ReportFindings`, doesn't apply fixes. Named to avoid colliding with the
  built-in, billed `/code-review ultra`.
