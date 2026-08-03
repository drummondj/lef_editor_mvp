# LEF Layout Editor MVP — Backend

C++23 backend that reads LEF/DEF and SystemVerilog EDA data into an in-memory
database, then renders it through a multi-threaded, layer-based pipeline into
Skia commands consumed by a Flutter plugin. This is an MVP/proof-of-concept:
the goal right now is finding the right architecture for editing hierarchical
designs with millions of objects, not shipping features. See `README.md` for
the full brief and the live plan checklist.

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
  transform, polygon union/buffer, label placement) over the database's
  `Point`/`Rect`/`Polygon`/`Path`/`Shape` types. Ported from the sibling
  `layout_engine/backend/utils/geometry.hpp`. Fully covered by
  `geometry_test.cpp`.
- `src/view_style/` — `ViewLayerSet`/`ViewLayer`, the rendering-purpose layer
  concept distinct from the LEF-mirroring `database`: a `TERMINAL` and
  `OBSTRUCTION` `ViewLayer` per physical `Layer`, plus one `BOUNDARY`
  `ViewLayer` not tied to any physical `Layer`. Hand-written (not
  cmg-generated) but reuses `database`'s generic `Id<Tag>`/`Pool<T,IdT>`
  templates directly. `ViewLayerSet::build_for_technology` builds the full
  set for a `Technology` once, shared/global — which `ViewLayer`s exist and
  how they're styled (`ViewLayerStyle`: outline/fill `Color`) isn't a
  per-`Scene` concern, only which ones are toggled off is. Fully covered by
  `view_style_test.cpp` (93.75% branch coverage is an accepted, irreducible
  gap: an exhaustive closed-enum `switch` still gets an instrumented
  "no case matched" branch region that can't be hit without UB).
- `src/scene/` — `Scene`, per-handle mutable view state (currently displayed
  `AbstractId`, pan/scale/viewport-size transform, per-`ViewLayer`
  visibility, selection). Distinct from the persistent `Root` database: the
  pipeline reads from a `Scene`, events will write into one. Layer
  visibility is keyed by `ViewLayerId` (see `src/view_style/`), not `LayerId`
  — a physical layer has independently toggleable `TERMINAL`/`OBSTRUCTION`
  visibility. Selection is `std::variant<TerminalId, ObstructionId>` — only
  the object kinds with a rendered geometric representation in an Abstract
  view today; extend the variant rather than generalizing to a type-erased
  handle before another kind (e.g. `Instance`, once a Layout/placement view
  exists) needs it.
- `src/pipeline/` — `Pipeline`, a 3-stage pass over a `Scene`'s
  `current_abstract()` (no node/task framework): `generate_shapes` →
  `filter_by_viewport_and_size` → `filter_by_layer_visibility`. Each stage
  is a non-static instance method owning a small `CachedStage<Key, Value>`
  member (remembers the last key/result pair, recomputes only when the key
  changes — not a general reactive framework, just enough to avoid
  hand-written invalidation flags) and chains to the previous stage
  internally, so `run()` reads as a flat 3-line sequence while still only
  recomputing what actually changed. One `Pipeline` instance lives per
  `Scene`-equivalent lifetime and is reused across repeated calls (e.g.
  every interactive frame) — a fresh instance recomputes everything on its
  first call.
  `generate_shapes` collects `Shape`s from Terminals' Ports, Obstructions,
  and the Abstract's boundary polygon, resolving each straight to its
  `ViewLayerId` (a `Shape::layer_name` + purpose lookup — a `Shape` has no
  `LayerId`/`ViewLayerId` field) in the same pass; `BOUNDARY`-purpose
  shapes skip the lookup entirely. There's no intermediate "tagged but
  unresolved" type — this used to be two stages (`generate_shapes` then a
  distinct `resolve_view_layers`) sharing the same `AbstractId` cache key,
  which just meant two full copies of the shape data where one now
  suffices; merged once the caching redesign made that redundancy visible
  (~39% faster cold start, 96.7ms → 58.9ms at 1M shapes — see
  `BENCHMARKS.md`). Keyed on `AbstractId` alone. The size filter culls a
  shape only if **both** bbox dimensions are under 1px at the `Scene`'s
  scale, so a long thin wire survives even though its width alone is
  sub-pixel; keyed on `AbstractId` + `Scene::viewport_version()`. The layer
  filter drops anything on a hidden `ViewLayerId`, keeping shapes whose
  `ViewLayerId` didn't resolve (e.g. an undeclared/typo'd layer name)
  rather than dropping them; keyed on `AbstractId` + `viewport_version()` +
  `Scene::visibility_version()`. Fully covered by `pipeline_test.cpp`.
  Current clean Release numbers (`-DENABLE_COVERAGE=OFF` — see the
  coverage gotcha below): fresh-instance `run()` (cold) 58.9ms;
  reused-instance pan-only 5.91ms, visibility-only 0.587ms, no-change
  ~0ms. See `BENCHMARKS.md` for the full measurement/investigation,
  including two earlier designs — a separate `PipelineCache` class (merged
  into `Pipeline` itself: cascading invalidation via manually-set boolean
  flags scattered across private methods was hard to follow and hard to
  extend, replaced by each stage owning its own `CachedStage` and chaining
  internally) and a separate `resolve_view_layers` stage (merged into
  `generate_shapes` once the caching redesign made it redundant, as above).
