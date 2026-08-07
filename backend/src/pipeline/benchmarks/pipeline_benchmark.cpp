#include "../../api/api.hpp"
#include "../../render/render.hpp"
#include "../pipeline.hpp"
#include "stress_data.hpp"
#include <benchmark/benchmark.h>

using namespace le;

// Pipeline now caches internally per-instance (see pipeline.hpp), so
// measuring a single stage's *uncached* per-call cost requires a fresh
// Pipeline every iteration - otherwise iterations after the first would be
// cache hits and the benchmark would measure ~nothing instead of real work.

// Now includes the ViewLayerId resolution that used to be a separate
// resolve_view_layers stage - see pipeline.hpp's class comment for why
// they were merged.
static void BM_GenerateShapes(benchmark::State &state)
{
    const auto &data = stress_data();
    for (auto _ : state)
    {
        Pipeline pipeline;
        const auto &shapes = pipeline.generate_shapes(data.root, data.abstract_id, data.view_layers);
        const auto *shapes_data = shapes.data();
        benchmark::DoNotOptimize(shapes_data);
    }
    state.SetItemsProcessed(state.iterations() * kTotalShapes);
}
BENCHMARK(BM_GenerateShapes)->Unit(benchmark::kMillisecond);

// Benchmarked on the full 1M-shape generated (and resolved) set, matching
// how Pipeline::run() actually calls it.
static void BM_FilterByViewportAndSize(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_scene(data);

    Pipeline setup;
    const auto &generated = setup.generate_shapes(data.root, data.abstract_id, data.view_layers);

    for (auto _ : state)
    {
        Pipeline pipeline;
        const auto &filtered = pipeline.filter_by_viewport_and_size(generated, scene, data.view_layers);
        const auto *filtered_data = filtered.data();
        benchmark::DoNotOptimize(filtered_data);
    }
    state.SetItemsProcessed(state.iterations() * generated.size());
}
BENCHMARK(BM_FilterByViewportAndSize)->Unit(benchmark::kMillisecond);

static void BM_FilterByLayerVisibility(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_scene(data);

    Pipeline setup;
    const auto &generated = setup.generate_shapes(data.root, data.abstract_id, data.view_layers);
    const auto &viewport_filtered = setup.filter_by_viewport_and_size(generated, scene, data.view_layers);

    for (auto _ : state)
    {
        Pipeline pipeline;
        const auto &filtered = pipeline.filter_by_layer_visibility(viewport_filtered, scene, data.view_layers);
        const auto *filtered_ptr = &filtered;
        benchmark::DoNotOptimize(filtered_ptr);
    }
    state.SetItemsProcessed(state.iterations() * viewport_filtered.size());
}
BENCHMARK(BM_FilterByLayerVisibility)->Unit(benchmark::kMillisecond);

// A fresh Pipeline every iteration, one run() call each - the "just
// switched to a different Abstract" cold-start case, where every stage is
// a cache miss.
static void BM_Run(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_scene(data);

    for (auto _ : state)
    {
        Pipeline pipeline;
        const auto &result = pipeline.run(data.root, scene, data.view_layers);
        const auto *result_ptr = &result;
        benchmark::DoNotOptimize(result_ptr);
    }
    state.SetItemsProcessed(state.iterations() * kTotalShapes);
}
BENCHMARK(BM_Run)->Unit(benchmark::kMillisecond);

// The interactive/reused-instance case: one Pipeline constructed once and
// reused across iterations (as a real caller would keep one alive for a
// Scene's whole interactive lifetime), compared against BM_Run's cold-start
// baseline above to measure the actual caching benefit, per BENCHMARKS.md.

// Nothing changes between calls - the steady-state "no input this frame"
// case. Expect near-zero: every stage hits its cache.
static void BM_RunReused_NoChange(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_scene(data);
    Pipeline pipeline;

    for (auto _ : state)
    {
        const auto &result = pipeline.run(data.root, scene, data.view_layers);
        const auto *result_ptr = &result;
        benchmark::DoNotOptimize(result_ptr);
    }
    state.SetItemsProcessed(state.iterations() * kTotalShapes);
}
BENCHMARK(BM_RunReused_NoChange)->Unit(benchmark::kMillisecond);

