#pragma once
#include "../draw_helpers.hpp"
#include "../../core/versioned_stage.hpp"
#include "../../scene/scene.hpp"
#include "include/core/SkPicture.h"
#include "include/core/SkPictureRecorder.h"
#include "include/core/SkRect.h"
#include <tuple>

namespace le
{
    /// @brief Records a white outline for every selected object that has a
    /// specific piece recorded (see draw_selected_piece_outline,
    /// Scene::SelectedObject - every reachable selection always records
    /// one), into its own small SkPicture separate from
    /// BuildOverlayPictureStage's mouse-driven chrome - keyed on
    /// viewport_version()/visibility_version()/selection_version(),
    /// deliberately *not* mouse_version().
    ///
    /// This split exists because a selected PATH piece's outline is
    /// traced via Geometry::path_to_polygons (a real Boost.Geometry
    /// buffer op, not free) - before this split, that work lived in
    /// build_overlay_picture keyed together with mouse_version(), which
    /// bumps on *every* pointer-move event, meaning every mouse move
    /// re-buffered every selected path's outline from scratch even though
    /// the selection hadn't changed - a real, reported regression that got
    /// worse the more objects were selected (see BENCHMARKS.md). Root of
    /// its own chain - reads Scene::selection() directly, no upstream
    /// Renderer stage to compose from.
    class BuildSelectionOverlayPictureStage
    {
    public:
        const sk_sp<SkPicture> &run(const Scene &scene)
        {
            const auto key = std::tuple{scene.viewport_version(), scene.visibility_version(), scene.selection_version()};
            return stage_.get(key, [&]
            {
                SkPictureRecorder recorder;
                SkCanvas *canvas = recorder.beginRecording(
                    SkRect::MakeWH(static_cast<SkScalar>(scene.viewport_width_px()), static_cast<SkScalar>(scene.viewport_height_px())));

                for (const auto &selected : scene.selection())
                    if (selected.piece)
                        draw_selected_piece_outline(*canvas, scene, *selected.piece);

                return recorder.finishRecordingAsPicture();
            });
        }

        uint64_t version() const { return stage_.version(); }
        uint64_t call_count() const { return stage_.call_count(); }

    private:
        VersionedStage<std::tuple<uint64_t, uint64_t, uint64_t>, sk_sp<SkPicture>> stage_;
    };
}
