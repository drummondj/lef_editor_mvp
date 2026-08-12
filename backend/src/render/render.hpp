#pragma once
#include "pixel_types.hpp"
#include "../database/database.hpp"
#include "../scene/scene.hpp"
#include "../view_style/view_style.hpp"
#include "stages/build_overlay_picture_stage.hpp"
#include "stages/build_picture_stage.hpp"
#include "stages/build_selection_overlay_picture_stage.hpp"
#include "stages/build_tiny_shapes_picture_stage.hpp"
#include "stages/compose_with_overlays_stage.hpp"
#include "stages/rasterize_stage.hpp"
#include "stages/transform_tiny_shapes_to_pixels_stage.hpp"
#include "stages/transform_to_pixels_stage.hpp"
#include <cstdint>
#include <map>
#include <vector>

namespace le
{
    /// @brief Owns one instance of each stage class above and chains them
    /// into Renderer's data-flow paths - construct one per Scene-equivalent
    /// lifetime and reuse it across repeated calls (e.g. every interactive
    /// frame); a fresh instance recomputes everything on its first call.
    ///
    ///   TransformToPixelsStage -> BuildPictureStage -> RasterizeStage (design)            \
    ///   TransformTinyShapesToPixelsStage -> BuildTinyShapesPictureStage -> RasterizeStage   -> ComposeWithOverlaysStage
    ///   BuildSelectionOverlayPictureStage -> RasterizeStage (selection)                    /
    ///   BuildOverlayPictureStage (drawn directly, not rasterized) ------------------------/
    ///
    /// Every public method below keeps the exact signature it had before
    /// UPDATES.md item 16's Renderer refactor (a `const Root&` parameter
    /// some methods no longer use in their own body is kept anyway,
    /// deliberately, so every existing caller - api.cpp, render_test.cpp,
    /// render_preview.cpp, pipeline_benchmark.cpp - needed zero changes);
    /// each is now a one-line-ish delegate to its corresponding stage
    /// member. See each stage class's own doc comment (in
    /// src/render/stages/) for its cache key and why it's shaped that way.
    ///
    /// Cache keys compose via upstream stages' own `.version()` uniformly
    /// (UPDATES.md item 16's core idea, extended here past where Pipeline's
    /// own refactor stopped - see BENCHMARKS.md for the decision writeup:
    /// unlike Pipeline, several of these stages take an already-built
    /// value as an explicit parameter, so composing here means a caller
    /// that reuses a stale artifact instead of re-running its upstream
    /// stage now gets a stale-but-cached result instead of an independent
    /// staleness check - traced every real call site first to confirm none
    /// of them do that).
    class Renderer
    {
    public:
        const std::map<ViewLayerId, std::vector<PixelShape>> &transform_to_pixels(const Root &root, const std::map<ViewLayerId, std::vector<RenderedShape>> &shapes, const Scene &scene)
        {
            return transform_stage_.run(root, shapes, scene);
        }

        const std::map<ViewLayerId, std::vector<PixelPoint>> &transform_tiny_shapes_to_pixels(const Root &root, const std::map<ViewLayerId, std::vector<Point>> &tiny_shapes, const Scene &scene)
        {
            return tiny_transform_stage_.run(root, tiny_shapes, scene);
        }

        const sk_sp<SkPicture> &build_picture(const std::map<ViewLayerId, std::vector<PixelShape>> &shapes, const Scene &scene, const ViewLayerSet &view_layers, const Root &root)
        {
            return build_picture_stage_.run(transform_stage_, shapes, scene, view_layers, root);
        }

        const sk_sp<SkPicture> &build_overlay_picture(const Scene &scene)
        {
            return build_overlay_picture_stage_.run(scene);
        }

        const sk_sp<SkPicture> &build_tiny_shapes_picture(const Root &root, const std::map<ViewLayerId, std::vector<PixelPoint>> &tiny_pixel_shapes, const Scene &scene, const ViewLayerSet &view_layers)
        {
            return build_tiny_shapes_picture_stage_.run(tiny_transform_stage_, root, tiny_pixel_shapes, scene, view_layers);
        }

        const sk_sp<SkPicture> &build_selection_overlay_picture(const Scene &scene)
        {
            return build_selection_overlay_picture_stage_.run(scene);
        }

        /// @brief Rasterize `picture` into a raw RGBA8888 PixelBuffer sized
        /// to the Scene's viewport. Kept as its own entry point (rather
        /// than only exposing compose_with_overlays below) since not every
        /// caller wants the overlay composited in - render_preview.cpp and
        /// this class's own tests call it directly.
        const PixelBuffer &rasterize(const Root &root, const sk_sp<SkPicture> &picture, const Scene &scene)
        {
            (void)root;
            return rasterize_design_stage_.run(build_picture_stage_.version(), picture, scene).buffer;
        }

        const PixelBuffer &compose_with_overlays(const Root &root, const sk_sp<SkPicture> &design_picture, const sk_sp<SkPicture> &tiny_shapes_picture, const sk_sp<SkPicture> &overlay_picture, const sk_sp<SkPicture> &selection_overlay_picture, const Scene &scene)
        {
            (void)root;
            return compose_stage_.run(rasterize_design_stage_, rasterize_tiny_stage_, rasterize_selection_stage_, build_overlay_picture_stage_,
                                       build_picture_stage_.version(), build_tiny_shapes_picture_stage_.version(), build_selection_overlay_picture_stage_.version(),
                                       design_picture, tiny_shapes_picture, overlay_picture, selection_overlay_picture, scene);
        }

        // Number of times each stage actually recomputed - exposed purely
        // to make cache hits/misses observable in tests.
        uint64_t transform_calls() const { return transform_stage_.call_count(); }
        uint64_t tiny_shapes_transform_calls() const { return tiny_transform_stage_.call_count(); }
        uint64_t picture_calls() const { return build_picture_stage_.call_count(); }
        uint64_t tiny_shapes_picture_calls() const { return build_tiny_shapes_picture_stage_.call_count(); }
        uint64_t overlay_picture_calls() const { return build_overlay_picture_stage_.call_count(); }
        uint64_t selection_overlay_picture_calls() const { return build_selection_overlay_picture_stage_.call_count(); }
        uint64_t rasterize_calls() const { return rasterize_design_stage_.call_count(); }
        uint64_t rasterize_tiny_shapes_calls() const { return rasterize_tiny_stage_.call_count(); }
        uint64_t rasterize_selection_overlay_calls() const { return rasterize_selection_stage_.call_count(); }
        uint64_t compose_calls() const { return compose_stage_.call_count(); }

    private:
        TransformToPixelsStage transform_stage_;
        TransformTinyShapesToPixelsStage tiny_transform_stage_;
        BuildPictureStage build_picture_stage_;
        BuildOverlayPictureStage build_overlay_picture_stage_;
        BuildTinyShapesPictureStage build_tiny_shapes_picture_stage_;
        BuildSelectionOverlayPictureStage build_selection_overlay_picture_stage_;
        RasterizeStage rasterize_design_stage_;
        RasterizeStage rasterize_tiny_stage_;
        RasterizeStage rasterize_selection_stage_;
        ComposeWithOverlaysStage compose_stage_;
    };
}