// Only pan changes each call, simulating interactive panning - the common
// case generate_shapes's AbstractId-keyed cache is meant for. Expect close
// to the uncached viewport-filter and layer-filter costs, not the full
// BM_Run cost, since generate_shapes is skipped every iteration.
static void BM_RunReused_PanOnly(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_scene(data);
    Pipeline pipeline;

    int64_t pan_x = 0;
    for (auto _ : state)
    {
        scene.set_pan(Point{pan_x++, 0});
        const auto &result = pipeline.run(data.root, scene, data.view_layers);
        const auto *result_ptr = &result;
        benchmark::DoNotOptimize(result_ptr);
    }
    state.SetItemsProcessed(state.iterations() * kTotalShapes);
}
BENCHMARK(BM_RunReused_PanOnly)->Unit(benchmark::kMillisecond);

// Only a layer's visibility changes each call, simulating toggling a layer
// on/off in the UI. Expect close to just the uncached layer-filter stage
// cost, since generate_shapes and the viewport filter are both still
// cached.
static void BM_RunReused_VisibilityOnly(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_scene(data);
    Pipeline pipeline;

    bool visible = true;
    for (auto _ : state)
    {
        scene.set_layer_name_visible("M1", visible);
        visible = !visible;
        const auto &result = pipeline.run(data.root, scene, data.view_layers);
        const auto *result_ptr = &result;
        benchmark::DoNotOptimize(result_ptr);
    }
    state.SetItemsProcessed(state.iterations() * kTotalShapes);
}
BENCHMARK(BM_RunReused_VisibilityOnly)->Unit(benchmark::kMillisecond);

// UPDATES.md 7.1's mouse-hover feature calls Pipeline::hit_test_point on
// every pointer-move event (see le_set_mouse_position in api.cpp) - unlike
// the stages above, it's not CachedStage-backed (the query point changes
// every call), so its real per-call cost matters directly, not just its
// cold-vs-warm delta. Measures a worst-case miss (scans every visible
// shape without ever finding a hit, since a real hit could short-circuit
// early) at the center of make_scene's own visible viewport - the
// candidate set is already viewport-culled/visibility-filtered by `run`,
// not the full kTotalShapes design, which is the whole point of bounding
// hit-testing to on-screen shapes rather than a full design scan.
static void BM_HitTestPoint(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_scene(data);

    Pipeline setup;
    const auto &shapes = setup.run(data.root, scene, data.view_layers);

    const Point query{50'000'000, 50'000'000}; // center of make_scene's visible [0, 100,000,000) range

    for (auto _ : state)
    {
        const auto hit = Pipeline::hit_test_point(shapes, data.view_layers, scene, query);
        const auto *hit_ptr = &hit;
        benchmark::DoNotOptimize(hit_ptr);
    }
}
BENCHMARK(BM_HitTestPoint)->Unit(benchmark::kMicrosecond);

// render module benchmarks - Renderer also caches internally per-instance
// (see render.hpp), so isolated-stage benchmarks need a fresh Renderer per
// iteration too, same reasoning as the Pipeline benchmarks above.

static void BM_TransformToPixels(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_scene(data);

    Pipeline setup_pipeline;
    const auto &generated = setup_pipeline.run(data.root, scene, data.view_layers);

    for (auto _ : state)
    {
        Renderer renderer;
        const auto &pixel_shapes = renderer.transform_to_pixels(generated, scene);
        const auto *pixel_shapes_ptr = &pixel_shapes;
        benchmark::DoNotOptimize(pixel_shapes_ptr);
    }
    state.SetItemsProcessed(state.iterations() * generated.size());
}
BENCHMARK(BM_TransformToPixels)->Unit(benchmark::kMillisecond);

static void BM_BuildPicture(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_scene(data);

    Pipeline setup_pipeline;
    const auto &generated = setup_pipeline.run(data.root, scene, data.view_layers);
    Renderer setup_renderer;
    const auto &pixel_shapes = setup_renderer.transform_to_pixels(generated, scene);

    for (auto _ : state)
    {
        Renderer renderer;
        const auto &picture = renderer.build_picture(pixel_shapes, scene, data.view_layers, data.root);
        benchmark::DoNotOptimize(picture.get());
    }
    state.SetItemsProcessed(state.iterations() * pixel_shapes.size());
}
BENCHMARK(BM_BuildPicture)->Unit(benchmark::kMillisecond);

