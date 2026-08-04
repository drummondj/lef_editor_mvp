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
`OFF` when reconfiguring for a benchmark (see CLAUDE.md's Coverage gotcha).
Machine: 10-core Apple Silicon Mac (macOS), Clang.

## 2026-08-03 — 4-stage pipeline (generate → viewport/size-filter → resolve view layers → layer-visibility-filter)

Commit: `71f9acd`

| Stage | Mean | cv |
| --- | --- | --- |
| `generate_shapes` | 46.9 ms | 0.58% |
| `filter_by_viewport_and_size` | 4.50 ms | 1.31% |
| `resolve_view_layers` | 1.89 ms | 0.56% |
| `filter_by_layer_visibility` | 0.825 ms | 1.01% |
| `run()` (all 4 stages) | 55.7 ms | 0.39% |

`resolve_view_layers` as its own stage *after* the viewport/size filter
pays the layer-lookup cost on only the ~25% of shapes that survive
culling — an earlier design resolving during `generate_shapes` measured
~46% slower. `generate_shapes` is ~84% of `run()` and the natural
optimization target if pipeline throughput ever needs to improve.

## 2026-08-03 — PipelineCache: cascading per-stage caching (later merged into Pipeline, see below)

Added a `PipelineCache` wrapping the four stages with cascading
last-seen-input comparison. v1 chained `resolve_view_layers`'s validity to
the viewport filter's, so it re-ran on every pan/zoom despite depending
only on `AbstractId`; v2 decoupled it (key: `AbstractId` alone), trading
+42ms cold-start (`resolve_view_layers` now runs on the full 1M shapes
instead of the ~250K viewport-filtered survivors) for -1.7ms on every
subsequent pan/zoom frame:

| Scenario | v1 | v2 |
| --- | --- | --- |
| No change (steady state) | ~0 ms | ~0 ms |
| Pan-only | 7.83 ms | 6.11 ms |
| Visibility-only | 0.579 ms | 0.614 ms |
| Cold start (fresh, one `run()`) | 55.7 ms (uncached baseline) | 97.7 ms |

Investigated why `resolve_view_layers` on the full 1M set (40.3ms) is far
more than 4x the 250K-subset cost (1.88ms) despite both lookups being
O(1) — not algorithmic; best explanation is that materializing ~1M `Shape`
copies (one heap allocation each) hits cache/TLB/allocator effects
disproportionately, the same reason `generate_shapes` itself costs
~47-50ms. Kept the v2 design: pan/zoom/select dominates real usage, so the
one-time cold-start cost is worth the per-frame win. This copy-volume
finding directly motivated merging the two stages, below.

## 2026-08-03 — PipelineCache merged into Pipeline; resolve_view_layers merged into generate_shapes

Two structural changes landed close together:

- `PipelineCache` (a separate class, cascading invalidation via
  manually-set boolean flags) was hard to read. Replaced: `Pipeline`'s
  stage methods became non-static instance methods, each owning a small
  `CachedStage<Key, Value>` member (remembers the last key/result,
  recomputes only when the key changes) and chaining to the previous
  stage internally. Pure structural change — confirmed via the full
  benchmark suite, all deltas within normal noise.
- Once merged, `generate_shapes` and `resolve_view_layers` turned out to
  always share the same cache key (`AbstractId`) and always recompute
  together — the reason they were originally split (resolving during
  generation measured ~46% slower) no longer applied, since that finding
  was for a design where viewport-filtering ran *between* them; the
  current design always resolves on the full set regardless. Merged:
  `generate_shapes` now resolves each shape's `ViewLayerId` inline;
  `TaggedShape` is gone; pipeline is down to 3 stages.

| Benchmark | Before (PipelineCache v2) | After both merges |
| --- | --- | --- |
| `BM_Run` (cold start) | 97.7 ms | **58.9 ms** |
| `generate_shapes` alone | ~52.6ms + ~40.3ms (two stages) ≈ 92.9 ms | **52.6 ms** |
| Reused, pan-only | 6.11 ms | 5.91 ms |
| Reused, visibility-only | 0.614 ms | 0.587 ms |

Cold-start dropped ~39%, and the merged `generate_shapes` costs far less
than the sum of the two stages it replaced — consistent with eliminating
one full 1M-`Shape` copy pass. Warm path unchanged, as expected.

## 2026-08-03 — render module: dbu→pixel transform + SkPicture generation

New `src/render/render.hpp`: `Renderer` adds two more `CachedStage`-backed
stages on top of `Pipeline`'s output — `transform_to_pixels` and
`build_picture` (Skia `SkPictureRecorder` draw calls).

| Benchmark | Mean |
| --- | --- |
| `BM_TransformToPixels` (isolated, ~250K shapes) | 0.715 ms |
| `BM_BuildPicture` (isolated, ~250K shapes) | 1.43 ms |
| `BM_Render` (pipeline + render, cold) | 62.0 ms (+2.5ms over pipeline-only) |
| `BM_RenderReused_PanOnly` (warm) | 8.56 ms (+2.75ms over pipeline-only) |
| `BM_RenderReused_NoChange` | ~0 ms |

**Threading decision, backed by these numbers**: warm/interactive path is
8.56ms at 1M shapes, comfortably under a 60fps/16.6ms budget — no case for
a background thread yet. Single-threaded stays the right choice until a
future benchmark (larger data, real text rendering, actual rasterization)
shows otherwise.

## 2026-08-03 — Pipeline groups by ViewLayerId (bottom-up draw order) + Terminal text labels

- `filter_by_layer_visibility`/`run()` now return `std::map<ViewLayerId,
  vector<RenderedShape>>` instead of a flat vector — checks visibility
  once per distinct ViewLayerId, and map iteration order gives correct
  bottom-up draw order for free (`ViewLayerId`'s ordering, via `Id<Tag>`'s
  new `operator<=>` added to `cmg`'s template, matches LEF-declared layer
  stacking order). `build_picture` follows suit, building each ViewLayer's
  `SkPaint` once per group instead of once per shape.
- `generate_shapes` attaches one text label per Terminal (via
  `Geometry::get_label_location`), riding on a real geometric Shape so it
  survives the viewport/size filter's bbox check.

| Benchmark | Before | After |
| --- | --- | --- |
| `BM_GenerateShapes` (1M shapes) | 52.6 ms | 101 ms |
| `BM_FilterByLayerVisibility` | 0.82 ms | 2.28 ms |
| `BM_Run` (cold) | 58.9 ms | 108 ms |
| `BM_RunReused_PanOnly` | 5.91 ms | 8.47 ms |
| `BM_RenderReused_PanOnly` (full chain) | 8.56 ms | 12.3 ms |

The `generate_shapes` regression is ~36ms of `Geometry::get_label_location`
cost (confirmed via an isolated 100K-call micro-benchmark, 356ns/call) and
the `filter_by_layer_visibility` regression is `std::map`'s per-shape tree
insert vs. a flat vector scan — both understood, not mysteries. One real
inefficiency was caught and fixed along the way (a needless per-Terminal
`std::vector<Shape>` copy), recovering ~5ms. Still comfortably under a
60fps budget (12.3ms/frame warm), so the ~2x cold-start regression wasn't
chased further.

## 2026-08-03 — One label per distinct layer, not one per Terminal

A Terminal's Shapes can span multiple physical layers (e.g. M1 and M2),
each needing its own label tied to its own `ViewLayerId`. `generate_shapes`
now buckets a Terminal's Shapes by `layer_name` instead of one combined
label per Terminal; single-layer Terminals (the common case) still get
exactly one label.

| Benchmark | Before | After |
| --- | --- | --- |
| `BM_GenerateShapes` (1M shapes) | 102 ms | 106 ms |
| `BM_Run` (cold) | 110 ms | 112-113 ms |

~4ms from one `unordered_map` construction per Terminal, even single-layer
ones. Not pursued further: small relative to the already-accepted
label-placement cost, and the warm/interactive path is unaffected
(`generate_shapes` is cached per-`AbstractId`, doesn't rerun on pan/zoom).

## 2026-08-04 — Merge overlapping rects/polygons within a Shape

Found visually (`render_preview` against `Nangate45_stdcell.lef`): a
Terminal Port's or Obstruction's Shape can contain several rects that
overlap *each other* (e.g. an L/T-shaped pin), and drawing each with a
translucent fill double-painted the overlap into a visibly darker patch.
`Geometry::merge_overlapping_fills(Shape&)` unions a Shape's own
rects+polygons into a minimal non-overlapping set; paths are left
untouched. Scoped to one Shape at a time.

No measurable regression on the 1M-shape stress benchmark (every shape
there has exactly one geometry item, so the merge's fast path is a no-op).
Isolated micro-benchmark (N half-overlapping rects) to measure the actual
union cost, since the stress data can't:

| N rects | Copy alone | Copy + merge |
| --- | --- | --- |
| 2 | 0.016 us | 3.48 us |
| 5 | 0.017 us | 13.6 us |
| 10 | 0.024 us | 30.1 us |
| 50 | 0.033 us | 176 us |

Grows slightly worse than linearly (each iterative union operates on a
result that's grown from previous ones). Real-world check: the full
`Nangate45_stdcell.lef` library (135 designs) went from 11.1ms to 30.5ms
total `generate_shapes` time combined (worst single design under 1ms) —
real cells do have some overlapping pin geometry, but not much per shape.
The dense `fakeram45_1024x32` SRAM macro was unaffected (~0.6ms either
way) — its density comes from many separate adjacent shapes, not
overlapping rects within one shape. `generate_shapes` is cached
per-`AbstractId`, so this cost is paid once per Abstract switch, not per
frame.

## 2026-08-04 — Renderer::rasterize(): SkPicture -> raw RGBA8888 PixelBuffer

Added the pixel-buffer rasterization step: `Renderer::rasterize()` turns
`build_picture`'s `SkPicture` into a `PixelBuffer` (pointer + width +
height + row_bytes), a third `CachedStage`-backed stage. Two decisions:

- **Explicit `kRGBA_8888_SkColorType`**, not `SkImageInfo::MakeN32Premul`'s
  platform-native `kN32_SkColorType` (BGRA on some platforms, RGBA on
  others) — guarantees identical bytes between the macOS dev machine and
  the Linux target.
- **Y-axis flip applied once per frame as a canvas transform** in
  `rasterize()` (not in `transform_to_pixels`, which stays unflipped) —
  dbu-space y increases upward, Skia's canvas is y-down.

**Real bug found via visual testing, not the unit tests**: a whole-canvas
flip also mirrors glyph rendering — Terminal labels came out
upside-down/mirrored ("VSS" as "SSV") in `render_preview` output. None of
the existing text tests caught this since they only scan for *any* opaque
pixel, not a legible glyph. Fixed with a local counter-flip
(`save`/`translate`/`scale(1,-1)`/`drawString`/`restore`) around each
label's own anchor in `build_picture`, which couples that `SkPicture` to
being drawn through `rasterize()`'s specific flip for text to render
upright (documented in both methods). A reminder that pixel-level
assertions don't catch every rendering bug — occasionally looking at the
actual image still matters.

| Benchmark | Result |
| --- | --- |
| `BM_Rasterize` (isolated) | 6.21 ms |
| `BM_Render` (cold, full chain) | 117 ms |
| `BM_RenderReused_NoChange` | ~0 ms |
| `BM_RenderReused_PanOnly` (warm, full chain) | **21.5 ms** |

**Headline finding**: the warm/interactive pan-only path went from 12.5ms
to 21.5ms (confirmed via in-session A/B, not just a cross-session
comparison) — the first time this project has *exceeded* a 60fps/16.6ms
budget rather than stayed under it. Investigated one hypothesis (per-frame
raster-surface allocation) and ruled it out (isolated measurement: 0.25ms,
too small to explain the gap); more likely explanation is that the
isolated benchmark and the reused one aren't quite apples-to-apples (fixed
vs. varying pan value → different surviving shape counts each iteration).
Not chased further — flagged prominently here and in README's reopened
Threading question instead.
