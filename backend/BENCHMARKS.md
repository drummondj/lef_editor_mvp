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