// Regression guard for a real, measured bug (see BENCHMARKS.md):
// build_picture used to have a whole-object selection-outline pass that
// called Scene::is_selected_as_whole_object once per *visible* PixelShape,
// itself a linear scan of the entire current selection - O(visible
// shapes * selection size), re-paid on every select() call (build_picture
// recomputed whenever selection_version() changed). That pass was also
// provably unreachable through the public API (api.cpp's le_mouse_up
// always calls Scene::select() with a piece - see Pipeline::hit_test_rect)
// so it was pure wasted cost. Removed entirely, along with
// Scene::is_selected_as_whole_object and build_picture's own dependency
// on selection_version. This benchmark now confirms build_picture stays
// flat-cost regardless of selection size, not just that it happens to be
// fast today.
static void BM_BuildPicture_WithLargeSelection(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_scene(data);

    Pipeline setup_pipeline;
    const auto &generated = setup_pipeline.run(data.root, scene, data.view_layers);
    Renderer setup_renderer;
    const auto &pixel_shapes = setup_renderer.transform_to_pixels(generated, scene);

    const int n = static_cast<int>(state.range(0));
    int selected = 0;
    for (const auto &[view_layer, group] : generated)
    {
        for (const auto &rs : group)
        {
            if (!rs.origin || selected >= n)
                continue;
            scene.select(*rs.origin, rs.shape); // always with a piece, matching le_mouse_up
            ++selected;
        }
    }

    for (auto _ : state)
    {
        Renderer renderer;
        const auto &picture = renderer.build_picture(pixel_shapes, scene, data.view_layers, data.root);
        benchmark::DoNotOptimize(picture.get());
    }
    state.SetItemsProcessed(state.iterations() * pixel_shapes.size());
}
BENCHMARK(BM_BuildPicture_WithLargeSelection)->Arg(0)->Arg(1000)->Arg(5000)->Arg(20000)->Unit(benchmark::kMillisecond);

static void BM_Rasterize(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_scene(data);

    Pipeline setup_pipeline;
    const auto &generated = setup_pipeline.run(data.root, scene, data.view_layers);
    Renderer setup_renderer;
    const auto &pixel_shapes = setup_renderer.transform_to_pixels(generated, scene);
    const auto &picture = setup_renderer.build_picture(pixel_shapes, scene, data.view_layers, data.root);

    for (auto _ : state)
    {
        Renderer renderer;
        const auto &buffer = renderer.rasterize(picture, scene);
        const uint8_t *buffer_data = buffer.data;
        benchmark::DoNotOptimize(buffer_data);
    }
    state.SetItemsProcessed(state.iterations() * pixel_shapes.size());
}
BENCHMARK(BM_Rasterize)->Unit(benchmark::kMillisecond);

// A fresh Pipeline + Renderer every iteration, running the full
// generate -> filter -> filter -> transform -> picture -> rasterize chain
// once each - the "just switched to a different Abstract" cold-start case,
// now including all three render stages.
static void BM_Render(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_scene(data);

    for (auto _ : state)
    {
        Pipeline pipeline;
        Renderer renderer;
        const auto &shapes = pipeline.run(data.root, scene, data.view_layers);
        const auto &pixel_shapes = renderer.transform_to_pixels(shapes, scene);
        const auto &picture = renderer.build_picture(pixel_shapes, scene, data.view_layers, data.root);
        const auto &buffer = renderer.rasterize(picture, scene);
        const uint8_t *buffer_data = buffer.data;
        benchmark::DoNotOptimize(buffer_data);
    }
    state.SetItemsProcessed(state.iterations() * kTotalShapes);
}
BENCHMARK(BM_Render)->Unit(benchmark::kMillisecond);

// Reused Pipeline + Renderer across iterations, mirroring the
// BM_RunReused_* scenarios above but through the full render chain -
// real numbers for the threading question (README's open design
// question): is Skia picture generation/rasterization actually a
// bottleneck on the interactive path, or does it stay cheap because it's
// caching-aware too?

