# LEF Layout Editor MVP — Backend

C++23 backend that reads LEF/DEF and SystemVerilog EDA data into an in-memory
database, then renders it through a layer-based pipeline into Skia commands
consumed by a Flutter plugin. This is an MVP/proof-of-concept: the goal right
now is finding the right architecture for editing hierarchical designs with
millions of objects, not shipping features. See `README.md` for the full
brief and the live plan checklist; see `BENCHMARKS.md` for benchmark history
and design-decision writeups; see `LEFDEF_BUGS.md` for confirmed bugs in
the vendored LEF/DEF parser/writer and how `src/io/` works around each —
none of these are duplicated here.

## Requirements (non-negotiable)

- Target: Linux servers, little/no GPU. Optimize for memory and CPU, not GPU.
- Tests are written alongside the code they cover, not after.
- Performance decisions must be backed by a benchmark, not intuition.
- C++23. Keep abstractions minimal and justified by present, not hypothetical, needs.
- Keep responses and docs concise — this repo's own README asks for that explicitly.

## Layout

- `src/database/` — the object-pool database. `schema.py` is the source of
  truth (a `codegen.Schema` of `Klass`/`Field` definitions); `generated/` is
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
  visibility, selection, and current interaction mode). Distinct from the
  persistent `Root` database.
  Layer visibility is keyed by `ViewLayerId`, not `LayerId` — a physical
  layer has independently toggleable `TERMINAL`/`OBSTRUCTION` visibility.
  Selection is `std::variant<TerminalId, ObstructionId>` — extend the
  variant as more selectable kinds need it rather than generalizing early.
  `Scene::Mode` (`SELECT`/`EDIT`, UPDATES.md item 11) is Select by
  default — Select is the only mode where `le_mouse_up` changes the
  current selection; Edit mode restricts mouse interaction to editing
  whatever is already selected (behavior TBD, a later item).
