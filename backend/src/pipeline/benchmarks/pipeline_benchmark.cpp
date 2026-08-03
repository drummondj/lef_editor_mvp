#include "../../io/lef_reader.hpp"
#include "../../render/render.hpp"
#include "../pipeline.hpp"
#include <benchmark/benchmark.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace le;

// Generates a deliberately unrealistic single-macro LEF file with
// kTotalShapes individual Shape entries (a fresh "LAYER ;" before every
// geometry item forces shapes_from_parser to finalize a separate Shape each
// time - see pipeline.hpp's comment on Shape granularity), split across
// PINs (10%) and one OBS block (90%), alternating M1/M2 and RECT/POLYGON/
// PATH, with position and size spread across a wide range so the pipeline's
// viewport, sub-pixel-size, and layer filters all have real work to do -
// not a realistic design, a stress test.
namespace
{
    constexpr int kTotalShapes = 1'000'000;

    // Deterministic per-item spread: 1000 columns x 1000 rows, size ramping
    // 0..~100um so roughly half the shapes land under any mid-range
    // sub-pixel threshold, alternating M1/M2 and RECT/POLYGON/PATH.
    struct ItemGeometry
    {
        double x_um;
        double y_um;
        double size_um;
        const char *layer;
    };

    ItemGeometry item_geometry(int i)
    {
        return ItemGeometry{
            .x_um = (i % 1000) * 200.0,
            .y_um = (i / 1000) * 200.0,
            .size_um = 0.001 + (i % 1000) * 0.1,
            .layer = (i % 2 == 0) ? "M1" : "M2",
        };
    }

    void write_geometry_item(std::ofstream &out, int i)
    {
        const ItemGeometry g = item_geometry(i);
        out << "      LAYER " << g.layer << " ;\n";
        switch (i % 3)
        {
        case 0: // RECT
            out << "      RECT " << g.x_um << " " << g.y_um << " "
                << (g.x_um + g.size_um) << " " << (g.y_um + g.size_um) << " ;\n";
            break;
        case 1: // POLYGON (axis-aligned quadrilateral - just exercises the POLYGON path distinctly from RECT)
            out << "      POLYGON "
                << g.x_um << " " << g.y_um << " "
                << (g.x_um + g.size_um) << " " << g.y_um << " "
                << (g.x_um + g.size_um) << " " << (g.y_um + g.size_um) << " "
                << g.x_um << " " << (g.y_um + g.size_um) << " ;\n";
            break;
        case 2: // PATH
            out << "      WIDTH " << g.size_um << " ;\n";
            out << "      PATH " << g.x_um << " " << g.y_um << " "
                << (g.x_um + g.size_um) << " " << g.y_um << " ;\n";
            break;
        }
    }

    std::string generate_stress_lef(int total_shapes)
    {
        std::filesystem::create_directories(BENCHMARK_DATA_DIR);
        std::string path = std::string(BENCHMARK_DATA_DIR) + "/stress.lef";

        std::ofstream out(path, std::ios::trunc);
        out << "VERSION 5.8 ;\n";
        out << "BUSBITCHARS \"<>\" ;\n";
        out << "DIVIDERCHAR \"/\" ;\n\n";
        out << "UNITS\n   DATABASE MICRONS 1000 ;\nEND UNITS\n\n";
        out << "LAYER M1\n   TYPE ROUTING ;\n   WIDTH 1 ;\n   PITCH 2 ;\n   DIRECTION HORIZONTAL ;\nEND M1\n\n";
        out << "LAYER M2\n   TYPE ROUTING ;\n   WIDTH 1 ;\n   PITCH 2 ;\n   DIRECTION VERTICAL ;\nEND M2\n\n";

        out << "MACRO STRESSTEST\n   CLASS CORE ;\n   SIZE 250000 BY 250000 ;\n\n";

        const int pin_shapes = total_shapes / 10;
        const int obs_shapes = total_shapes - pin_shapes;

        for (int i = 0; i < pin_shapes; ++i)
        {
            out << "   PIN P" << i << "\n      DIRECTION INPUT ;\n      PORT\n";
            write_geometry_item(out, i);
            out << "      END\n   END P" << i << "\n";
        }

        out << "   OBS\n";
        for (int i = 0; i < obs_shapes; ++i)
            write_geometry_item(out, pin_shapes + i);
        out << "   END\n";

        out << "END STRESSTEST\n";
        return path;
    }

