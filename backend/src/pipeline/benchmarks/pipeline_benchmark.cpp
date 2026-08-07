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
        const auto &filtered = pipeline.filter_by_viewport_and_size(generated, scene);
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
    const auto &viewport_filtered = setup.filter_by_viewport_and_size(generated, scene);

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

BENCHMARK_MAIN();