- `src/io/` — format readers. Currently `lef_reader.{hpp,cpp}`, which drives
  the vendored `lefr*` LEF-parser C callbacks and populates `Root` via the
  generated create/get API. Depends on `geometry` for polygon construction/union.
  Tested against `src/lefdef/lef/TEST/complete.5.8.lef` (the vendored parser's
  own regression fixture) plus small hand-written `.lef` files under
  `src/io/tests/fixtures/` for cases that fixture doesn't hit (e.g. an `OBS`
  on the `OVERLAP` layer, malformed input, duplicate names). `LEFReader` only
  supports a subset of LEF; extend the tests as more constructs get support.
  `orientation_from_parser`/`routing_direction_from_parser`/
  `signal_direction_from_parser` are `public` (unlike the rest of
  `LEFReader`) specifically so they can be unit-tested directly — they're
  pure and touch no parser/instance state.
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
- `XxxId` — a `{index, generation}` handle (see `generated/ids.hpp`), not a pointer.
- Storage in a `Pool<XxxData, XxxId>` (`generated/pool.hpp`) — a generational
  slot array, so erased objects can't alias a reused slot.
- `Root` (`generated/root.hpp`) owns every pool plus an `index_` for
  parent→children and lookup-by-field indices, and exposes
  `create_x`/`get_x`/`get_x_ids`/`for_each_x_id`/`clear_x`/`get_x_size` per class.

To change the schema: edit `src/database/schema.py`, bump `Schema.version`,
then regenerate with the `regen-database` skill rather than editing
`generated/` by hand.

Real test coverage lives in each module's own `tests/` directory, not
`generated/` — `cmg` doesn't emit test files.

## Open gaps (tracked in README's Plan checklist)

- `src/lefdef/def` is vendored but not yet wired into `CMakeLists.txt` — add
  an `ExternalProject_Add(def_lib ...)` (mirroring `lef_lib`) when a DEF
  reader module is added.
- `cmg` itself isn't installed in this environment — see the `regen-database`
  skill for setup (or run it via `poetry run cmg` from the local
  `/Users/john/Projects/synthosilicon/cmg` checkout, which is what was used
  to produce the current `generated/` output).

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
and fails (`ranlib: liblef.a is not writable`, `mv: lef.tab.c: No such file`).
The `lef_lib` `ExternalProject_Add` step already forces this with
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
written to `build/coverage/report.txt` and `build/coverage/lcov.info` for
`genhtml`/VS Code "Coverage Gutters"/Codecov). Excludes `_deps`, vendored
`src/lefdef`, and `src/database/generated` (cmg boilerplate) from the report.

Requires Clang (uses `-fprofile-instr-generate`, not GCC's `--coverage`
model) and `llvm-profdata`/`llvm-cov` — resolved via `xcrun` automatically
on macOS.

**Gotcha:** `ENABLE_COVERAGE` is a *cached* option — reconfiguring with e.g.
`-DCMAKE_BUILD_TYPE=Release` alone does **not** reset a previously-set-ON
value back to OFF, and `le_enable_coverage_instrumentation()` forces `-O0`
regardless of `CMAKE_BUILD_TYPE`. Always pass `-DENABLE_COVERAGE=OFF`
explicitly (or use a fresh `build/`) to get back to a normal, uninstrumented
build — this silently produced ~15-20x-inflated (but suspiciously
low-variance, so not obviously "noisy") benchmark numbers once already.

### Benchmarks

```
cmake --build build --target pipeline_benchmarks
./build/pipeline_benchmarks
```

Build in `-DCMAKE_BUILD_TYPE=Release` for real numbers — Debug timings aren't
meaningful. `src/pipeline/benchmarks/pipeline_benchmark.cpp` generates a
deliberately unrealistic 1M-shape single-macro LEF file (streamed straight to
`${CMAKE_BINARY_DIR}/benchmark_data/`, ~78MB, never committed — see its
comment for exactly how shapes/positions/sizes/layers are spread) and times
each `Pipeline` stage in isolation (a fresh instance per iteration, since
stages cache internally) and `run()` under several call patterns (fresh
instance/cold start, and a reused instance with no change, pan-only,
visibility-only). See the `src/pipeline/` entry above for the current
headline results, and `BENCHMARKS.md` for the full history. Add
`--benchmark_repetitions=5 --benchmark_report_aggregates_only=true` for
stable numbers when comparing two approaches, and `--benchmark_filter=<regex>`
to run a subset.

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

`../../layout_engine/backend` (sibling repo, same author) is an earlier, more
complete implementation of the same idea — same `le` namespace, same
pool/schema database pattern, a working `CMakeLists.txt` wiring up the vendored
`lefdef` libs + spdlog + fmt, and a Taskflow/Boost.Geometry/Skia render
pipeline. This MVP deliberately restarts the pipeline/rendering architecture
decisions rather than importing that one — treat it as reference and lessons
learned (its `BACKEND_REVIEW.md` has a real bug/perf review worth skimming),
not as code to copy wholesale.

## Skills

- `regen-database` — regenerate `src/database/generated/` from `schema.py` via `cmg`.
- `build-test` — configure/build/test the CMake project once one exists.
- `cpp-review` — review pending changes for missing test coverage, unnecessary
  allocations/copies/moves, memory safety, and other issues; reports via
  `ReportFindings`, doesn't apply fixes. Named to avoid colliding with the
  built-in, billed `/code-review ultra`.
