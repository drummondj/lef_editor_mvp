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
    /// @brief Records the mouse-driven overlay chrome - the live
    /// rubber-band drag-select rectangle if a drag is in progress
    /// (see draw_drag_rect), the grid-snap indicator box (see
    /// draw_cursor), and if a shape is currently hovered its yellow
    /// outline (see draw_hover_outline) - into its own small
    /// SkPicture, cached independently of the (potentially
    /// design-sized, expensive) design picture - keyed on
    /// viewport_version()/visibility_version() (grid spacing lives
    /// there) and mouse_version(), deliberately *not* AbstractId or
    /// selection_version(): this overlay is a pure viewport/mouse
    /// concern, not design or selection content, so switching
    /// Abstracts or changing the selection doesn't need to recompute
    /// it. Selected-piece outlines are a separate picture (see
    /// BuildSelectionOverlayPictureStage) precisely so a mouse move
    /// alone never has to re-walk the selection list - see that
    /// class's own comment for why that split exists. Root of its own
    /// chain - reads Scene's drag/cursor/hover state directly, no
    /// upstream Renderer stage to compose from.
    class BuildOverlayPictureStage
    {
    public:
        const sk_sp<SkPicture> &run(const Scene &scene)
        {
            const auto key = std::tuple{scene.viewport_version(), scene.visibility_version(), scene.mouse_version()};
            return stage_.get(key, [&]
            {
                SkPictureRecorder recorder;
                SkCanvas *canvas = recorder.beginRecording(
                    SkRect::MakeWH(static_cast<SkScalar>(scene.viewport_width_px()), static_cast<SkScalar>(scene.viewport_height_px())));

                draw_drag_rect(*canvas, scene);

                draw_cursor(*canvas, scene);

                if (const auto &hover = scene.hover())
                    draw_hover_outline(*canvas, scene, *hover);

                return recorder.finishRecordingAsPicture();
            });
        }

        uint64_t version() const { return stage_.version(); }
        uint64_t call_count() const { return stage_.call_count(); }

    private:
        VersionedStage<std::tuple<uint64_t, uint64_t, uint64_t>, sk_sp<SkPicture>> stage_;
    };
}