static void BM_RenderReused_NoChange(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_scene(data);
    Pipeline pipeline;
    Renderer renderer;

    for (auto _ : state)
    {
        const auto &shapes = pipeline.run(data.root, scene, data.view_layers);
        const auto &pixel_shapes = renderer.transform_to_pixels(shapes, scene);
        const auto &picture = renderer.build_picture(pixel_shapes, scene, data.view_layers, data.root);
        const auto &buffer = renderer.rasterize(picture, scene);
        const uint8_t *buffer_data = buffer.data;
        benchmark::DoNotOptimize(buffer_data);
    }
    state.SetItemsProcessed(state.iterations() * kTotalShapes);
}
BENCHMARK(BM_RenderReused_NoChange)->Unit(benchmark::kMillisecond);

static void BM_RenderReused_PanOnly(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_scene(data);
    Pipeline pipeline;
    Renderer renderer;

    int64_t pan_x = 0;
    for (auto _ : state)
    {
        scene.set_pan(Point{pan_x++, 0});
        const auto &shapes = pipeline.run(data.root, scene, data.view_layers);
        const auto &pixel_shapes = renderer.transform_to_pixels(shapes, scene);
        const auto &picture = renderer.build_picture(pixel_shapes, scene, data.view_layers, data.root);
        const auto &buffer = renderer.rasterize(picture, scene);
        const uint8_t *buffer_data = buffer.data;
        benchmark::DoNotOptimize(buffer_data);
    }
    state.SetItemsProcessed(state.iterations() * kTotalShapes);
}
BENCHMARK(BM_RenderReused_PanOnly)->Unit(benchmark::kMillisecond);

// Isolated micro-benchmark for Geometry::merge_overlapping_fills's own
// cost. The 1M-shape stress data above can't measure this: every shape
// there has exactly one geometry item (a fresh "LAYER ;" before each one
// forces a new Shape - see stress_data.hpp), so the merge always takes its
// <=1-part no-op fast path. Real multi-rect-per-shape geometry - e.g. LEF's
// OBS ITERATE/DO/STEP array syntax, or a dense SRAM macro's fabric - is
// what actually exercises the union, so this benchmarks that directly
// instead of assuming it's cheap.
namespace
{
    // N unit-height rects spaced 5 dbu apart but 10 dbu wide, so
    // consecutive rects overlap by half - real merging work for
    // boost::geometry to do, not N already-disjoint parts unioned into N
    // separate output polygons.
    Shape overlapping_rects_shape(int n)
    {
        Shape shape{.layer_name = "M1"};
        shape.rects.reserve(n);
        for (int i = 0; i < n; ++i)
            shape.rects.push_back(Rect{.ll = {i * 5, 0}, .ur = {i * 5 + 10, 10}});
        return shape;
    }
}

