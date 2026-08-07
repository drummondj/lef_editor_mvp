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

## 2026-08-07 — Pipeline::hit_test_point (UPDATES.md 7.1 mouse hover)

Unlike every stage above, `hit_test_point` isn't `CachedStage`-backed —
it's called fresh on *every* pointer-move event (`le_set_mouse_position`
in api.cpp), against the already viewport-culled/visibility-filtered
shape set `Pipeline::run` produces, not the full 1M-shape design. The
concern this benchmark was written to settle: is bounding the candidate
set to on-screen shapes alone enough, or does per-candidate geometry cost
also need optimizing?

`BM_HitTestPoint` measures a worst-case miss (scans every visible
candidate without ever finding a hit) at the center of the stress scene's
visible viewport (`make_scene`'s own `[0, 100,000,000)` dbu range on each
axis, roughly a quarter of the design's ~250K on-screen candidates after
viewport culling and the M2-layer-hidden visibility filter).

| Version | `BM_HitTestPoint` |
| --- | --- |
| Naive `Geometry::contains` (no bbox pre-check) | 16.9 ms |
| With a cheap bbox pre-check before `bg::within`/`path_to_polygons` | **60.0 µs** |

Bounding the candidate set to on-screen shapes alone was **not** enough -
16.9ms/call would cap interactive hover responsiveness well below 60fps,
confirmed by direct comparison against `BM_RunReused_PanOnly`'s 9.76ms
(the cost of re-filtering the *entire* shape set on every viewport
change) elsewhere in this file: hit-testing on every mouse pixel move was
costing *more* than a full pipeline re-filter that only happens on
pan/zoom. Root cause: `Geometry::contains` called `path_to_polygons`
(a real Boost.Geometry buffer operation) on every visible path-shaped
candidate regardless of whether the query point was anywhere near it -
about a third of the stress design's shapes are paths.

