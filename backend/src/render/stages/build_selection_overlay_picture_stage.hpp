#pragma once
#include "../draw_helpers.hpp"
#include "../../core/rendered_shape.hpp"
#include "../../core/versioned_stage.hpp"
#include "../../database/database.hpp"
#include "../../scene/scene.hpp"
#include "include/core/SkPicture.h"
#include "include/core/SkPictureRecorder.h"
#include "include/core/SkRect.h"
#include <map>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace le
{
    /// @brief Records a white outline for every selected Shape, into its
    /// own small SkPicture separate from BuildOverlayPictureStage's mouse-
    /// driven chrome - keyed on viewport_version()/visibility_version()/
    /// selection_version(), deliberately *not* mouse_version().
    ///
    /// This split exists because a selected PATH piece's outline is
    /// traced via Geometry::path_to_polygons (a real Boost.Geometry
    /// buffer op, not free) - before this split, that work lived in
    /// build_overlay_picture keyed together with mouse_version(), which
    /// bumps on *every* pointer-move event, meaning every mouse move
    /// re-buffered every selected path's outline from scratch even though
    /// the selection hadn't changed - a real, reported regression that got
    /// worse the more objects were selected (see BENCHMARKS.md).
    ///
    /// `Scene::SelectedObject` no longer carries its own geometry (it's
    /// just a `ShapeId` - see scene.hpp's own comment on why selection is
    /// shape-granular): this stage resolves each selected id's geometry
    /// from the Pipeline's own generated `shapes` map, the same *merged*
    /// (Geometry::merge_overlapping_fills) geometry that's actually
    /// rendered on screen - not a fresh `Root::get_shape` lookup - so the
    /// highlight traces the shape as drawn, with no seams where unmerged
    /// overlapping rects would otherwise show internal edges. This is
    /// orthogonal to why selection became shape-level in the first place
    /// (a Property Viewer correctness issue, not a rendering one).
    ///
    /// Key adds `Root::mutation_version()`/`Scene::current_abstract()`
    /// (mirroring `TransformToPixelsStage`'s own key exactly) since this
    /// stage now depends on a Pipeline-derived artifact (`shapes`) with no
    /// upstream Renderer-linked `.version()` to compose from - without it,
    /// editing a currently-selected shape's geometry (e.g. via TCL, no
    /// selection change) would keep serving a stale cached highlight.
    class BuildSelectionOverlayPictureStage
    {
    public:
        const sk_sp<SkPicture> &run(const Scene &scene, const Root &root, const std::map<ViewLayerId, std::vector<RenderedShape>> &shapes)
        {
            const auto key = std::tuple{scene.current_abstract(), scene.viewport_version(), scene.visibility_version(), scene.selection_version(), root.mutation_version()};
            return stage_.get(key, [&]
            {
                SkPictureRecorder recorder;
                SkCanvas *canvas = recorder.beginRecording(
                    SkRect::MakeWH(static_cast<SkScalar>(scene.viewport_width_px()), static_cast<SkScalar>(scene.viewport_height_px())));

                std::unordered_map<ShapeId, const RenderedShape *> by_shape_id;
                for (const auto &[view_layer_id, group] : shapes)
                    for (const RenderedShape &rs : group)
                        if (rs.shape_id)
                            by_shape_id.emplace(*rs.shape_id, &rs);

                for (const auto &selected : scene.selection())
                {
                    const auto it = by_shape_id.find(selected.shape_id);
                    if (it != by_shape_id.end())
                        draw_selected_piece_outline(*canvas, scene, it->second->shape);
                }

                return recorder.finishRecordingAsPicture();
            });
        }

        uint64_t version() const { return stage_.version(); }
        uint64_t call_count() const { return stage_.call_count(); }

    private:
        VersionedStage<std::tuple<AbstractId, uint64_t, uint64_t, uint64_t, uint64_t>, sk_sp<SkPicture>> stage_;
    };
}