- `src/core/` — header-only generic building blocks shared between
  `pipeline` and `render` (UPDATES.md item 16), so neither module depends
  on the other for them: `RenderedShape`/`TinyShapeDot` (`pipeline`'s
  output type, `render`'s input type) and `VersionedStage<Key, Value>` —
  a single-slot memoization primitive (`get(key, compute_fn)`) with its
  own monotonic `version()`, bumped on every real recompute. A downstream
  stage composes its own cache key from an upstream stage's `version()`
  instead of manually re-deriving everything the upstream depends on —
  the fix for a caching-bug class where a new upstream trigger (e.g.
  `Root::mutation_version()`) had to be hand-copied into every downstream
  key or a change silently went unseen. `CachedStage<Key, Value>` is a
  backward-compatible alias for `VersionedStage`, still used by `render`'s
  own stages.
- `src/pipeline/` — `Pipeline` chains five stage classes, one per file
  under `src/pipeline/stages/`, each built on `core`'s `VersionedStage`:
  `GenerateShapesStage` → `FilterByViewportAndSizeStage` →
  `FilterByLayerVisibilityStage` (the `run()` chain) and
  `GenerateShapesStage` → `TinyShapesByViewportStage` →
  `TinyShapesByLayerVisibilityStage` (the `run_tiny_shapes()` chain,
  sharing `GenerateShapesStage` with the first). `Pipeline` itself is a
  thin owner of one instance of each stage plus orchestration
  (`run`/`run_tiny_shapes`/`hit_test_point`/`hit_test_rect`) — every
  public method keeps its original signature, delegating to its stage in
  one line; reuse one `Pipeline` instance per `Scene`-equivalent
  lifetime. `GenerateShapesStage::run` resolves each `Shape` straight to
  its `ViewLayerId` in the same pass (no separate resolve stage), merges
  each Shape's own overlapping rects/polygons via
  `Geometry::merge_overlapping_fills`, and attaches one text label per
  distinct layer a Terminal has geometry on.
  `FilterByLayerVisibilityStage`/`TinyShapesByLayerVisibilityStage` group
  into `std::map<ViewLayerId, ...>` (not `unordered_map`) — deliberate,
  since `ViewLayerId`'s ordering matches LEF-declared layer stacking
  order, giving correct bottom-up draw order for free; don't change this
  to `unordered_map`. See each stage class's own doc comment for its
  exact cache key and why it's shaped that way, and `BENCHMARKS.md` for
  current numbers and history. Fully covered by `pipeline_test.cpp`.
- `src/render/` — `Renderer` chains eight stage classes, one per file
  under `src/render/stages/` (each built on `core`'s `VersionedStage`;
  `render` links `core` directly, not `pipeline` — it never names the
  `Pipeline` class itself, only takes its output by reference), plus two
  shared support headers: `pixel_types.hpp` (`PixelShape`/`PixelBuffer`/
  `RasterizedFrame`/etc., pixel-space mirrors of the dbu-space types in
  `core`) and `draw_helpers.hpp` (style constants and free Skia drawing
  functions - `draw_grid`, `draw_group`, `pattern_shader`, etc. - shared
  across several stage classes). `render.hpp` itself is a thin aggregator
  - the `Renderer` class, one member per stage, every public method a
    one-line delegate — same shape as `Pipeline`'s own refactor.
    `RasterizeStage` (SkPicture → Y-flipped RGBA8888 `RasterizedFrame`) is
    instantiated three times (design/tiny-shapes/selection-overlay frames)
    rather than three separate classes — those three methods had identical
    bodies, differing only in which picture and which upstream `.version()`
    fed them (UPDATES.md item 16 point 4's "generic class for future
    expansion"). Unlike `Pipeline`, cache keys compose via upstream
    `.version()` **uniformly**, including two stages
    (`BuildPictureStage`/`RasterizeStage`) whose pre-refactor keys
    deliberately did _not_ trust upstream freshness (a fix for two earlier
    real bugs — see git history / BENCHMARKS.md 2026-08-12's Renderer
    entry for the full trade-off writeup) — composing means a caller that
    reuses a stale artifact without re-running its upstream now silently
    gets a stale-but-cached frame; verified every real call site
    (`api.cpp`, `render_preview.cpp`, every benchmark) always re-runs the
    full chain in order, so this isn't a live risk today.
    `Renderer::render(root, shapes, tiny_shapes, scene, view_layers)` wires
    the whole nine-call chain (mirrors `Pipeline::run()`'s own role) - takes
    `Pipeline`'s output (`shapes`/`tiny_shapes`, `core`'s own boundary
    types), not a `Pipeline&`, since `render` doesn't link `pipeline`; the
    caller runs `Pipeline::run()`/`run_tiny_shapes()` first. `api.cpp`'s
    `le_render_pixel_buffer` and `render_preview.cpp` both just call this
    now instead of wiring all nine calls by hand. Every individual stage
    method still exists alongside it for partial-chain callers (isolation
    benchmarks in `pipeline_benchmark.cpp`, `render_test.cpp`) - `render()`
    is a convenience wrapper, not a replacement.
    `TransformToPixelsStage` (dbu→pixel, no Y-flip) → `BuildPictureStage`
    (Skia `SkPictureRecorder` draw calls) → `RasterizeStage` (SkPicture →
    raw `PixelBuffer`) is the main chain; `RasterizeStage`/`ComposeWithOverlaysStage`
    use explicit `kRGBA_8888_SkColorType` (not Skia's platform-native
    `kN32_SkColorType`) so byte layout matches between the macOS dev
    machine and the Linux target, and apply the Y-axis flip
    (`TransformToPixelsStage` deliberately doesn't) as one whole-canvas
    transform — `draw_group` (in `draw_helpers.hpp`) counter-flips each
    text label locally to keep glyphs upright under that flip, so the two
    are coupled; check both if touching either. `render` is a compiled
    library (`add_library(render STATIC src/render/render.cpp)`), not
    header-only like its siblings — isolates `SkFontMgr_mac_ct.h`/
    `ApplicationServices.h` (legacy Carbon `Rect`/`Point`/`Polygon`
    typedefs collide with `le::` types under `using namespace le`) to
    `render.cpp`, which now defines a free `default_typeface()` function
    (declared in `draw_helpers.hpp`) rather than a `Renderer::` static
    method; don't change this back to `INTERFACE`. Single-threaded — see
    README's Threading open design question and `BENCHMARKS.md` for
    current warm-path numbers. Fully covered by `render_test.cpp`,
    including real pixel-byte assertions, not just "didn't crash". Depends
    on a machine-specific Skia checkout, not committed to this repo — see
    Open gaps below.
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
- `src/tcl/` — `le_api.i` (SWIG), `le_tcl_shim.hpp`/`.cpp`, `le_tcl_procs.tcl`:
  a Tcl-facing scripting surface wrapping `api.hpp` (see TCL_EXPLORATION.md),
  distinct from `src/api/`'s Dart-FFI-facing one — domain verb command
  names, no visible handle, friendly string ids (`"terminal:NAME"`/
  `"layer:M1"`/`"shape:3"`, name-based or numeric depending on the class -
  see `le_tcl_shim.hpp`'s own "IDs" comment) instead of raw `Le*Id` structs.
  `le_shell` (Tcl_Main-based) and any `tclsh` can both load `le_tcl.so` and
  source `le_tcl_procs.tcl`. Property *reading* (property tables,
  friendly-id resolution, `is_child` enumeration), `get_<type>` search, and
  `create_<type>` are all generated uniformly for every TCL-readable class —
  see "TCL codegen" below; only `delete_X`/`set_X_<field>` stay hand-written,
  currently for `Terminal`/`TerminalPort`/`Obstruction`/`Shape` (+
  `Abstract`'s boundary) — the classes this MVP actually edits at all beyond
  creation, not read-only LEF technology reference data (`Technology`/
  `Layer`/`Via`/...), which nonetheless still gets a generated `create_<type>`
  like every other class (nothing calls it today, but it costs nothing extra
  to generate uniformly). Fully covered by
  `src/tcl/tests/smoke_test.tcl`/`crud_test.tcl`/`shell_test.tcl` (run via
  `tclsh8.6`, not the generic `tclsh` — see the `build-test` skill).
- `src/lefdef/` — vendored LEF/DEF 6.0.62-p004 C parser source (Si2 distribution).
  Built by its own `Makefile` via `ExternalProject_Add` in the top-level
  `CMakeLists.txt`; only `lef/` is wired into the build so far (`def/` is
  vendored but unbuilt until a DEF reader exists). Never hand-edit — it's
  third-party source, license in `src/lefdef/{lef,def}/LICENSE.TXT`.
- Each module's tests live alongside it in a `tests/` subdirectory (e.g.
  `src/database/tests/database_test.cpp`), hand-written GTest.

## Database codegen (codegen)

Generated code follows the **INDEXED_POOLS** export style, produced by this
project's own `codegen` fork (repo root: `codegen/` — a project-specific
fork of [cmg](https://github.com/johndru-astrophysics/cmg), which stays
generic/reusable; `codegen` owns this project's own display/formatting
conventions instead, e.g. the `dbu` field type and its LEF/DEF unit-conversion
formatting — see `codegen/codegen/schema.py`'s `TYPEMAP` and
`Field.wrap_with_to_property*`). Every `Klass` in `schema.py` becomes:

- `XxxData` — a plain data struct.
- `XxxId` — a `{index, generation}` handle (see `generated/ids.hpp`), not a
  pointer, fully ordered (usable as a `std::map` key with no custom comparator).
- Storage in a `Pool<XxxData, XxxId>` (`generated/pool.hpp`) — a generational
  slot array, so erased objects can't alias a reused slot.
- `Root` (`generated/root.hpp`) owns every pool plus an `index_` for
  parent→children and lookup-by-field indices, and exposes
  `create_x`/`get_x`/`get_x_ids`/`for_each_x_id`/`clear_x`/`get_x_size` per class.

A field's `has_pool` defaults `True` — a `Klass` is embedded (a plain value
type inline in its owner's `XxxData`, e.g. `Point`/`Rect`/`Symmetry`) only by
explicitly setting `has_pool=False`. Converting an embedded struct to pooled
(add a back-reference `parent=` `Field` on it per owner relationship, mark
the owner's own field `is_child=True`) needs no template changes — every
pool-backed `Klass` gets the same `create_x`/`get_x`/`get_<owner>_<field>()`
surface uniformly, whether it's one of the ~15 originally-pooled top-level
classes (`Layer`/`Via`/`Terminal`/...) or one of the ~20 former embedded
structs pooled in a later round specifically so they'd also get their own
generated property table and (see below) `create_<type>` command.

`Field.unique_per_parent` (paired with `index=True`) makes `create_x`
fallible for that `Klass`: it builds a per-parent-scoped index (nested by the
owning `Klass`'s own parent field) instead of the default flat/global one a
plain `index=True` field gets, and returns an invalid id — without
inserting — if a sibling under the same parent already has that value,
instead of always succeeding. `Terminal.name` is the only field using this
today (a Terminal's name only needs to be unique within its own Abstract,
not globally — real LEF libraries reuse pin names like VDD/IN0 across
different Abstracts) — see `Field.unique_per_parent`'s own docstring in
`codegen/codegen/schema.py` for the full mechanism (nested index shape,
`create_x`/`set_x_<field>`/`delete_x` bookkeeping, the `get_x_by_<field>`
accessor's parent-scoped signature).

To change the schema: edit `src/database/schema.py`, bump `Schema.version`
(only needed for a real field/class shape change, not a pure codegen-side
formatting change), then regenerate with the `regen-database` skill rather
than editing `generated/` by hand. Real test coverage lives in each module's
own `tests/` directory, not `generated/` — codegen doesn't emit test files.

## TCL codegen (codegen, `--target tcl`)

A separate generation target from the database one above (`regen-tcl`
skill, not `regen-database`) — covers `src/tcl/`'s property-*reading*,
`get_<type>` *search*, and `create_<type>` surface: `src/api/generated_tcl/`
(`ids.inc`/`declarations.inc`/`handle_fields.inc`/
`property_accessors_internal.inc`/`property_accessors_public.inc`/
`filter_tables.inc`/`search.inc`, `#include`d from `api.hpp`/`api.cpp`) and
`src/tcl/generated/` (`le_tcl_shim_generated.hpp`/`.inc`,
`le_api_generated.i`, `le_tcl_procs_generated.tcl`,
`#include`d/`%include`d/`source`d from
`le_tcl_shim.hpp`/`.cpp`/`le_api.i`/`le_tcl_procs.tcl`). Every pool-backed
`Klass` gets a generated property table, friendly-id resolution,
`is_child`-field enumeration, a `get_<type>` search command, and a
`create_<type>` command by default (`Klass.tcl_readable`/`Klass.tcl_id_field`
in `codegen/codegen/schema.py` — see the `regen-tcl` skill for the opt-out/
override mechanics and the full list of injection points) — uniformly
across all ~35 classes today (including `Terminal`/`TerminalPort`/
`Obstruction`/`Shape`, whose `create_X` used to be hand-written —
`codegen/codegen/tcl_generator.py`'s `HAND_WRITTEN_CRUD_CLASSES` now only
documents which classes *also* have hand-written `delete_X`/`set_X_<field>`,
not `create_X` anymore, and not an exclusion set for this target in general).
`Klass.has_current_access = True` (`Technology`/`Abstract`/`Schematic`) marks
a class with a generated "current instance" concept (`current_X`/
`set_current_X` — independent of any other "current view" state elsewhere,
e.g. `Scene::current_abstract()`, which drives GUI rendering) that every
*other* readable class's `get_<type>` default scope (`-of` omitted) derives
from automatically, purely from schema graph structure — see
`codegen/codegen/tcl_scope.py`'s own module docstring for the algorithm, and
the `regen-tcl` skill for the full injection-point list.

`create_<type>` covers one flag per scalar field (`str`/`int`/`double`/
`dbu`/`bool`/enum) — `is_child`/`is_list` fields and embedded-struct-typed
fields stay out of scope (a future `add_X`/`set_X` round, matching how
`Shape`'s own rect/polygon/path CRUD is already separate from
`create_shape`). A flag is required iff `Field.create_required()` (mirrors
`is_optional`, except `bool` is always optional — `false` is already a
zero-cost "not specified" default, so requiring every boolean flag on every
call would be pure noise); an *omitted* optional flag ends up genuinely
unset (`std::nullopt`), not a zero-value default — a `str`/enum field passes
`nullptr` through the C layer (Tcl can't produce a null `const char*`
directly, so an empty string is treated as "omitted", the same convention
this codebase's hand-written `-flag` parsing already used before this
generator existed), a numeric field gets a companion `has_<field>` int32.
`dbu` fields cross the C boundary in microns (`<field>_um`, converted via
`database_units_microns()`/`to_dbu()`), and an enum field crosses as its
`to_string()`/`from_string()` spelling (e.g. `"INPUT"`, parsed via the
matching generated `<enum>_from_string()` — see `enum_hpp_j2.py` — not a raw
numeric code). A multi-parent class (`Shape.terminal_port`/`.obstruction`,
`ViaLayer`, `Foreign`, `LayerDensityEntry`'s `ac_layer`/`dc_layer`) takes one
`Le<Parent>Id`/token flag per parent field, generically validated to require
*exactly one* resolving (not zero, not both) — this is what unified the
formerly hand-written `create_terminal_port_shape`/`create_obstruction_shape`
split into one generated `create_shape -terminal_port|-obstruction`. All of
this construction logic (per-field validation, the exactly-one-parent check,
the `<Klass>Data{...}` initializer) is built as one Python string in
`Klass.create_api_body()` (`codegen/codegen/schema.py`), not deeply nested
Jinja — the per-field-type/optionality branching reads far more clearly as
real Python control flow. `delete_X`/`set_X_<field>` stay a separate,
not-yet-built effort.

## Open gaps (tracked in README's Plan checklist)

- `src/lefdef/def` is vendored but not yet wired into `CMakeLists.txt` — add
  an `ExternalProject_Add(def_lib ...)` (mirroring `lef_lib`) when a DEF
  reader module is added.
- Skia isn't vendored/built by this project — `src/render/`'s
  `CMakeLists.txt` `skia` target points `SKIA_DIR` at a pre-built checkout
  (default `/Volumes/Docking/Projects/synthosilicon/skia/skia`, override with
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

A second tree, `build_release` (`-DCMAKE_BUILD_TYPE=Release`), is also
expected to exist and be kept up to date alongside `build` —
`flutter_plugin/macos/lef_editor_plugin.podspec` links `build_release`'s
`api`/`render`/`io` output directly (a real running Flutter app needs
actual optimized performance, not debug-build timings), so it's a
persistent tree, not a throwaway benchmarking artifact. See the
`build-test` skill and `flutter_plugin/CLAUDE.md`'s Native linking
section.

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

**Gotcha:** `ENABLE_COVERAGE` is a _cached_ option — reconfiguring with e.g.
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

- `regen-database` — regenerate `src/database/generated/` from `schema.py` via the local `codegen` fork.
- `regen-tcl` — regenerate `src/tcl`'s generated property-reading surface from `schema.py` via the local `codegen` fork's `tcl` target.
- `build-test` — configure/build/test the CMake project once one exists.
- `cpp-review` — review pending changes for missing test coverage, unnecessary
  allocations/copies/moves, memory safety, and other issues; reports via
  `ReportFindings`, doesn't apply fixes. Named to avoid colliding with the
  built-in, billed `/code-review ultra`.
