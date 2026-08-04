# Pipeline Benchmarks

Results from `src/pipeline/benchmarks/pipeline_benchmark.cpp` — a synthetic
1M-shape single-macro LEF (10% PINs / 90% OBS, alternating M1/M2 layers and
RECT/POLYGON/PATH geometry). Run with:

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_COVERAGE=OFF
cmake --build build --target pipeline_benchmarks
./build/pipeline_benchmarks --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
```

`ENABLE_COVERAGE` is a cached CMake option — always pass it explicitly as
`OFF` when reconfiguring for a benchmark, or a value left over `ON` from a
coverage run will silently force `-O0` onto a nominally-Release build (see
CLAUDE.md's Coverage gotcha). Machine: 10-core Apple Silicon Mac (macOS),
Clang.

## 2026-08-03 — 4-stage pipeline (generate → viewport/size-filter → resolve view layers → layer-visibility-filter)

Commit: `71f9acd` (Add view_style module and 4-stage pipeline with view-layer resolution)

| Stage                         | Mean     | Median   | stddev   | cv    |
| ----------------------------- | -------- | -------- | -------- | ----- |
| `generate_shapes`             | 46.9 ms  | 46.8 ms  | 0.274 ms | 0.58% |
| `filter_by_viewport_and_size` | 4.50 ms  | 4.53 ms  | 0.059 ms | 1.31% |
| `resolve_view_layers`         | 1.89 ms  | 1.89 ms  | 0.011 ms | 0.56% |
| `filter_by_layer_visibility`  | 0.825 ms | 0.824 ms | 0.008 ms | 1.01% |
| `run()` (all 4 stages)        | 55.7 ms  | 55.6 ms  | 0.219 ms | 0.39% |

Comment: confirms `resolve_view_layers` as its own stage _after_ the
viewport/size filter is the right call — it only pays the `Layer`-by-name +
purpose lookup on the ~25% of shapes that survive viewport culling (1.89ms),
not the full 1M (an earlier design that resolved during `generate_shapes`
measured ~59.8ms for `run()` alone, ~46% slower than this). `generate_shapes`
remains the dominant cost (~84% of `run()`) and is the next optimization
target if pipeline throughput ever needs to improve, not stage order.

## 2026-08-03 — PipelineCache v1: cascading per-stage caching, resolve chained to the viewport filter

**Superseded same-day by the "PipelineCache v2" entry below** — kept for
history. v1 chained `resolve_view_layers`'s cache validity to the viewport
filter's (matching `Pipeline::run()`'s stage order exactly), which meant
`resolve_view_layers` re-ran on every pan/zoom frame even though its real
inputs (`root`/`view_layers`) never changed. v2 decouples it, keying it on
`AbstractId` alone instead.

Commit: `71f9acd` + uncommitted (`PipelineCache`, `Scene::viewport_version()`/
`visibility_version()`, new `BM_RunCached_*` benchmarks below)

`PipelineCache` wraps `Pipeline`'s four stages with cascading last-seen-input
comparison (`AbstractId` for `generate_shapes`, `Scene::viewport_version()`
for `filter_by_viewport_and_size`, `Scene::visibility_version()` for
`filter_by_layer_visibility`; `resolve_view_layers` has no independent
trigger — it shares stage 2's cache validity, see `pipeline_cache.hpp`).
Benchmarked against the uncached `BM_Run` baseline above, repeatedly calling
`PipelineCache::run()` while varying only one thing per call:

| Scenario                                                                | Mean      | vs. uncached `BM_Run` (55.7ms)   |
| ----------------------------------------------------------------------- | --------- | -------------------------------- |
| `BM_RunCached_NoChange` (nothing changed — steady state)                | ~0.000 ms | full cache hit, effectively free |
| `BM_RunCached_PanOnly` (pan changes every call)                         | 7.83 ms   | ~7.1x faster                     |
| `BM_RunCached_VisibilityOnly` (a layer's visibility toggles every call) | 0.579 ms  | ~96x faster                      |

Comment: matches the design intent exactly. Pan/zoom is the dominant
interactive case and it still has to re-pay `filter_by_viewport_and_size` +
`resolve_view_layers` + `filter_by_layer_visibility` every call (~7.1ms,
close to the sum of their uncached costs: 4.46+1.83+0.82ms) — but skipping
`generate_shapes`'s 46.8ms (84% of the uncached total) is exactly the win
the cache targets, since `generate_shapes` is invariant across pan/zoom as
long as the displayed Abstract doesn't change. Visibility-only changes are
even cheaper (0.579ms) since three of the four stages stay cached. First pass at this benchmark accidentally captured `PipelineCache::run()`'s
`const&` return via `auto` instead of `const auto&` (a hidden full-vector
copy on every "cache hit") — inflated `NoChange` to 0.8ms instead of ~0ms;
fixed before recording these numbers.

## 2026-08-03 — PipelineCache v2: resolve_view_layers cached per-AbstractId, decoupled from the viewport filter

Commit: `71f9acd` + uncommitted. Chain reordered to
`generate → resolve → viewport-filter → layer-filter` inside `PipelineCache`
only — `Pipeline::run()`'s own stage order (`generate → viewport-filter →
resolve → layer-filter`) is unchanged and still correct for a single
uncached call. `resolve_view_layers` now runs on the *full* generated set
(not the viewport-filtered survivors) so its cache key is `AbstractId`
alone, same as `generate_shapes` — it no longer re-runs on pan/zoom.
`filter_by_viewport_and_size` was templated (it only touches `.shape`) so
it can filter `RenderedShape` post-resolve instead of only `TaggedShape`
pre-resolve.

| Benchmark | Mean | Notes |
|---|---|---|
| `BM_ResolveViewLayers` (250K post-viewport-filter survivors, unchanged) | 1.88 ms | ~7.5 ns/shape |
| `BM_ResolveViewLayersOnFullSet` (all 1M shapes) | **40.3 ms** | ~40 ns/shape — 5.4x worse per-shape, not flat |
| `BM_RunCached_ColdStart` (fresh `PipelineCache`, one `run()` — new Abstract selected) | **97.7 ms** | vs. 55.7ms uncached `BM_Run` — **+42ms** |
| `BM_RunCached_NoChange` | ~0.000 ms | unchanged from v1 |
| `BM_RunCached_PanOnly` | **6.11 ms** | down from v1's 7.83ms — **-1.7ms/frame** |
| `BM_RunCached_VisibilityOnly` | 0.614 ms | essentially unchanged from v1's 0.579ms |

Comment: the initial design rationale (extrapolate ~7ms for resolving 1M
shapes from the 1.88ms/250K figure, assuming a flat per-shape lookup cost)
was wrong — measured cost is 40.3ms, not ~7ms. Investigated whether this is
algorithmic: `Root::get_layer_by_name` is a genuine O(1)
`unordered_map<string, LayerId>` lookup and `ViewLayerSet::find` scans a
fixed ~5-entry list either way — neither scales with shape count, and the
two benchmarks are structurally identical apart from input size, so this
isn't a benchmark-harness artifact. Best available explanation (not
profiler-confirmed): `resolve_view_layers` materializes a full second copy
of however many `Shape`s it's given — each with one heap allocation for its
populated geometry vector — and copying ~1M of them appears to hit
cache/TLB/allocator effects that make it disproportionately worse than
copying 250K, not just proportionally worse. This is the same reason
`generate_shapes` itself costs ~47-50ms for 1M shapes: both stages are
dominated by materializing ~1M `Shape` copies, not by per-shape lookup
logic. In other words, this is the "cull before you pay an expensive
per-shape cost" lesson from the 4-stage-pipeline entry above, recurring one
layer up: running `resolve_view_layers` on the full set means paying that
expensive copy pass *twice* (once in `generate_shapes`, once here) instead
of once-on-1M-plus-once-on-250K.

Net effect: Abstract-switch latency nearly doubles (55.7ms → 97.7ms, +42ms)
to save ~1.7ms on every subsequent pan/zoom frame on the same Abstract.
Break-even is ~25 pan/zoom frames per Abstract view — almost certainly
crossed in any real interactive session, so this wins on total session
time — but it's a real, user-perceptible latency regression at the moment
of switching Abstracts, not a free win. Decision: keep this design, since
pan/zoom/select dominates real usage per the target workflow (read LEF once
→ pick an Abstract → many viewport/selection changes). If `resolve_view_layers`'s
absolute cost ever matters on its own, profiling the copy-volume hypothesis
above (e.g. avoiding the second full `Shape` copy) is a bigger potential win
than this caching change alone — not pursued now.

## 2026-08-03 — PipelineCache merged into Pipeline (structural refactor, no behavior change)

`PipelineCache` was hard to read (cascading invalidation via manually-set
boolean flags scattered across four private methods) and required a
separate class from `Pipeline`. Restructured: `Pipeline`'s four stage
methods are now non-static instance methods, each owning a small
`CachedStage<Key, Value>` member (remembers the last key/compute-result
pair, recomputes only when the key changes - not a general reactive
framework, just enough to replace the hand-written flags). Each stage
chains to the previous one internally and its own key already includes
every upstream trigger it transitively depends on, so cascading
invalidation now falls out of key comparison for free. `Pipeline::run()`
reads as a flat 4-line sequence again, identical in spirit to the original
pre-caching version. `pipeline_cache.hpp`/`PipelineCache` are gone; one
`Pipeline` instance now lives per `Scene`-equivalent lifetime instead.

This is a pure structural change - same cache keys, same stage order, same
algorithm. Re-ran the full benchmark suite to confirm nothing regressed:

| Benchmark | Before (PipelineCache v2) | After (merged Pipeline) |
|---|---|---|
| `resolve_view_layers` on full 1M set | 40.3 ms | 41.5 ms |
| Cold start (fresh instance, one `run()`) | 97.7 ms | 96.7 ms |
| Reused, pan-only | 6.11 ms | 6.30 ms |
| Reused, visibility-only | 0.614 ms | 0.625 ms |
| Reused, no change | ~0.000 ms | ~0.000 ms |

All within normal run-to-run noise - confirms the merge didn't change
performance, only readability/structure. Isolated-stage benchmarks
(`BM_GenerateShapes`, `BM_FilterByViewportAndSize`, `BM_ResolveViewLayers`,
`BM_FilterByLayerVisibility`, `BM_Run`) now construct a fresh `Pipeline`
per iteration to keep measuring true uncached cost, since the stage
methods cache internally; `BM_RunCached_*` renamed to `BM_RunReused_*` to
reflect that they now exercise the same `Pipeline::run()` a real caller
would use, just with one instance reused across iterations instead of
constructed fresh. The old `BM_ResolveViewLayers` (measured on the
250K post-viewport-filter subset, no longer a real code path since resolve
always runs on the full set now) and `BM_RunCached_ColdStart` (now
identical in meaning to `BM_Run`) were dropped as redundant; their findings
remain in the entries above.

## 2026-08-03 — resolve_view_layers merged into generate_shapes, TaggedShape removed

Since the `PipelineCache` merge (previous entry), `generate_shapes` and
`resolve_view_layers` were already keyed on the same cache key
(`AbstractId` alone) and always recomputed together - they never had
independent cache lifetimes. The original reason they were split apart
(resolving during generation cost ~46% more, per the 4-stage-pipeline
entry near the top of this file) no longer applied: that finding was for
a design where viewport-filtering ran *between* generate and resolve,
culling to ~25% of shapes first; the current design already runs resolve
on the *full* generated set regardless of whether it's a separate stage.
With both stages always processing the same 1M shapes together, keeping
them separate just meant two full deep-copies of the shape data (one
`vector<TaggedShape>` in generate, one `vector<RenderedShape>` in resolve)
where one now suffices - matching the copy-volume hypothesis from the
"PipelineCache v2" entry above. Merged: `generate_shapes` now resolves each
shape's `ViewLayerId` inline as it's built, directly into `RenderedShape`;
`TaggedShape` is gone entirely, and the pipeline is down to 3 stages
(generate → viewport-filter → layer-filter) instead of 4.

| Benchmark | Before (separate generate + resolve) | After (merged) |
|---|---|---|
| `BM_Run` (cold start, fresh instance) | 96.7 ms | **58.9 ms** |
| `generate_shapes` alone (1M shapes) | ~52.6 ms (generate) + ~40.3 ms (resolve) ≈ 92.9 ms combined | **52.6 ms** |
| Reused, pan-only | 6.30 ms | 5.91 ms |
| Reused, visibility-only | 0.625 ms | 0.587 ms |
| Reused, no change | ~0.000 ms | ~0.000 ms |

Comment: confirms the prediction from the design discussion - cold-start
`run()` dropped ~39% (96.7ms → 58.9ms), and the merged `generate_shapes`
costs far less than the sum of the two stages it replaced (52.6ms vs.
~92.9ms), consistent with eliminating one full `Shape`-copy pass over 1M
elements. The warm/interactive path (pan-only, visibility-only, no-change)
is unchanged or marginally better, as expected - those stages weren't
touched. This directly resolves the "avoiding the second full `Shape`
copy" follow-up flagged as a bigger potential win in the "PipelineCache
v2" entry above, without needing any profiling to confirm it - the
benchmark speaks for itself.

## 2026-08-03 — render module: dbu→pixel transform + SkPicture generation

New `src/render/render.hpp`: `Renderer` adds two more `CachedStage`-backed
stages on top of `Pipeline`'s output - `transform_to_pixels` (dbu→pixel,
`pixel = (dbu - pan) * scale`, no Y-flip) and `build_picture` (Skia
`SkPictureRecorder`/`SkCanvas` draw calls using each shape's
`ViewLayerStyle`), both keyed the same way `Pipeline::filter_by_layer_visibility`'s
output already is. Single-threaded - see the Threading comment below for
why that's staying true for now, backed by these numbers rather than
guessed.

| Benchmark | Mean | Notes |
|---|---|---|
| `BM_TransformToPixels` (fresh instance, ~250K visible shapes) | 0.715 ms | isolated stage, uncached |
| `BM_BuildPicture` (fresh instance, ~250K visible shapes) | 1.43 ms | isolated stage, uncached |
| `BM_Run` (pipeline only, cold) | 59.5 ms | baseline from the entry above |
| `BM_Render` (pipeline + render, cold) | 62.0 ms | +2.5ms over `BM_Run` |
| `BM_RunReused_PanOnly` (pipeline only, warm) | 5.81 ms | baseline from the entry above |
| `BM_RenderReused_PanOnly` (pipeline + render, warm) | 8.56 ms | +2.75ms over `BM_RunReused_PanOnly` |
| `BM_RenderReused_NoChange` | ~0.000 ms | full cache hit across pipeline + render |

Comment: rendering adds a small, bounded cost on top of the pipeline
regardless of cold or warm path - roughly +2-2.75ms, consistent with the
isolated stage costs (0.715+1.43≈2.15ms) plus some noise. `SkPicture`
*recording* (building the draw-command list) is genuinely cheap at this
scale, as expected - the expensive part of a real rendering pipeline is
usually rasterizing a picture to actual pixels, which nothing here does
yet (that's a Flutter-texture-facing concern, not this module's).

**Threading decision, backed by these numbers**: the warm/interactive path
(pan-only, the dominant real-usage case) is 8.56ms total with rendering
included, at 1M synthetic shapes - comfortably under a 60fps/16.6ms frame
budget with margin to spare. No benchmark here shows Skia picture
generation as a bottleneck, so per this project's own rule (benchmark
before optimizing/threading, not intuition) there's no case for adding a
background thread yet. Single-threaded stays the right choice until a
future benchmark - larger data, real font/text rendering, or actual pixel
rasterization once that's added - shows otherwise.

## 2026-08-03 — Pipeline groups by ViewLayerId (bottom-up draw order) + Terminal text labels

Two features landed together since they touch the same methods:

- `Pipeline::filter_by_layer_visibility` (and `run()`) now return
  `std::map<ViewLayerId, std::vector<RenderedShape>>` instead of a flat
  vector - grouped by ViewLayer, checking visibility once per distinct
  ViewLayerId instead of once per shape, with map iteration order giving
  correct bottom-up draw order for free (`ViewLayerId`'s `{index,
  generation}` ordering, via `Id<Tag>`'s new defaulted `operator<=>` in
  `cmg`'s `ids_hpp_j2.py` template, exactly matches LEF-declared physical
  layer stacking order - confirmed by tracing `create_layer`'s append-only
  vector through `ViewLayerSet::build_for_technology`, not assumed).
  `Renderer` follows suit: `PixelShape` drops its now-redundant
  `view_layer` field (the map key carries it), and `build_picture`
  constructs each ViewLayer's fill/stroke `SkPaint` once per group instead
  of once per shape.
- `Pipeline::generate_shapes` now attaches one text label per Terminal
  (its name, placed via `Geometry::get_label_location` on the union of all
  its Ports' geometry), riding along on a real geometric Shape rather than
  a standalone text-only one so it survives `filter_by_viewport_and_size`'s
  bbox check. `Renderer` draws it via Skia's `SkFont`/`drawString`, using a
  CoreText-backed default typeface on macOS (see the Skia setup note in
  `CLAUDE.md` - Linux needs an equivalent added later).

| Benchmark | Before | After | Cause |
|---|---|---|---|
| `BM_GenerateShapes` (1M shapes) | 52.6 ms | 101 ms | +48.4ms, ~36ms confirmed via isolated micro-benchmark to be `Geometry::get_label_location`'s cost across 100K Terminals (~356ns/call) |
| `BM_FilterByLayerVisibility` (~250K shapes) | 0.82 ms | 2.28 ms | std::map grouping (tree insert per shape) vs. a flat vector scan |
| `BM_Run` (cold) | 58.9 ms | 108 ms | dominated by `generate_shapes`'s regression above |
| `BM_RunReused_PanOnly` (warm) | 5.91 ms | 8.47 ms | dominated by `filter_by_layer_visibility`'s regression above |
| `BM_RunReused_VisibilityOnly` | 0.587 ms | 2.78 ms | ~entirely `filter_by_layer_visibility`'s regression (the only stage that reruns) |
| `BM_BuildPicture` (~250K shapes) | 1.43 ms | 2.30 ms | now actually draws each Terminal's label text |
| `BM_RenderReused_PanOnly` (warm, full chain) | 8.56 ms | 12.3 ms | sum of the `filter_by_layer_visibility` and `build_picture` regressions above |

Comment: every regression here is understood and attributed to a specific,
real, confirmed cause - not a mystery or an accidental inefficiency left
unexamined. `Geometry::get_label_location`'s cost was verified directly
with a standalone 100K-call micro-benchmark (355.9 ns/call, 35.6ms total)
before accepting it as inherent to the label feature rather than a bug.
One real inefficiency *was* caught and fixed along the way: the first
implementation collected each Terminal's Port Shapes into a fresh
`std::vector<Shape>` per Terminal (a needless extra heap allocation and
partial copy per Terminal that didn't exist before this feature) before
computing the label and moving them into the final result; restructured to
two passes over the same Port/Shape iterators - one accumulating just the
lightweight geometry primitives (not whole Shapes) into a combined Shape
for label placement, one copying Port Shapes directly into the final
vector exactly as before this feature existed - which recovered about 5ms
of that regression (106ms → 101ms) without touching the inherent
label-placement cost.

Despite the absolute increase, the warm/interactive path is still well
within budget: 12.3ms per pan/zoom frame at 1M shapes remains comfortably
under a 60fps/16.6ms budget, and the [pipeline latency budget]
memory - cold-start (Abstract switch) can afford up to ~1-2s behind a
loading spinner - means the ~2x `generate_shapes`/cold-start regression is
not a problem worth chasing further right now. Not pursued as a follow-up:
optimizing `get_label_location` for the common single-rect case (skip the
boost::geometry union/point-in-polygon machinery when there's exactly one
rect and no other geometry) would recover most of the remaining cost, but
nothing currently requires it.

Follow-up cleanup: the two-pass structure above (build `combined`, then
re-iterate the same Ports/Shapes to push them) was simplified to one pass -
push each Shape immediately, remembering the first one's index, attach the
label to it after the loop once `combined`'s label point is known. Using
`combined` itself as the pushed geometry (instead of each Shape
individually) was considered and rejected: a Terminal's Shapes can
legitimately span different physical layers, so collapsing them would
break per-layer visibility toggling and coarsen viewport/sub-pixel culling
to one shared bbox instead of each piece's own. Re-benchmarked to confirm:
102ms/110ms (`BM_GenerateShapes`/`BM_Run`), statistically unchanged from
101ms/108ms - the eliminated re-traversal was never the dominant cost, only
the allocation fix above was. A code-clarity improvement, not a
performance one, and confirmed as such rather than assumed.

## 2026-08-03 — One label per distinct layer, not one per Terminal

A Terminal's Shapes can legitimately span multiple physical layers (e.g.
M1 and M2), and each needs its own label tied to its own `ViewLayerId` -
one label for the whole Terminal would either pick an arbitrary layer or
require the rejected single-`combined`-shape design from the entry above.
`generate_shapes` now buckets a Terminal's Shapes by `layer_name` (a local
`std::unordered_map<std::string, LabelAccumulator>`, one accumulator -
combined geometry + first-shape-index - per distinct layer) instead of one
`Shape combined` per Terminal; single-layer Terminals (the common case)
still get exactly one label, unchanged.

| Benchmark | Before | After | Cause |
|---|---|---|---|
| `BM_GenerateShapes` (1M shapes) | 102 ms | 106 ms | one `unordered_map` construction per Terminal, even single-layer ones |
| `BM_Run` (cold) | 110 ms | 112-113 ms | dominated by `generate_shapes`'s regression above |

Comment: confirmed real and consistent across two separate runs (cv <1.2%
both times), not noise - even though the *common* case (one layer per
Terminal) doesn't functionally need the map, it still pays for the hash
table's bucket-array allocation on the first insert. A fast path could
special-case "only one distinct layer_name seen so far" and skip the map
entirely until a second layer is actually encountered, avoiding that
allocation for the common case. Not pursued, for the same reason
`get_label_location`'s own ~36ms cost isn't being chased in the entry
above: this ~4ms/~4% is small relative to that already-accepted dominant
cost, and the warm/interactive path (what real usage actually hits) is
unaffected either way - `generate_shapes` is cached per-`AbstractId` and
doesn't rerun on pan/zoom/visibility changes. Revisit only if a future
benchmark shows this path actually matters more than it does today.

## 2026-08-04 — Merge overlapping rects/polygons within a Shape

Found visually against real LEF data (`render_preview` against
`Nangate45_stdcell.lef`, not the synthetic stress data): a Terminal Port's
or Obstruction's Shape can legitimately contain several rects that overlap
*each other* (e.g. an L/T-shaped pin drawn as two overlapping rects), and
`build_picture` draws each with a translucent fill - so the overlapping
region got painted twice, showing up as a visibly darker patch at the
corner. `Geometry::merge_overlapping_fills(Shape&)` (new) unions a Shape's
own rects+polygons into a minimal non-overlapping set via boost::geometry,
replacing both in place; paths are left untouched (they carry width/stroke
semantics a fill-polygon union would lose, and weren't the source of this
artifact). Called once per pushed Shape in `generate_shapes`, scoped to
that Shape alone - not across different Terminals/Obstructions, which
would destroy the per-object identity `RenderedShape`/selection relies on.

**Existing 1M-shape stress benchmark: no measurable regression**, confirmed
rather than assumed - every shape in `stress_data.hpp`'s generated LEF has
exactly one geometry item (a fresh `LAYER ;` before each one forces a new
Shape), so `merge_overlapping_fills`'s `<=1`-part fast path is a no-op for
all of it:

| Benchmark | Before | After |
|---|---|---|
| `BM_GenerateShapes` (1M shapes) | 106 ms | 105 ms |
| `BM_Run` (cold) | 112-113 ms | 112 ms |
| `BM_RunReused_PanOnly` | 8.47 ms | 8.63 ms |
| `BM_RunReused_VisibilityOnly` | 2.78 ms | 2.66 ms |
| `BM_Render` (cold, full chain) | - | 115 ms |
| `BM_RenderReused_PanOnly` | 12.3 ms | 12.6 ms |

All deltas are within each other's run-to-run noise (cv 0.3%-1.6% across 5
repetitions) - not a real change, as expected given the fast path.

Because the stress data can't exercise the actual union work, added a
dedicated isolated micro-benchmark (`BM_MergeOverlappingFills`, N
half-overlapping unit-height rects, `BM_ShapeCopyBaseline` as a control for
the Shape-copy cost `generate_shapes` already pays regardless of this
change):

| N rects | Copy alone | Copy + merge |
|---|---|---|
| 2 | 0.016 us | 3.48 us |
| 5 | 0.017 us | 13.6 us |
| 10 | 0.024 us | 30.1 us |
| 50 | 0.033 us | 176 us |

The copy cost is negligible throughout - essentially all of the added time
is `boost::geometry` union work, and it grows slightly worse than linearly
(1.74us/rect at N=2 vs. 3.52us/rect at N=50): each of the N-1 iterative
`bg::union_` calls operates on a result that's grown from the previous
ones, so per-union cost isn't constant.

Real-world check against the two actual cases that motivated this (both
via a temporary `std::chrono` timing added to `render_preview.cpp` around
`generate_shapes`, then reverted - not a permanent instrumentation): the
full `Nangate45_stdcell.lef` standard-cell library (135 designs) went from
11.1ms to 30.5ms total `generate_shapes` time across every design combined
(worst single design under 1ms both ways) - real cells' pin/OBS geometry
does have some genuinely overlapping rects, but not many per shape. The
dense `fakeram45_1024x32` SRAM macro (the "solid red block" from the
render-preview conversation) was unaffected either way (~0.6ms both with
and without) - its visual density comes from many separate shapes packed
edge-to-edge, not from many overlapping rects *within* one shape, so this
fix doesn't apply there and correctly does ~nothing.

Comment: `generate_shapes` is cached per-`AbstractId`, so this entire cost
is paid once per Abstract switch, not per frame - a full standard-cell
library's ~19ms combined increase (spread across 135 designs, each loaded
independently as its own Abstract) is not a concern. Not measured: a
design with many more overlapping rects per shape than these two real
examples happen to have (the isolated micro-benchmark above gives a per-
shape cost model - roughly `N^1.15`-ish us for N overlapping rects - for
estimating that case if it comes up).

## 2026-08-04 — Renderer::rasterize(): SkPicture -> raw RGBA8888 PixelBuffer

Added the pixel-buffer rasterization step README's Plan/open-design-
questions section flagged as the last piece of `render` before an `api`
module can exist: `Renderer::rasterize()` rasterizes `build_picture`'s
`SkPicture` into a `PixelBuffer` (raw pointer + width + height + row_bytes),
a third `CachedStage`-backed stage keyed the same way as the other two.
Two decisions the README explicitly deferred to this step, now resolved:

- **Explicit `kRGBA_8888_SkColorType`, not `SkImageInfo::MakeN32Premul`**.
  `kN32_SkColorType` is platform-native - BGRA on some platforms, RGBA on
  others (`SkColorType.h`). This project develops on macOS but targets
  Linux servers (a standing constraint, not new to this change) - using
  the native-native type would silently produce different byte layouts on
  the two platforms, invisible until eventually compared against Flutter's
  actual texture ingestion on Linux. Explicit `kRGBA_8888_SkColorType`
  guarantees identical bytes regardless of build platform.
- **Y-axis flip, applied once per frame as a canvas transform in
  `rasterize()`** (`translate` + `scale(1,-1)`), not by changing
  `transform_to_pixels`'s per-shape math or reversing output rows after
  the fact. dbu-space y increases upward (physical layout convention);
  `transform_to_pixels` maps that straight through with no flip (unchanged
  by this work), so without correction, a design's "up" would render
  toward the *bottom* of the buffer (Skia's canvas is y-down).

**A real bug found via actual visual testing, not caught by the unit
tests**: a whole-canvas flip mirrors *glyph rendering* too, not just shape
position - Terminal labels came out upside-down/mirrored ("VSS" as "SSV")
in `render_preview` output. None of the existing text tests caught this
because `BuildPictureDrawsTerminalLabelAsOpaqueTextOverTranslucentFill`
only scans for *any* opaque pixel in a region - it doesn't check the glyph
is actually legible. Fixed in `draw_group`'s text-drawing loop: each label
draw is now wrapped in a local `save()`/`translate(anchor)`/`scale(1,-1)`/
`drawString(0,0)`/`restore()` that counter-flips around its own anchor
point, canceling `rasterize()`'s whole-canvas flip for that glyph while
still letting its *position* follow the flip. This couples
`build_picture`'s `SkPicture` to being drawn through `rasterize()`'s
specific flip for text to render upright - documented in both methods'
comments, not a hidden dependency. Caught by actually looking at rendered
output (`render_preview`, now itself switched to call the real
`Renderer::rasterize()` instead of its own ad hoc `SkSurface` code, so it
exercises and visually validates the exact production path) - a reminder
that "didn't crash" and even "found an opaque pixel" pixel-level assertions
don't catch every real-world rendering bug; occasionally looking at the
actual image still matters.

Benchmarks (1M-shape stress data, clean Release, `-DENABLE_COVERAGE=OFF`,
5 repetitions, cv < 1.5% throughout unless noted):

| Benchmark | Result | Notes |
|---|---|---|
| `BM_Rasterize` (isolated, fresh `Renderer`/call) | 6.21 ms | ~2000x2000px raster surface, 1M shapes' worth of picture |
| `BM_Render` (cold, full chain incl. rasterize) | 117 ms | was 115ms before this step existed |
| `BM_RenderReused_NoChange` | ~0 ms | fully cached, as expected |
| `BM_RenderReused_PanOnly` (warm, full chain incl. rasterize) | **21.5 ms** | was 12.3-12.6ms before - **now exceeds a 60fps/16.6ms frame budget** |

**The `BM_RenderReused_PanOnly` regression is the headline finding here**,
confirmed with an in-session A/B (temporarily removed the `rasterize()`
call from that exact benchmark and re-measured in the same run, rather
than trusting a cross-session comparison against an earlier number):
12.5ms without `rasterize()`, 21.5ms with it - a genuine +9.0ms, not noise
(both measurements cv < 1.5%). That's larger than `BM_Rasterize`'s isolated
6.21ms; investigated one hypothesis (repeatedly allocating a fresh ~16MB
raster surface every uncached call - each `CachedStage::get()` cache miss
constructs a new `RasterizedFrame` before the old one is destroyed, so
there's real allocate/free churn every pan frame) with a standalone
isolated micro-benchmark of just `SkSurfaces::Raster` + `clear()` at the
same 2000x2000 size: 0.252ms, cv 0.43% - ruled out, far too small to
explain a ~2.8ms gap. More likely explanation, not further chased: `BM_
Rasterize`'s picture is fixed at one pan value (built once, outside its
timed loop), while `BM_RenderReused_PanOnly`'s picture is rebuilt every
iteration at a *different*, incrementing pan value, so each iteration's
viewport-culled shape count (and therefore what `rasterize()` actually has
to draw) genuinely varies - not a true apples-to-apples comparison of
identical workloads.

Comment: this is the first time any full-chain benchmark in this project
has exceeded the 60fps/16.6ms interactive budget - every prior entry in
this file explicitly noted staying comfortably under it. README's
Threading open design question ("no case for threading yet") was written
against pre-rasterize numbers and should be revisited with this - not done
as part of this change (out of scope for landing the rasterization step
itself), but flagged here and in README directly so it isn't lost.