    struct StressData
    {
        Root root;
        AbstractId abstract_id;
        ViewLayerSet view_layers;
    };

    const StressData &stress_data()
    {
        static const StressData data = [] {
            StressData d;

            auto t0 = std::chrono::steady_clock::now();
            std::string path = generate_stress_lef(kTotalShapes);
            auto t1 = std::chrono::steady_clock::now();

            LEFReader reader;
            int result = reader.read_lef(path, d.root, "stress_lib");
            auto t2 = std::chrono::steady_clock::now();

            if (result != 0)
            {
                std::cerr << "Failed to parse generated stress LEF (result=" << result << ")\n";
                std::exit(1);
            }

            DesignId design_id = d.root.get_design_by_name("STRESSTEST");
            d.abstract_id = d.root.get_design_abstract(design_id);
            d.view_layers = ViewLayerSet::build_for_technology(d.root, d.root.get_technology_ids().front());

            std::cerr << "[setup] generated " << kTotalShapes << "-shape LEF in "
                      << std::chrono::duration<double>(t1 - t0).count() << "s, parsed in "
                      << std::chrono::duration<double>(t2 - t1).count() << "s\n";

            return d;
        }();
        return data;
    }

    // scale chosen so the ~0.001..100um size spread straddles the 1px
    // threshold; viewport chosen to cover a quarter of the position grid -
    // both filters end up doing real, non-trivial culling rather than an
    // all-or-nothing pass.
    Scene make_scene(const StressData &data)
    {
        Scene scene;
        scene.set_current_abstract(data.abstract_id);
        scene.set_pan(Point{0, 0});
        scene.set_scale(0.00002); // 1px == 50,000 dbu == 50um
        scene.set_viewport_size(2000, 2000); // visible: [0, 100,000,000) dbu on each axis

        // M2 shapes appear as both PINs and OBS items in the generated LEF
        // (item_geometry alternates M1/M2 regardless of which), so hide both purposes.
        LayerId m2 = data.root.get_layer_by_name("M2");
        scene.set_layer_visible(data.view_layers.find(m2, ViewLayerPurpose::TERMINAL), false);
        scene.set_layer_visible(data.view_layers.find(m2, ViewLayerPurpose::OBSTRUCTION), false);

        return scene;
    }
}

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
        const auto &filtered = pipeline.filter_by_layer_visibility(viewport_filtered, scene);
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
    ViewLayerId m1_obstruction = data.view_layers.find(data.root.get_layer_by_name("M1"), ViewLayerPurpose::OBSTRUCTION);

    bool visible = true;
    for (auto _ : state)
    {
        scene.set_layer_visible(m1_obstruction, visible);
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
        const auto &picture = renderer.build_picture(pixel_shapes, scene, data.view_layers);
        benchmark::DoNotOptimize(picture.get());
    }
    state.SetItemsProcessed(state.iterations() * pixel_shapes.size());
}
BENCHMARK(BM_BuildPicture)->Unit(benchmark::kMillisecond);

// A fresh Pipeline + Renderer every iteration, running the full
// generate -> filter -> filter -> transform -> picture chain once each -
// the "just switched to a different Abstract" cold-start case, now
// including the render stages.
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
        const auto &picture = renderer.build_picture(pixel_shapes, scene, data.view_layers);
        benchmark::DoNotOptimize(picture.get());
    }
    state.SetItemsProcessed(state.iterations() * kTotalShapes);
}
BENCHMARK(BM_Render)->Unit(benchmark::kMillisecond);

// Reused Pipeline + Renderer across iterations, mirroring the
// BM_RunReused_* scenarios above but through the full render chain -
// real numbers for the threading question (README's open design
// question): is Skia picture generation actually a bottleneck on the
// interactive path, or does it stay cheap because it's caching-aware too?

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
        const auto &picture = renderer.build_picture(pixel_shapes, scene, data.view_layers);
        benchmark::DoNotOptimize(picture.get());
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
        const auto &picture = renderer.build_picture(pixel_shapes, scene, data.view_layers);
        benchmark::DoNotOptimize(picture.get());
    }
    state.SetItemsProcessed(state.iterations() * kTotalShapes);
}
BENCHMARK(BM_RenderReused_PanOnly)->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();
