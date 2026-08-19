#pragma once
#include "../draw_helpers.hpp"
#include "../../core/rendered_shape.hpp"
#include "../../core/versioned_stage.hpp"
#include "../../database/database.hpp"
#include "../../geometry/geometry.hpp"
#include "../../scene/scene.hpp"
#include "include/core/SkPicture.h"
#include "include/core/SkPictureRecorder.h"
#include "include/core/SkRect.h"
#include <map>
#include <tuple>
#include <vector>

namespace le
{
    /// @brief Records a white outline for every selected *piece*
    /// (UPDATES.md item 21), into its own small SkPicture separate from
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
    /// worse the more objects were selected (see BENCHMARKS.md).
    ///
    /// `Scene::SelectedObject` doesn't carry its own geometry - just a
    /// `ShapeId` plus a `piece_kind`/`piece_index` identifying which one
    /// of that Shape's rects/polygons/paths is selected (see scene.hpp's
    /// own comment): this stage resolves each selected id's *whole*
    /// geometry from the Pipeline's own generated `shapes` map, the same
    /// *merged* (Geometry::merge_overlapping_fills) geometry that's
    /// actually rendered on screen - not a fresh `Root::get_shape`
    /// lookup - so the highlight traces the shape as drawn, with no
    /// seams where unmerged overlapping rects would otherwise show
    /// internal edges, then extracts just the addressed piece from that
    /// (Geometry::extract_piece) before outlining it. Note this means
    /// the outlined piece is positioned/shaped as *rendered* (merged),
    /// which can visually differ from the exact raw piece Move itself
    /// would translate for a Shape with overlapping siblings - an
    /// accepted, minor cosmetic gap (the raw piece is always what
    /// actually moves; only the highlight's own shape can look
    /// slightly different in that specific overlapping-siblings case).
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
        // `shapes` is unused (kept in the signature purely so every
        // existing caller - Renderer::build_selection_overlay_picture,
        // and the many render_test.cpp call sites that already pass it -
        // doesn't need to change): UPDATES.md item 21's piece-granular
        // selection resolves each selected piece straight from `root`
        // (Geometry::extract_piece against the real, raw stored
        // geometry) rather than from Pipeline's *rendered*
        // (merge_overlapping_fills-processed) `shapes` map, which the
        // old whole-shape design used to resolve here - see this class's
        // own header comment for why a merged lookup would silently
        // fail to find a piece-level selection at all for any Shape
        // bundling 2+ rects/polygons (merge changes both the piece count
        // and kind).
        const sk_sp<SkPicture> &run(const Scene &scene, const Root &root, const std::map<ViewLayerId, std::vector<RenderedShape>> &shapes)
        {
            (void)shapes;
            const auto key = std::tuple{scene.current_abstract(), scene.viewport_version(), scene.visibility_version(), scene.selection_version(), root.mutation_version()};
            return stage_.get(key, [&]
            {
                SkPictureRecorder recorder;
                SkCanvas *canvas = recorder.beginRecording(
                    SkRect::MakeWH(static_cast<SkScalar>(scene.viewport_width_px()), static_cast<SkScalar>(scene.viewport_height_px())));

                for (const auto &selected : scene.selection())
                {
                    if (const ShapeData *data = root.get_shape(selected.shape_id))
                        draw_selected_piece_outline(*canvas, scene, Geometry::extract_piece(*data, selected.piece_kind, selected.piece_index));
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