Fix: `Geometry::contains` now checks each polygon/path's own bbox first
(a handful of integer comparisons, reusing the existing private
`bbox_of` helper) and only falls through to `bg::within`/
`path_to_polygons` if that cheap check passes - rects were already this
cheap. 280x faster, comfortably under the frame budget, so the naive
linear-scan design (bounded to on-screen shapes) stands without needing
a spatial index - confirming the plan's "benchmark before optimizing
further" approach rather than building an R-tree speculatively.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_COVERAGE=OFF
cmake --build build --target pipeline_benchmarks
./build/pipeline_benchmarks --benchmark_filter=BM_HitTestPoint --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
```

## 2026-08-07 — compose_with_overlays: selection-overlay picture replay scaled with selection size

Reported symptom: after a piece-level selection fix (`Pipeline::hit_test_rect`/
`draw_selected_piece_outline` now trace a selected PATH piece's *buffered*
outline via `Geometry::path_to_polygons`, not a cheap centerline halo),
interactive mouse-move/zoom/multi-select got progressively slower the more
objects were selected. First fix attempt split `build_overlay_picture`
(mouse-driven chrome: drag rect/cursor/hover) from a new
`build_selection_overlay_picture` (selected-piece outlines), so a pure
mouse move no longer *re-records* the selection outline SkPicture. That
fix alone didn't resolve the report - added `BM_ComposeWithOverlays_
ManySelectedPieces_MouseMoveOnly` (selects N real pieces from the stress
design's own filtered output, mixed RECT/POLYGON/PATH, then measures
`compose_with_overlays` cost with only mouse position changing) to find
out why, instead of continuing to reason about it from code alone:

| Selected pieces | Before raster fix | After raster fix |
| --- | --- | --- |
| 0 | 1.37 ms | 2.29 ms |
| 100 | 1.59 ms | 2.28 ms |
| 1,000 | 1.66 ms | 2.32 ms |
| 5,000 | 2.56 ms | 2.30 ms |
| 20,000 | 6.28 ms | 2.33 ms |

Root cause: `compose_with_overlays` *replayed* `selection_overlay_picture`
(`canvas->drawPicture`) on every call, regardless of whether the picture
had actually been re-recorded. SkPicture replay cost, like recording
cost, scales with the number of recorded draw ops - so even with
recording fixed, every mouse move still cost proportionally to selection
size just from re-executing thousands of already-recorded stroke/path
draw calls. Fix: added `rasterize_selection_overlay_frame`, mirroring the
existing `rasterize_frame` (design picture) pattern - the selection
overlay is now rasterized into its own cached raster image (keyed on
`{viewport_version, visibility_version, selection_version}`, no
`mouse_version`) and `compose_with_overlays` blits that image
(`SkCanvas::drawImage`, cost independent of content complexity) instead
of replaying the picture. After the fix, cost is flat regardless of
selection size (~2.3ms, matching the always-present two-image-blit
baseline) - the ~0.9ms baseline increase from 1.37ms to 2.29ms at zero
selected pieces is the fixed cost of the second full-viewport blit,
always paid now instead of a near-free empty `drawPicture` call.

This is the second time this session a "the picture-cache call count
looks right" test passed while the actual regression (replay cost, not
recompute cost) remained - a reminder that `CachedStage` call-count
assertions only prove *recomputation* was avoided, not that *replay* of
whatever's cached is cheap; when what's cached is an `SkPicture` rather
than a flat buffer, replay cost still needs its own reasoning (or
benchmark) before assuming a cache hit means "cheap."

## 2026-08-07 — Scene::select(): O(N²) dedup scan, not the overlay/composite cost above

The `compose_with_overlays` fix directly above did not resolve the user's
reported symptom ("many seconds of delay" moving the mouse after
selecting many shapes) - a clear sign the real cost was somewhere neither
of the two overlay fixes had touched, since both were millisecond-scale
and the report was multi-second. Asked to actually profile rather than
keep guessing: a benchmark exercising the real repro end-to-end
(`le_read_lef` the stress LEF, `le_fit_scene`, then a full-viewport
`le_mouse_down`/`le_mouse_up` drag-select, added as
`BM_RefreshSelectedObjects_ManySelectedPieces`) ran for **7+ minutes**
without completing what should be a sub-second setup step - had to be
killed. Root cause: `Scene::select()`'s dedup (`src/scene/scene.hpp`)
scanned the *entire current selection*, doing a full structural
rect/polygon/path comparison (`same_piece`) against every existing entry
sharing the candidate's origin, on every single insert. The stress-test
LEF's ~900,000 obstruction shapes all share **one** `ObstructionId`
origin (one `OBS` block), which is exactly the worst case for this: every
`select()` call during a big drag-select scans against *every previously
selected piece*, since they all share that one origin and the existing
origin-mismatch short-circuit never fires. `le_mouse_up`'s drag branch
(`src/api/api.cpp`) calls `scene.select()` once per enclosed piece, so a
drag enclosing M pieces cost O(M²) `same_piece` calls - and ran inside the
handle-wide mutex, blocking everything else for however long it took.

Fix: replaced the linear scan with a signature-bucketed index
(`piece_signature()` - a hash consistent with `same_piece`'s existing
equality, `selection_index_` - `std::unordered_multimap<size_t /*sig*/,
size_t /*index*/>`), so dedup only compares against the small bucket of
pieces that could plausibly match, not the whole selection. Isolated
benchmark (`BM_SceneSelect_ManyPiecesSameOrigin` - N distinct pieces
sharing one synthetic origin, timing only the `select()` loop, not LEF
parsing or `hit_test_rect`):

| N pieces | Before (O(N²)) | After (O(N) average) |
| --- | --- | --- |
| 1,000 | 2.38 ms | 0.073 ms |
| 5,000 | 58.5 ms | 0.413 ms |
| 20,000 | 930 ms | 1.71 ms |

544x faster at 20,000 pieces, and the "before" column's growth
(~24.6x time for 5x items, ~15.9x time for 4x items) confirms the
quadratic behavior directly, not just asserted from reading the code.
`items_per_second` after the fix stays flat (~12-13M/s) across all three
sizes, confirming O(1) average per call, not just "faster."

A second, independent, additive cause was found the same investigation:
the Flutter frontend (`frontend/lib/providers/le_provider.dart`)
unconditionally rebuilds its entire selected-object list - several FFI
calls per selected object - on *every* mouse-move event, regardless of
whether the selection changed. Fixed by exposing `Scene::selection_version()`
through the C API (`le_selection_version`) so the frontend can skip that
rebuild when nothing selection-related has actually changed since the
last check - not benchmarked in isolation the same way (Dart/FFI-side,
not something GoogleBenchmark measures), verified instead by rebuilding
the app and reproducing the original repro.

(A separate incremental-refresh design was attempted on top of this - a
`le_selected_object_signature` per-entry comparison token letting the
frontend diff old vs. new selection instead of always rebuilding fully -
but was reverted: it added real complexity across three layers (C API,
FFI bindings, Dart) without addressing the actual remaining bottleneck,
which turned out to be entirely different - see below.)

## 2026-08-07 — build_picture's dead whole-object selection-outline pass

Even after both fixes above, the reported symptom persisted: selecting
more than a handful of objects was slow, and stayed slow adding just one
more object to an already-large selection - not explained by either
prior fix (`Scene::select()` was already confirmed O(1) average; the
Dart-side refresh gate only helps *unchanged* frames, not actual
selection changes). Asked directly to profile rather than keep
hypothesizing, and to justify prior claims about where time was going
(none of which had been profiler-verified) - used three independent
methods, escalating in rigor:

1. **Isolated GoogleBenchmark**, `BM_BuildPicture_WithLargeSelection` (a
   selection of N pieces populated once, then `build_picture` called
   repeatedly): 3.95 ms (N=0) -> 12.9 ms (N=1,000) -> 64.5 ms (N=5,000) ->
   64.3 ms (N=20,000, plateaus - ran out of distinct candidates to
   select). ~16x slower at 5,000 selected than 0.
2. **macOS `sample`** (statistical call-stack sampler, no source changes)
   attached to a Release build running that same benchmark: of 6,245
   samples inside `build_picture`'s recompute call, 5,630 (90%) landed
   directly in the lambda's own *inlined* code, vs. only ~321 total for
   the separately-visible `draw_group` (the actual per-shape drawing
   work) - and 272 samples specifically in `std::variant`'s equality-
   dispatch machinery, the one piece of evidence that survived inlining
   and pointed concretely at `SelectionRef == SelectionRef` comparisons.
3. **Same benchmark, Debug build** (no inlining, so every function keeps
   its own call-tree frame) - removed all ambiguity: `Scene::
   is_selected_as_whole_object` (`scene.hpp:552`) accounted for 1,056
   samples in one call-tree branch alone, ~30x more than `draw_group`
   (~35) and ~100x more than `Scene::select` (~9) in the same branch -
   with `find_if.h:24-25` (the linear scan inside it) and
   `scene.hpp:554` (the comparison lambda) as its own direct children.

Root cause: `build_picture`'s whole-object selection-outline pass (drawn
once per *visible* `PixelShape`, gated on `Scene::is_selected_as_whole_object`
- itself a linear scan of the *entire current selection*) was O(visible
shapes * selection size), and reran on every selection change since
`selection_version()` was part of `build_picture`'s cache key. It was
also provably dead: `api.cpp`'s `le_mouse_up` only ever calls
`Scene::select()` with a piece (confirmed by grep, both call sites), so
`is_selected_as_whole_object` could never return true through the public
API - this was pure wasted cost with no possible visual effect.

Fix: removed the whole-object pass, `Scene::is_selected_as_whole_object`,
and `draw_selection_outline` entirely (all otherwise unused). Since
`build_picture` no longer has any selection-dependent content at all,
also dropped `selection_version()` from its cache key and
`rasterize_frame`'s (kept in sync by hand, per their documented
invariant) - a selection change no longer invalidates either.

| N pieces selected | Before | After |
| --- | --- | --- |
| 0 | 3.95 ms | 4.02 ms |
| 1,000 | 12.9 ms | 4.02 ms |
| 5,000 | 64.5 ms | 3.99 ms |
| 20,000 | 64.3 ms | 4.00 ms |

Flat regardless of selection size. Re-profiled with `sample` after the
fix as a final check, not just re-benchmarked: neither `build_picture`'s
lambda nor any selection/variant-comparison symbol appears anywhere in
the post-fix profile at all - the hot functions are now legitimate
per-shape Skia drawing work (`SkPaint`, `SkPathData`, `SkMatrix`) and
one-time LEF parsing setup, exactly what should be there.

## 2026-08-07 — cmg-generated to_string/to_properties/operator<< took their struct by value

Reported: the slowdown was still reproducible selecting Obstruction
pieces specifically, never Terminal pieces - a real clue, not a red
herring. The user's own hypothesis (too many shapes embedded in *one*
Obstruction, not too many selected pieces) was correct. Root cause:
`cmg` (the schema code generator, `/Users/john/Projects/synthosilicon/cmg`)
generates `to_string`/`to_properties`/`operator<<` for every schema
class taking the struct *by value*
(`cmg/templates/indexed_pools/struct_hpp_j2.py`). Invisible for small
classes, but `ObstructionData::shapes` (`std::vector<Shape>`) is an
*embedded* struct field, not pool-referenced like `TerminalData`'s ports
(fetched separately via `Root::get_terminal_ports`) - so every call to
`le::to_properties(*obstruction)` (`api.cpp`'s
`build_selected_object_properties`, called on every property fetch for
any selected Obstruction piece) deep-copied the whole `shapes` vector
just to read a couple of fields.

Isolated benchmark (`BM_ToPropertiesObstructionCopyCost`) against the
real stress-design Obstruction (900K shapes, all under one
`ObstructionId` - the same one this whole investigation has used):

| | Before | After |
| --- | --- | --- |
| `to_properties(ObstructionData)` per call | 29.1 ms | sub-microsecond (50M+ iterations in the benchmark's own min-time window) |

Fixed at the generator (not by hand-patching generated/, which must
never be hand-edited): changed the three signatures in `cmg`'s template
to `const T&`, none of the three bodies mutate their own copy. Fixes
*every* schema class generically, not just Obstruction - any future
class with a large embedded field gets this for free. Regenerated via
the local `cmg` checkout (`poetry run cmg --schema ... --output
src/database/generated --export-style INDEXED_POOLS`, per the
`regen-database` skill) - `src/database/generated/` is gitignored build
output, not committed, so there's no diff to review here beyond
rebuilding/retesting.

`BM_RefreshSelectedObjects_ManySelectedPieces` - added earlier this
session to measure the full `le_selected_object_*` FFI call sequence,
but unable to complete even once until now (killed once at O(N²)
`Scene::select`, silently paying this same 29ms-per-call cost on every
run after that was fixed) - finally completed cleanly: 36.4 ms at
50,996 selected pieces, 88.5 ms at 122,333 - real remaining cost now
proportional to *actual FFI call count* (kind + properties per selected
object), not a hidden per-call landmine.

## 2026-08-08 — PATH rendering: buffered outline + pattern fill + centerline

Reported: unselected PATH shapes on both Terminals and Obstructions
rendered as solid-colored blocks with no visible fill pattern, unlike
RECT/POLYGON on the same layer. Confirmed visually via `render_preview`
against a small hand-written LEF (PIN/OBS PATH shapes on M1) before
touching any code. Root cause: `draw_group`'s PATH branch drew an
outline-colored "border" stroke at `path.width + 2*kPathOutlineMarginPx`
directly *underneath* a narrower pattern-shaded "wire" stroke - since a
layer's `outline_color` is also `pattern_shader`'s own tile color, that
border acted as an opaque same-color backing plate showing straight
through every transparent gap in the pattern.

Fix: `RenderedShape` (`pipeline.hpp`) gains `path_outlines` - each
path's buffered outline (`Geometry::path_to_polygons`, flat ends, miter
joins - the same buffering already used for hover/selected-piece
outlines), computed once at `generate_shapes` time. `PixelPath`
(`render.hpp`) carries the transformed result as `buffered_outline`.
`draw_group` now fills/outlines a PATH's `buffered_outline` exactly like
a real POLYGON (pattern fill, thin boundary), plus a new centerline
stroke along the path's own original polygon so it still reads as a
wire. Verified visually (border + BRICK pattern + a clearly visible
centerline, confirmed by rendering a large single path in isolation) and
via new pixel-level regression tests.

**Performance, measured, not assumed** - `Geometry::path_to_polygons`
costs ~768ns/call in isolation (`BM_PathToPolygonsSingleCall`), so
computing it once per path at `generate_shapes` time (cached per-
AbstractId, not per-frame) rather than in `transform_to_pixels` (reruns
every pan/zoom) was the deliberate design choice - confirmed by checking
each stage's own cache key before choosing where this goes, not assumed.

Two real regressions were found via benchmarking that the design above
didn't anticipate, both against a clean "before this session's PATH
work" baseline (`git stash` on `pipeline.hpp`/`render.hpp` only, same
machine/session state, `--benchmark_min_time=8-10s` for a stable
reading - the first attempt at this comparison used the default ~1s
min-time and only 3-8 iterations, which gave wildly unstable numbers
ranging 43-173ms for the same benchmark on the same code; only the
long-running, ~1700+-iteration numbers below should be trusted):

| Benchmark | Before | After |
| --- | --- | --- |
| `BM_GenerateShapes` (1M shapes, cold) | 137 ms | 434 ms |
| `BM_RunReused_PanOnly` (pipeline only, warm) | 10.3 ms | 11.7 ms |
| `BM_RenderReused_PanOnly` (full chain, warm) | 39.0 ms | 52.1 ms |

`BM_GenerateShapes`'s +297ms is the expected, accepted cost from point
above (~330K paths × ~768ns + overhead) - paid once per Abstract-load,
within this project's documented cold-start budget (see
`pipeline_latency_budget` memory: 1-2s is fine, spinner shown).
`BM_RunReused_PanOnly`'s +1.4ms is small and was *not* the concerning
number it first appeared to be: `RenderedShape::path_outlines` was
originally a plain `std::vector<std::vector<Polygon>>`, and
`filter_by_viewport_and_size`/`filter_by_layer_visibility` both already
copy every surviving `RenderedShape` wholesale on every pan/zoom (an
existing pattern, not new) - that plain vector made each such copy
meaningfully heavier, wrapped in `std::shared_ptr<const ...>` instead so
copying a `RenderedShape` costs an O(1) refcount bump regardless of how
much outline geometry a path-heavy `Shape` holds.

`BM_RenderReused_PanOnly`'s +13ms is real and not fully explained by the
pipeline-only number - the difference is in `Renderer`'s own stages
(`transform_to_pixels` now also transforms `buffered_outline` polygons;
`draw_group` now issues a shader-filled polygon draw plus a boundary
stroke plus a centerline stroke per path, instead of two plain strokes).
Not investigated further: this project's render pipeline was *already*
well over its 60fps/16.6ms interactive-frame budget before this change
(39.0ms warm-pan, flagged as an open question in README's Threading
section since 2026-08-04 - see the `Renderer::rasterize()` entry above),
so this doesn't cross a new qualitative threshold, and the underlying
open question (single-threaded rendering) is the same one that would
need solving regardless of this specific fix.