// Baseline: the Shape copy alone (same copy Pipeline::generate_shapes
// already pays when pushing into its output vector, merge or not), so
// BM_MergeOverlappingFills's numbers below can be read as "on top of a
// cost already being paid" rather than all-new.
static void BM_ShapeCopyBaseline(benchmark::State &state)
{
    const Shape template_shape = overlapping_rects_shape(static_cast<int>(state.range(0)));
    for (auto _ : state)
    {
        Shape copy = template_shape;
        benchmark::DoNotOptimize(copy);
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_ShapeCopyBaseline)->Arg(2)->Arg(5)->Arg(10)->Arg(50)->Unit(benchmark::kMicrosecond);

static void BM_MergeOverlappingFills(benchmark::State &state)
{
    const Shape template_shape = overlapping_rects_shape(static_cast<int>(state.range(0)));
    for (auto _ : state)
    {
        Shape copy = template_shape;
        Geometry::merge_overlapping_fills(copy);
        benchmark::DoNotOptimize(copy);
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_MergeOverlappingFills)->Arg(2)->Arg(5)->Arg(10)->Arg(50)->Unit(benchmark::kMicrosecond);

// Overlay/composite benchmarks - added to actually locate a reported
// mouse-move/zoom/multi-select slowdown that scaled with selection size,
// rather than continuing to reason about it from code reading alone.
// Selects a batch of real pieces (mixed RECT/POLYGON/PATH, matching the
// stress generator's 1-in-3 PATH ratio, and drawn from the same giant
// single-Obstruction OBS block a real rectangle-drag-select over the
// stress LEF would produce) from the stress design's own filtered
// output, then measures the steady-state "mouse moves, selection doesn't
// change" cost through the actual render.hpp entry points a real
// interactive session calls (api.cpp's le_render_pixel_buffer).
namespace
{
    void select_pieces(le::Scene &scene, const std::map<le::ViewLayerId, std::vector<le::RenderedShape>> &shapes, int count)
    {
        int selected = 0;
        for (const auto &[view_layer, group] : shapes)
        {
            for (const auto &rs : group)
            {
                if (!rs.origin)
                    continue;
                scene.select(*rs.origin, rs.shape);
                if (++selected >= count)
                    return;
            }
        }
    }
}

static void BM_BuildSelectionOverlayPicture_ManySelectedPieces(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_scene(data);

    Pipeline setup_pipeline;
    const auto &shapes = setup_pipeline.run(data.root, scene, data.view_layers);
    select_pieces(scene, shapes, static_cast<int>(state.range(0)));

    for (auto _ : state)
    {
        Renderer renderer;
        const auto &picture = renderer.build_selection_overlay_picture(scene);
        benchmark::DoNotOptimize(picture.get());
    }
}
BENCHMARK(BM_BuildSelectionOverlayPicture_ManySelectedPieces)->Arg(100)->Arg(1000)->Arg(5000)->Arg(20000)->Unit(benchmark::kMillisecond);

// Steady state: selection is built once (one Renderer reused across
// iterations, matching how api.cpp keeps one Renderer per handle for a
// Scene's whole interactive lifetime, so the selection-overlay picture's
// own cache stays warm), then only mouse position changes every
// iteration - the exact scenario reported as slow.
static void BM_ComposeWithOverlays_ManySelectedPieces_MouseMoveOnly(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_scene(data);

    Pipeline pipeline;
    const auto &shapes = pipeline.run(data.root, scene, data.view_layers);
    select_pieces(scene, shapes, static_cast<int>(state.range(0)));

    Renderer renderer;
    const auto &pixel_shapes = renderer.transform_to_pixels(shapes, scene);
    const auto &design_picture = renderer.build_picture(pixel_shapes, scene, data.view_layers, data.root);

    int64_t x = 0;
    for (auto _ : state)
    {
        scene.set_mouse_position(x++ % 2000, 0);
        const auto &overlay_picture = renderer.build_overlay_picture(scene);
        const auto &selection_overlay_picture = renderer.build_selection_overlay_picture(scene);
        const auto &buffer = renderer.compose_with_overlays(design_picture, overlay_picture, selection_overlay_picture, scene);
        const uint8_t *buffer_data = buffer.data;
        benchmark::DoNotOptimize(buffer_data);
    }
}
BENCHMARK(BM_ComposeWithOverlays_ManySelectedPieces_MouseMoveOnly)->Arg(0)->Arg(100)->Arg(1000)->Arg(5000)->Arg(20000)->Unit(benchmark::kMillisecond);

// le_selected_object_* FFI-facing benchmark - added because the two
// Renderer-side fixes above (overlay/selection-picture split, then
// rasterizing the selection overlay instead of replaying it) did not
// resolve a reported mouse-move delay that scaled with selection size.
// The Flutter provider (frontend/lib/providers/le_provider.dart)
// unconditionally calls refreshSelectedObjects() on *every* pointer
// move/hover event (handlePointerEvent -> refreshAndNotify -> ...), which
// rebuilds the full selected-object property list from scratch every
// time: le_selected_object_kind + le_selected_object_property_count + one
// le_selected_object_property_at call per property, for *every currently
// selected object* - none of this is Renderer/Pipeline content, so it was
// invisible to every benchmark above. This measures the real C API call
// sequence Dart's selectedObjectProperties() makes, through the actual
// api.cpp (mutex-locked-per-call) surface, not a synthetic
// reconstruction - i.e. the true cost of one refreshSelectedObjects() call
// at a given selection size.
static void BM_RefreshSelectedObjects_ManySelectedPieces(benchmark::State &state)
{
    // Ensures the shared stress LEF file exists on disk (stress_data()
    // also builds an in-memory le::Root, unused here - le_read_lef needs
    // its own independent Root via the C API, matching how the real app
    // reads a file rather than sharing this file's own le::Root).
    stress_data();
    const std::string path = std::string(BENCHMARK_DATA_DIR) + "/stress.lef";

    LeHandle *handle = le_create();
    le_read_lef(handle, path.c_str());
    le_set_current_design(handle, 0);
    le_set_viewport_size(handle, 2000, 2000);
    le_fit_scene(handle, 10);

    // A full-viewport drag-select from the origin out to (range, range) -
    // varying how much of the fitted design gets enclosed, and therefore
    // how many pieces end up selected, at the scale the bug report
    // described (the more selected, the slower).
    le_mouse_down(handle, 0, 0);
    le_mouse_up(handle, static_cast<int32_t>(state.range(0)), static_cast<int32_t>(state.range(0)));

    const int32_t count = le_selection_count(handle);
    state.counters["selected_pieces"] = static_cast<double>(count);

    for (auto _ : state)
    {
        for (int32_t i = 0; i < count; ++i)
        {
            benchmark::DoNotOptimize(le_selected_object_kind(handle, i));
            const int32_t property_count = le_selected_object_property_count(handle, i);
            for (int32_t p = 0; p < property_count; ++p)
            {
                const LeProperty property = le_selected_object_property_at(handle, i, p);
                benchmark::DoNotOptimize(&property);
            }
        }
    }

    le_destroy(handle);
}
BENCHMARK(BM_RefreshSelectedObjects_ManySelectedPieces)->Arg(200)->Arg(800)->Arg(1400)->Arg(2000)->Unit(benchmark::kMillisecond);

// Isolated Scene::select() benchmark - times only the select() loop
// itself (not Pipeline::hit_test_rect or any other machinery le_mouse_up
// also runs), against N distinct single-rect pieces sharing one synthetic
// origin - the exact shape of a real drag-select over one Obstruction's
// whole OBS block (a real design puts every OBS item under one shared
// ObstructionId regardless of how many rects/polygons/paths it holds).
// Isolating this from the full le_mouse_up/hit_test_rect/LEF-parsing path
// makes before/after numbers for select()'s own dedup cost directly
// comparable and fast to run, unlike BM_RefreshSelectedObjects_
// ManySelectedPieces above (which depends on this being fast just to
// finish its own setup).
static void BM_SceneSelect_ManyPiecesSameOrigin(benchmark::State &state)
{
    const int n = static_cast<int>(state.range(0));

    std::vector<Shape> pieces;
    pieces.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        pieces.push_back(Shape{.layer_name = "M1", .rects = {Rect{.ll = {i * 10, 0}, .ur = {i * 10 + 5, 5}}}});

    const ObstructionId origin{1, 0};

    for (auto _ : state)
    {
        state.PauseTiming();
        Scene scene;
        state.ResumeTiming();

        for (const Shape &piece : pieces)
            scene.select(origin, piece);
        benchmark::DoNotOptimize(scene.selection().size());
    }
    state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_SceneSelect_ManyPiecesSameOrigin)->Arg(1000)->Arg(5000)->Arg(20000)->Unit(benchmark::kMillisecond);

// Investigates a reported "still slow selecting Obstruction pieces
// specifically, not Terminal pieces" symptom, after every previously-found
// Scene/Renderer-side cost was fixed. Candidate: le::to_properties(le::
// ObstructionData) - generated/obstruction.hpp - takes its argument *by
// value*, so every call deep-copies the whole ObstructionData, including
// its embedded `std::vector<Shape> shapes` (a real struct field, unlike
// Terminal's ports which are pool-referenced via Root's own index, not
// embedded - see api.cpp's build_selected_object_properties comment on
// that distinction). api.cpp's build_selected_object_properties calls
// exactly this (`le::to_properties(*obstruction)`) on every
// le_selected_object_property_count() call - i.e. on every selection
// change involving that Obstruction, regardless of which one piece was
// actually selected. Measures the real stress-design Obstruction (all
// ~900K OBS shapes under one ObstructionId) directly, not a synthetic
// reconstruction.
static void BM_ToPropertiesObstructionCopyCost(benchmark::State &state)
{
    const auto &data = stress_data();

    ObstructionId obstruction_id;
    bool found = false;
    data.root.for_each_obstruction_id([&](ObstructionId id)
                                       {
        if (!found) { obstruction_id = id; found = true; } });

    const ObstructionData *obstruction = data.root.get_obstruction(obstruction_id);
    state.counters["shapes"] = static_cast<double>(obstruction->shapes.size());

    for (auto _ : state)
    {
        const auto properties = le::to_properties(*obstruction);
        benchmark::DoNotOptimize(properties.data());
    }
}
BENCHMARK(BM_ToPropertiesObstructionCopyCost)->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();
