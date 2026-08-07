#pragma once
#include "../pipeline/pipeline.hpp"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkFont.h"
#include "include/core/SkImage.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPicture.h"
#include "include/core/SkPictureRecorder.h"
#include "include/core/SkRect.h"
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkShader.h"
#include "include/core/SkSurface.h"
#include "include/core/SkTileMode.h"
#include "include/core/SkTypeface.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <tuple>
#include <vector>

namespace le
{
    struct PixelPoint
    {
        double x = 0.0;
        double y = 0.0;
    };

    struct PixelRect
    {
        PixelPoint ll;
        PixelPoint ur;
    };

    struct PixelPolygon
    {
        std::vector<PixelPoint> points;
    };

    struct PixelPath
    {
        PixelPolygon polygon;
        double width = 0.0;
    };

    struct PixelText
    {
        std::string label;
        PixelPoint location;
        double size = 0.0;
    };

    /// @brief A Shape transformed from dbu-space to pixel-space (Scene's
    /// `pixel = (dbu - pan) * scale`), mirroring Shape's own
    /// rects/polygons/paths/texts structure with double coordinates instead
    /// of Rect/Polygon/Path/Text's integer dbu ones. Still no Y-axis flip
    /// applied here - dbu-space y increases upward (physical layout
    /// convention) and this maps it straight through, so pixel-space y does
    /// too, which doesn't match Skia's own y-down canvas convention until
    /// corrected. That correction happens at `Renderer::rasterize()` (a
    /// canvas transform applied once per frame, not a per-shape one here) -
    /// see its own doc comment. No ViewLayerId field - callers get that
    /// from the grouping map key (see Renderer) instead of carrying a
    /// redundant copy per shape.
    struct PixelShape
    {
        std::vector<PixelRect> rects;
        std::vector<PixelPolygon> polygons;
        std::vector<PixelPath> paths;
        std::vector<PixelText> texts;

        /// Mirrors RenderedShape::origin (pipeline.hpp) - nullopt for the
        /// BOUNDARY shape. Carried through transform_to_pixels unchanged
        /// (no coordinate transform needed) so build_picture can draw a
        /// selection outline (UPDATES.md 7) without needing the
        /// dbu-space RenderedShape map too.
        std::optional<SelectionRef> origin;
    };

    /// @brief Raw RGBA8888 pixel data for one rasterized frame, produced by
    /// `Renderer::rasterize()`. Explicitly `kRGBA_8888_SkColorType` (not
    /// Skia's platform-native `kN32_SkColorType`, which is BGRA on some
    /// platforms and RGBA on others - see `SkColorType.h`) so the byte
    /// layout is identical between this project's macOS dev machine and
    /// its Linux deployment target; the raw output isn't visually
    /// inspectable, so a silent per-platform channel-order mismatch here
    /// would be very easy to miss until it showed up as wrong colors on
    /// Linux specifically. Premultiplied alpha, row-major, top-to-bottom
    /// (row 0 is the top on-screen row - see `rasterize()`'s Y-flip). Exact
    /// row_bytes may exceed `width * 4` (Skia may pad rows for alignment) -
    /// always index by it, never assume a tight stride. `data` points into
    /// memory owned by the `Renderer` instance (the cached raster surface
    /// backing it) - valid only until the next call that invalidates this
    /// cache entry, same lifetime convention as `build_picture`'s returned
    /// `sk_sp<SkPicture>&`. The exact format/orientation a real Flutter
    /// texture needs isn't confirmed against Flutter's own API yet (no
    /// `api`/plugin module exists in this repo yet) - revisit this comment
    /// once that integration happens.
    struct PixelBuffer
    {
        const uint8_t *data = nullptr;
        int width = 0;
        int height = 0;
        size_t row_bytes = 0;
    };

    /// @brief Transforms Pipeline's filtered, ViewLayerId-grouped dbu-space
    /// output into pixel space, records it into an SkPicture via Skia draw
    /// calls, then rasterizes that picture into a raw RGBA8888 PixelBuffer.
    /// Three CachedStage-backed stages (see pipeline.hpp), keyed the same
    /// way Pipeline::filter_by_layer_visibility's output already is -
    /// AbstractId + Scene::viewport_version() + Scene::visibility_version()
    /// already cover everything all three depend on (pan/scale/viewport-
    /// size all bump viewport_version()).
    ///
    /// Grouping is preserved through the transform (std::map<ViewLayerId,
    /// vector<PixelShape>>, not a flat vector) so build_picture can look up
    /// each ViewLayer's style and construct its fill/stroke SkPaint once
    /// per group instead of once per shape, and so iterating the map (in
    /// ViewLayerId order - see Pipeline::filter_by_layer_visibility's
    /// comment for why that's bottom-up draw order) needs no separate sort.
    ///
    /// Takes a Pipeline& from the caller rather than owning one - matches
    /// how Pipeline's own stages take the previous stage's output as an
    /// explicit parameter rather than owning it, and avoids a Scene needing
    /// two separate cache-holding objects with the same lifetime. One
    /// Renderer instance per Scene-equivalent lifetime, same convention as
    /// Pipeline.
    ///
    /// Single-threaded for now - no benchmark yet shows Skia picture
    /// generation is a bottleneck worth threading (see README's Threading
    /// open design question); revisit once BENCHMARKS.md has real numbers.
    class Renderer
    {
    public:
        const std::map<ViewLayerId, std::vector<PixelShape>> &transform_to_pixels(const std::map<ViewLayerId, std::vector<RenderedShape>> &shapes, const Scene &scene)
        {
            const auto key = std::tuple{scene.current_abstract(), scene.viewport_version(), scene.visibility_version()};
            return pixel_shapes_.get(key, [&]
                                     {
                const Point pan = scene.pan();
                const double scale = scene.scale();
                auto to_pixel = [&](Point p)
                {
                    return PixelPoint{
                        .x = (static_cast<double>(p.x) - static_cast<double>(pan.x)) * scale,
                        .y = (static_cast<double>(p.y) - static_cast<double>(pan.y)) * scale,
                    };
                };

                std::map<ViewLayerId, std::vector<PixelShape>> result;

                for (const auto &[view_layer, group] : shapes)
                {
                    std::vector<PixelShape> pixel_group;
                    pixel_group.reserve(group.size());

                    for (const auto &rs : group)
                    {
                        PixelShape ps;
                        ps.origin = rs.origin;

                        ps.rects.reserve(rs.shape.rects.size());
                        for (const auto &r : rs.shape.rects)
                            ps.rects.push_back(PixelRect{.ll = to_pixel(r.ll), .ur = to_pixel(r.ur)});

                        ps.polygons.reserve(rs.shape.polygons.size());
                        for (const auto &poly : rs.shape.polygons)
                        {
                            PixelPolygon pp;
                            pp.points.reserve(poly.points.size());
                            for (const auto &pt : poly.points)
                                pp.points.push_back(to_pixel(pt));
                            ps.polygons.push_back(std::move(pp));
                        }

                        ps.paths.reserve(rs.shape.paths.size());
                        for (const auto &path : rs.shape.paths)
                        {
                            PixelPath pp;
                            pp.width = static_cast<double>(path.width) * scale;
                            pp.polygon.points.reserve(path.polygon.points.size());
                            for (const auto &pt : path.polygon.points)
                                pp.polygon.points.push_back(to_pixel(pt));
                            ps.paths.push_back(std::move(pp));
                        }

                        ps.texts.reserve(rs.shape.texts.size());
                        for (const auto &t : rs.shape.texts)
                        {
                            // t.size (dbu) scales like any other width
                            // (mirrors PixelPath.width above), but a label
                            // shouldn't literally touch/overflow the edges
                            // of the geometry it's on (kLabelWidthRatio),
                            // and must stay legible even on hair-thin
                            // geometry at any zoom level (kMinLabelPixelSize).
                            const double pixel_size = std::max(t.size * scale * kLabelWidthRatio, kMinLabelPixelSize);
                            ps.texts.push_back(PixelText{.label = t.label, .location = to_pixel(t.location), .size = pixel_size});
                        }

                        pixel_group.push_back(std::move(ps));
                    }

                    result.emplace(view_layer, std::move(pixel_group));
                }

                return result; });
        }

        /// @brief Record the pixel-space shapes into an SkPicture, sized to
        /// the Scene's viewport, drawn in map order (bottom-up, see
        /// Pipeline::filter_by_layer_visibility's comment) on top of the
        /// background dot grid + axis lines drawn first by draw_grid, and
        /// the currently selected Abstract's own origin marker (see
        /// draw_origin_marker, UPDATES.md 5.4) drawn just after - `root` is
        /// only needed to look up that Abstract's AbstractData::origin;
        /// skipped entirely if scene.current_abstract() doesn't resolve
        /// (no Design selected). Each group's ViewLayerStyle (outline/fill
        /// Color) comes from `view_layers`; a ViewLayerId that doesn't
        /// resolve to a known ViewLayer (see Pipeline's layer filter
        /// comment on why that's kept, not dropped) has its whole group
        /// skipped here - there's no style to draw it with. A final pass
        /// draws a white outline (see draw_selection_outline, UPDATES.md
        /// 7) around every PixelShape whose origin is currently selected
        /// (Scene::is_selected) - after every group, so a selected
        /// shape's outline is never obscured by a shape on a higher
        /// layer, unlike its own per-group outline/fill.
        const sk_sp<SkPicture> &build_picture(const std::map<ViewLayerId, std::vector<PixelShape>> &shapes, const Scene &scene, const ViewLayerSet &view_layers, const Root &root)
        {
            const auto key = std::tuple{scene.current_abstract(), scene.viewport_version(), scene.visibility_version(), scene.selection_version()};
            return picture_.get(key, [&]
                                {
                SkPictureRecorder recorder;
                SkCanvas *canvas = recorder.beginRecording(
                    SkRect::MakeWH(static_cast<SkScalar>(scene.viewport_width_px()), static_cast<SkScalar>(scene.viewport_height_px())));

                // Grid first, so it sits underneath the real design
                // geometry drawn below rather than obscuring it.
                draw_grid(*canvas, scene);

                if (const AbstractData *abstract = root.get_abstract(scene.current_abstract()))
                    draw_origin_marker(*canvas, scene, abstract->origin);

                for (const auto &[view_layer_id, group] : shapes)
                {
                    const ViewLayerData *view_layer = view_layers.get(view_layer_id);
                    if (!view_layer)
                        continue;

                    draw_group(*canvas, group, view_layer->style);
                }

                if (!scene.selection().empty())
                {
                    for (const auto &[view_layer_id, group] : shapes)
                        for (const auto &pixel_shape : group)
                            if (pixel_shape.origin && scene.is_selected(*pixel_shape.origin))
                                draw_selection_outline(*canvas, pixel_shape);
                }

                return recorder.finishRecordingAsPicture(); });
        }

        /// @brief Records the mouse-driven overlay chrome - the live
        /// rubber-band drag-select rectangle if a drag is in progress
        /// (see draw_drag_rect), the grid-snap indicator box (see
        /// draw_cursor), and, if a shape is currently hovered, its yellow
        /// outline (see draw_hover_outline) - into its own small
        /// SkPicture, cached independently of the (potentially
        /// design-sized, expensive) design picture above - keyed on
        /// viewport_version()/visibility_version() (grid spacing lives
        /// there) and mouse_version(), deliberately *not* AbstractId: this
        /// overlay is a pure viewport/mouse concern, not design content,
        /// so switching Abstracts doesn't need to recompute it. Neither
        /// hover state (Scene::hover()) nor drag state
        /// (Scene::is_dragging()/drag_rect_dbu()) needs its own cache key
        /// - set_hover/begin_drag/end_drag are only ever called alongside
        /// set_mouse_position (or bump mouse_version_ themselves, for
        /// begin_drag/end_drag), which already bumps mouse_version() on
        /// every call - see compose_with_overlays for how this combines
        /// with the design picture without re-rasterizing it, this
        /// project's answer to UPDATES.md 5.2's own flagged perf concern
        /// (recomputing this tiny picture on every mouse-move event is
        /// cheap; recomputing/re-rasterizing the whole design picture on
        /// every mouse-move event would not be).
        const sk_sp<SkPicture> &build_overlay_picture(const Scene &scene)
        {
            const auto key = std::tuple{scene.viewport_version(), scene.visibility_version(), scene.mouse_version()};
            return overlay_picture_.get(key, [&]
                                        {
                SkPictureRecorder recorder;
                SkCanvas *canvas = recorder.beginRecording(
                    SkRect::MakeWH(static_cast<SkScalar>(scene.viewport_width_px()), static_cast<SkScalar>(scene.viewport_height_px())));

                draw_drag_rect(*canvas, scene);

                draw_cursor(*canvas, scene);

                if (const auto &hover = scene.hover())
                    draw_hover_outline(*canvas, scene, *hover);

                return recorder.finishRecordingAsPicture(); });
        }

        /// @brief Rasterize `picture` into a raw RGBA8888 PixelBuffer sized
        /// to the Scene's viewport - see rasterize_frame's own comment for
        /// the full Y-flip/format rationale (this is a thin wrapper over
        /// it, discarding the RasterizedFrame's surface and keeping just
        /// its PixelBuffer view). Kept as its own entry point (rather than
        /// only exposing compose_with_overlays below) since not every
        /// caller wants the overlay composited in - render_preview.cpp
        /// and this class's own tests call it directly.
        const PixelBuffer &rasterize(const sk_sp<SkPicture> &picture, const Scene &scene)
        {
            return rasterize_frame(picture, scene).buffer;
        }

        /// @brief Composites the design picture's cached, already-rasterized
        /// frame (from rasterize_frame - the expensive step for a large
        /// design) with the cheap mouse-overlay picture (see
        /// build_overlay_picture) into one final RGBA8888 buffer, without
        /// re-rasterizing the design's own vector content on every call.
        /// Reuses the design's cached raster surface via a cheap SkImage
        /// snapshot + blit (`SkCanvas::drawImage`, a bitmap copy - not a
        /// re-walk of the design's draw ops) rather than recording both
        /// pictures into one fresh canvas, which would re-execute every
        /// shape's draw calls again on every mouse move. `overlay_picture`
        /// is drawn through the same whole-canvas Y-flip rasterize_frame
        /// applies to the design (translate + scale(1,-1) - see its own
        /// comment for why that flip exists), since build_overlay_picture
        /// records pre-flip pixel-space coordinates, the same convention
        /// build_picture's own shapes use.
        ///
        /// Cached on all five of AbstractId/viewport_version/
        /// visibility_version/selection_version/mouse_version - a
        /// superset of rasterize_frame's own key (selection_version
        /// included here for the same reason it's in rasterize_frame's
        /// key: `design_picture`'s content depends on it, and
        /// CachedStage's key comparison is the only thing that decides
        /// whether to recompute - it never inspects `design_picture`
        /// itself, so a key that doesn't change would silently return a
        /// stale composited buffer even though a *different* picture was
        /// passed in). A mouse-only change *does* invalidate this entry,
        /// but recomputing it only costs one full-viewport image blit
        /// plus the tiny overlay picture, not anything proportional to
        /// design complexity - this project's answer to UPDATES.md 5.2's
        /// own flagged perf concern (a naive single-picture-per-frame
        /// design would re-rasterize the whole design on every
        /// mouse-move event).
        const PixelBuffer &compose_with_overlays(const sk_sp<SkPicture> &design_picture, const sk_sp<SkPicture> &overlay_picture, const Scene &scene)
        {
            const auto key = std::tuple{scene.current_abstract(), scene.viewport_version(), scene.visibility_version(), scene.selection_version(), scene.mouse_version()};
            return composed_.get(key, [&]
                                 {
                const RasterizedFrame &design_frame = rasterize_frame(design_picture, scene);

                const int width = scene.viewport_width_px();
                const int height = scene.viewport_height_px();
                if (width <= 0 || height <= 0 || !design_frame.surface)
                    return RasterizedFrame{};

                const SkImageInfo info = SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
                sk_sp<SkSurface> surface = SkSurfaces::Raster(info);
                SkCanvas *canvas = surface->getCanvas();
                canvas->clear(SK_ColorTRANSPARENT);

                // Cheap blit of the already-rasterized design pixels - no
                // re-walk of its underlying (potentially design-sized)
                // draw commands.
                canvas->drawImage(design_frame.surface->makeImageSnapshot(), 0, 0);

                canvas->translate(0, static_cast<SkScalar>(height));
                canvas->scale(1, -1);
                canvas->drawPicture(overlay_picture);

                SkPixmap pixmap;
                surface->peekPixels(&pixmap);

                return RasterizedFrame{
                    .surface = std::move(surface),
                    .buffer = PixelBuffer{
                        .data = static_cast<const uint8_t *>(pixmap.addr()),
                        .width = width,
                        .height = height,
                        .row_bytes = pixmap.rowBytes(),
                    },
                }; })
                .buffer;
        }

        // Number of times each stage actually recomputed - exposed purely
        // to make cache hits/misses observable in tests.
        uint64_t transform_calls() const { return pixel_shapes_.call_count(); }
        uint64_t picture_calls() const { return picture_.call_count(); }
        uint64_t overlay_picture_calls() const { return overlay_picture_.call_count(); }
        uint64_t rasterize_calls() const { return rasterized_.call_count(); }
        uint64_t compose_calls() const { return composed_.call_count(); }

    private:
        static SkColor to_sk_color(Color c) { return SkColorSetARGB(c.a, c.r, c.g, c.b); }

        // SkFont's own default constructor uses a null typeface, which
        // resolves to SkTypeface::MakeEmpty() (a typeface with no glyphs -
        // drawString silently draws nothing, no error). A real system
        // default has to come from a platform font manager instead -
        // CoreText-backed on macOS (matches this project's dev machine and
        // the Skia checkout's font manager - see CLAUDE.md's Skia setup
        // note); a Linux build needs an equivalent (e.g. fontconfig/
        // FreeType-backed SkFontMgr) added when that target exists - not
        // done yet, matching this project's target being Linux servers but
        // development happening on macOS for now.
        //
        // Defined out-of-line in render.cpp, not here: SkFontMgr_mac_ct.h
        // pulls in ApplicationServices.h, which defines legacy Carbon
        // Rect/Point/Polygon typedefs at global scope - colliding with
        // le::Rect/le::Point/le::Polygon (ambiguous lookup) in every
        // translation unit that includes this header and does `using
        // namespace le` (which all of this project's test/benchmark files
        // do). Isolating it to its own .cpp keeps that collision from
        // leaking into render.hpp's consumers, the same reason io/ is a
        // compiled library rather than header-only around its own vendored
        // C headers.
        static sk_sp<SkTypeface> default_typeface();

        static SkPath to_sk_path(const PixelPolygon &poly, bool close)
        {
            if (poly.points.empty())
                return SkPath();

            SkPathBuilder builder;
            builder.moveTo(static_cast<SkScalar>(poly.points.front().x), static_cast<SkScalar>(poly.points.front().y));
            for (size_t i = 1; i < poly.points.size(); ++i)
                builder.lineTo(static_cast<SkScalar>(poly.points[i].x), static_cast<SkScalar>(poly.points[i].y));
            if (close)
                builder.close();

            return builder.detach();
        }

        // Fixed screen-pixel tile size for every tiled FillPattern below
        // except the diagonal stripes (which need their own - see
        // kDiagonalStripeTileSize) - deliberately not scaled with
        // Scene::scale() (the pattern stays a constant visual density at
        // any zoom level, like a hatch fill in a CAD tool, rather than
        // shrinking to nothing zoomed out or ballooning zoomed in).
        static constexpr int kPatternTileSize = 12;

        // Spacing between stripes, in screen pixels.
        static constexpr SkScalar kDiagonalStripePeriod = 8.0f;

        // SkShader's kRepeat tiling only ever translates by exact multiples
        // of the tile's own size - so for the "redundant offset lines,
        // let the canvas clip them" technique below to reconstruct a
        // truly continuous periodic hatch (not a phase-shifted zigzag
        // between tiles), the tile size *must* be an exact multiple of
        // kDiagonalStripePeriod. 3x gives a reasonable amount of visible
        // repetition per tile without an oversized offscreen surface.
        // Don't change kDiagonalStripePeriod without keeping this in sync.
        static constexpr int kDiagonalStripeTileSize = static_cast<int>(kDiagonalStripePeriod) * 3;

        // Minimum on-screen text size in pixels regardless of how thin the
        // labeled geometry is - keeps labels legible at any zoom level
        // instead of shrinking to unreadable specks on hair-thin paths/
        // polygon arms. Placeholder default, easily tuned.
        static constexpr double kMinLabelPixelSize = 8.0;

        // Fraction of the local geometry width actually used for text
        // size, so a label doesn't touch/overflow the edges of the shape
        // it's on. Placeholder default, easily tuned.
        static constexpr double kLabelWidthRatio = 0.6;

        // Below this on-screen pixel spacing, a grid dot tier (minor or
        // major, checked independently) is hidden entirely rather than
        // smearing into a solid wash as the view zooms out - see
        // draw_grid's own comment. Placeholder default, easily tuned.
        static constexpr double kMinGridDotPixelSpacing = 8.0;

        // Major and minor dots are drawn the same size - only kMajorGridColor's
        // brightness distinguishes the two tiers.
        static constexpr float kGridDotRadius = 1.0f;

        // Grid dots/axis lines are UI chrome, not design geometry -
        // deliberately muted/neutral so they don't compete visually with
        // real shapes; kMajorGridColor is brighter/more opaque than
        // kMinorGridColor so the major tier still reads as bolder despite
        // being drawn at the same radius. Placeholder defaults, easily tuned.
        static constexpr Color kMinorGridColor = {128, 128, 128, 120};
        static constexpr Color kMajorGridColor = {255, 255, 255, 230};
        static constexpr Color kAxisLineColor = {255, 255, 255, 160};

        // Origin marker (UPDATES.md 5.4) - fully opaque, and a color
        // distinct from both the grid (gray/white) and the cursor box
        // (red) so it isn't confused with either. Fixed on-screen size
        // regardless of zoom, same rationale as kCursorBoxSizePx.
        static constexpr Color kOriginMarkerColor = {255, 200, 0, 255};
        static constexpr float kOriginMarkerStrokeWidth = 2.0f;
        static constexpr float kOriginMarkerSizePx = 16.0f;

        // Grid-snap indicator box (UPDATES.md 5.2) - fully opaque so it
        // stays visible over any layer color/pattern underneath. Fixed
        // on-screen size regardless of zoom (see draw_cursor) - 7x7px
        // with a 1px stroke leaves a ~6px transparent interior, comfortably
        // enough for the grid dot itself (kGridDotRadius, 2px diameter) to
        // show through centered inside the box rather than being overdrawn
        // by the stroke.
        static constexpr Color kCursorBoxColor = {255, 0, 0, 255};
        static constexpr float kCursorBoxStrokeWidth = 1.0f;
        static constexpr float kCursorBoxSizePx = 7.0f;

        // Hover outline (UPDATES.md 7.1 item 1) - opaque yellow, distinct
        // from the grid/origin marker/cursor box colors above. Traces the
        // actual geometry of the hovered piece (see draw_hover_outline),
        // so unlike the cursor box/origin marker this isn't a fixed
        // on-screen size - it scales with the shape like real geometry.
        static constexpr Color kHoverOutlineColor = {255, 255, 0, 255};
        static constexpr float kHoverOutlineStrokeWidth = 2.0f;

        // Selection outline (UPDATES.md 7) - opaque white, distinct from
        // every other overlay color above (and from every layer's own
        // default palette color, none of which are pure white). Unlike
        // hover (one piece only), every piece belonging to a selected
        // Terminal/Obstruction is outlined - once an object is actually
        // selected, the whole thing reads as selected, not just whichever
        // piece happened to be under the cursor at click time.
        static constexpr Color kSelectionOutlineColor = {255, 255, 255, 255};
        static constexpr float kSelectionOutlineStrokeWidth = 2.0f;
        // A selected Path is stroked in white at its own width plus this
        // margin on each side (not just retraced at the same width) so it
        // reads as a highlighted halo rather than simply recoloring the
        // wire white.
        static constexpr float kSelectionPathHaloMarginPx = 2.0f;

        // Rubber-band drag-select rectangle (UPDATES.md 7.1 item 5) - a
        // translucent fill so covered shapes stay visible underneath, plus
        // a solid stroke for a crisp edge. Blue, a color family not
        // already used by the grid/origin marker/cursor box/hover outline
        // above.
        static constexpr Color kDragRectFillColor = {80, 160, 255, 60};
        static constexpr Color kDragRectStrokeColor = {80, 160, 255, 220};
        static constexpr float kDragRectStrokeWidth = 2.0f;

        // Renders one FillPattern into a small transparent-background tile
        // and wraps it in a kRepeat/kRepeat SkShader - this project's
        // answer to "maybe using a shader?" (UPDATES.md 2.3): Skia's
        // SkShader tiling works entirely on a CPU raster surface, no GPU
        // involved, so it fits this project's no-GPU target directly.
        // FillPattern::NONE and ::CROSS return null - NONE draws as today's
        // flat color (no shader needed) and CROSS is drawn directly against
        // each shape's own bounds in draw_group instead of tiled (see its
        // own comment for why a repeating tile is the wrong shape for it).
        static sk_sp<SkShader> pattern_shader(FillPattern pattern, SkColor color)
        {
            if (pattern == FillPattern::NONE || pattern == FillPattern::CROSS)
                return nullptr;

            const bool diagonal = pattern == FillPattern::DIAGONAL_STRIPES_NE || pattern == FillPattern::DIAGONAL_STRIPES_NW;
            const int tile_size = diagonal ? kDiagonalStripeTileSize : kPatternTileSize;

            const SkImageInfo info = SkImageInfo::MakeN32Premul(tile_size, tile_size);
            sk_sp<SkSurface> surface = SkSurfaces::Raster(info);
            SkCanvas *canvas = surface->getCanvas();
            canvas->clear(SK_ColorTRANSPARENT);

            SkPaint paint;
            // Anti-aliasing off, deliberately - a hairline at an exact
            // pixel-grid coordinate (e.g. this tile's own half-size
            // offsets) still gets split into two ~50%-coverage rows by
            // Skia's AA, which repeated across every tile turns crisp
            // brick/stripe edges into a hazy, low-alpha wash instead of a
            // legible pattern. Crisp, fully-opaque single-pixel lines read
            // far better at this tile's small size.
            paint.setAntiAlias(false);
            paint.setColor(color);

            const auto s = static_cast<SkScalar>(tile_size);

            switch (pattern)
            {
            case FillPattern::DIAGONAL_STRIPES_NE:
            case FillPattern::DIAGONAL_STRIPES_NW:
            {
                // A field of parallel 45-degree segments spanning past the
                // tile's own edges (not just within [0, s]) so the hatch
                // reads as continuous stripes once tiled, not a sawtooth -
                // the classic tiled-hatch technique (e.g. CAD ANSI31 fill).
                paint.setStyle(SkPaint::kStroke_Style);
                const bool ne = pattern == FillPattern::DIAGONAL_STRIPES_NE;
                for (SkScalar offset = -s; offset <= 2 * s; offset += kDiagonalStripePeriod)
                {
                    if (ne)
                        canvas->drawLine(offset, 0, offset + s, s, paint);
                    else
                        canvas->drawLine(offset + s, 0, offset, s, paint);
                }
                break;
            }
            case FillPattern::BRICK:
            {
                // Two staggered rows (the standard masonry half-offset
                // joint). Tiling repeats this tile's own content exactly -
                // it doesn't draw anything extra at the seam - so both
                // horizontal mortar joints need drawing explicitly: one at
                // the tile's own top edge (y=0, becoming the joint between
                // this tile's top row and the previous tile's bottom row)
                // and one at the row split (y=s/2). Leaving the y=0 one out
                // meant every other row boundary had no joint at all.
                paint.setStyle(SkPaint::kStroke_Style);
                canvas->drawLine(0, 0, s, 0, paint);             // horizontal joint at the tile's top edge
                canvas->drawLine(0, s / 2, s, s / 2, paint);     // horizontal joint between the two rows
                canvas->drawLine(s / 2, 0, s / 2, s / 2, paint); // top row's interior vertical joint
                canvas->drawLine(0, s / 2, 0, s, paint);         // bottom row's interior vertical joint (staggered to the tile edge)
                break;
            }
            case FillPattern::DOTS:
            {
                paint.setStyle(SkPaint::kFill_Style);
                canvas->drawCircle(s / 2, s / 2, s * 0.15f, paint);
                break;
            }
            default:
                break;
            }

            sk_sp<SkImage> image = surface->makeImageSnapshot();
            return image->makeShader(SkTileMode::kRepeat, SkTileMode::kRepeat, SkSamplingOptions());
        }

        // Draws the background dot grid (major/minor tiers, dbu-space
        // lattices independent of any Shape/ViewLayer - the only thing
        // this class draws that isn't derived from pipeline output) plus
        // solid axis lines at dbu (x=0)/(y=0) - UPDATES.md 5.1.
        //
        // Direct per-dot drawing, not a tiled SkShader (unlike
        // pattern_shader): dot positions are anchored to absolute dbu
        // coordinates that shift with `pan`, and a shader tile's phase
        // would need a local-matrix offset recomputed every frame to stay
        // aligned - simple direct drawing sidesteps that entirely, and
        // hiding a tier once its on-screen pixel spacing drops below
        // kMinGridDotPixelSpacing already bounds the worst case to a few
        // tens of thousands of dots (viewport_px / kMinGridDotPixelSpacing
        // per axis), not an unbounded loop. Revisit if a benchmark ever
        // shows this mattering.
        //
        // Major and minor lattices are iterated independently (not "walk
        // the minor lattice and check every 10th point"), so this stays
        // correct even if major spacing isn't an exact multiple of minor
        // spacing - a minor-lattice point that coincides with a major one
        // is skipped in favor of the major draw below it, avoiding a
        // double-draw at the default 50/5 = 10x ratio.
        static void draw_grid(SkCanvas &canvas, const Scene &scene)
        {
            const double scale = scene.scale();
            const int width_px = scene.viewport_width_px();
            const int height_px = scene.viewport_height_px();
            if (scale <= 0.0 || width_px <= 0 || height_px <= 0)
                return;

            const Point pan = scene.pan();

            SkPaint axis_paint;
            axis_paint.setColor(to_sk_color(kAxisLineColor));
            axis_paint.setStyle(SkPaint::kStroke_Style);
            const auto axis_x_px = static_cast<SkScalar>((0.0 - static_cast<double>(pan.x)) * scale);
            const auto axis_y_px = static_cast<SkScalar>((0.0 - static_cast<double>(pan.y)) * scale);
            // Skia clips these to the canvas for free when the dbu origin
            // itself is off-screen - no visibility check needed first.
            canvas.drawLine(axis_x_px, 0, axis_x_px, static_cast<SkScalar>(height_px), axis_paint);
            canvas.drawLine(0, axis_y_px, static_cast<SkScalar>(width_px), axis_y_px, axis_paint);

            const int64_t minor_spacing = scene.minor_grid_spacing();
            const int64_t major_spacing = scene.major_grid_spacing();
            if (minor_spacing <= 0 || major_spacing <= 0)
                return;

            // Visible dbu-space range: pixel = (dbu - pan) * scale, so
            // dbu = pixel / scale + pan.
            const double dbu_x_min = static_cast<double>(pan.x);
            const double dbu_x_max = dbu_x_min + width_px / scale;
            const double dbu_y_min = static_cast<double>(pan.y);
            const double dbu_y_max = dbu_y_min + height_px / scale;

            // The first grid line at or above `min_value` on a lattice
            // spaced `spacing` apart - std::ceil handles a negative
            // min_value correctly too.
            auto first_line = [](double min_value, int64_t spacing)
            {
                return spacing * static_cast<int64_t>(std::ceil(min_value / static_cast<double>(spacing)));
            };

            auto to_pixel_x = [&](int64_t dbu_x)
            { return static_cast<SkScalar>((static_cast<double>(dbu_x) - static_cast<double>(pan.x)) * scale); };
            auto to_pixel_y = [&](int64_t dbu_y)
            { return static_cast<SkScalar>((static_cast<double>(dbu_y) - static_cast<double>(pan.y)) * scale); };

            if (minor_spacing * scale >= kMinGridDotPixelSpacing)
            {
                SkPaint minor_paint;
                minor_paint.setAntiAlias(true);
                minor_paint.setColor(to_sk_color(kMinorGridColor));
                minor_paint.setStyle(SkPaint::kFill_Style);

                for (int64_t x = first_line(dbu_x_min, minor_spacing); x <= dbu_x_max; x += minor_spacing)
                {
                    for (int64_t y = first_line(dbu_y_min, minor_spacing); y <= dbu_y_max; y += minor_spacing)
                    {
                        if (x % major_spacing == 0 && y % major_spacing == 0)
                            continue; // drawn as a major dot below instead
                        canvas.drawCircle(to_pixel_x(x), to_pixel_y(y), kGridDotRadius, minor_paint);
                    }
                }
            }

            if (major_spacing * scale >= kMinGridDotPixelSpacing)
            {
                SkPaint major_paint;
                major_paint.setAntiAlias(true);
                major_paint.setColor(to_sk_color(kMajorGridColor));
                major_paint.setStyle(SkPaint::kFill_Style);

                for (int64_t x = first_line(dbu_x_min, major_spacing); x <= dbu_x_max; x += major_spacing)
                {
                    for (int64_t y = first_line(dbu_y_min, major_spacing); y <= dbu_y_max; y += major_spacing)
                        canvas.drawCircle(to_pixel_x(x), to_pixel_y(y), kGridDotRadius, major_paint);
                }
            }
        }

        // Draws a fixed on-screen-size "+" cross at the Abstract's own
        // origin point (UPDATES.md 5.4) - not necessarily dbu (0,0); an
        // Abstract's origin is wherever its own LEF ORIGIN statement
        // placed it (AbstractData::origin) - in the same pre-flip pixel
        // space as draw_grid and build_picture's own shapes. Fixed size
        // regardless of Scene::scale, same "marks a reference point, not
        // geometry that should grow with zoom" rationale as
        // kCursorBoxSizePx.
        static void draw_origin_marker(SkCanvas &canvas, const Scene &scene, Point origin_dbu)
        {
            const double scale = scene.scale();
            if (scale <= 0.0)
                return;

            const Point pan = scene.pan();
            const auto cx = static_cast<SkScalar>((static_cast<double>(origin_dbu.x) - static_cast<double>(pan.x)) * scale);
            const auto cy = static_cast<SkScalar>((static_cast<double>(origin_dbu.y) - static_cast<double>(pan.y)) * scale);

            SkPaint paint;
            paint.setAntiAlias(true);
            paint.setStyle(SkPaint::kStroke_Style);
            paint.setStrokeWidth(kOriginMarkerStrokeWidth);
            paint.setColor(to_sk_color(kOriginMarkerColor));

            const SkScalar half = kOriginMarkerSizePx / 2.0f;
            canvas.drawLine(cx - half, cy, cx + half, cy, paint);
            canvas.drawLine(cx, cy - half, cx, cy + half, paint);
        }

        // Draws the grid-snap indicator box (UPDATES.md 5.2) - a red
        // outline of a fixed on-screen size (kCursorBoxSizePx, NOT scaled
        // by Scene::scale - a dbu-sized box would grow/shrink with zoom
        // like a real shape, but this marks a screen position, not
        // geometry) centered on the snapped minor-grid point the mouse is
        // currently over, in the same pre-flip pixel space as draw_grid
        // and build_picture's own shapes. Sized/stroked so the grid dot
        // itself (kGridDotRadius) is visible centered inside the box
        // rather than overdrawn by the stroke. Shown regardless of
        // whether the minor grid tier itself is currently visible
        // (unlike draw_grid's own density floor) - the mouse marker is
        // meant to be visible at all times, not just when the dots
        // happen to be dense enough to draw. No-op only if no mouse
        // position is set (Scene::has_mouse_position).
        static void draw_cursor(SkCanvas &canvas, const Scene &scene)
        {
            const double scale = scene.scale();
            if (scale <= 0.0)
                return;

            const std::optional<Point> snapped = scene.snapped_mouse_position();
            if (!snapped)
                return;

            const Point pan = scene.pan();

            auto to_pixel_x = [&](int64_t dbu_x)
            { return static_cast<SkScalar>((static_cast<double>(dbu_x) - static_cast<double>(pan.x)) * scale); };
            auto to_pixel_y = [&](int64_t dbu_y)
            { return static_cast<SkScalar>((static_cast<double>(dbu_y) - static_cast<double>(pan.y)) * scale); };

            const SkScalar cx = to_pixel_x(snapped->x);
            const SkScalar cy = to_pixel_y(snapped->y);
            const SkScalar half = kCursorBoxSizePx / 2.0f;
            const SkRect rect = SkRect::MakeLTRB(cx - half, cy - half, cx + half, cy + half);

            SkPaint paint;
            paint.setAntiAlias(true);
            paint.setStyle(SkPaint::kStroke_Style);
            paint.setStrokeWidth(kCursorBoxStrokeWidth);
            paint.setColor(to_sk_color(kCursorBoxColor));
            canvas.drawRect(rect, paint);
        }

        // Draws the hover outline (UPDATES.md 7.1 item 1) - a yellow
        // stroke around `hover.outline`'s own geometry, in the same
        // pre-flip pixel space as draw_grid/draw_cursor/draw_origin_marker
        // (not PixelShape's already-transformed space - `hover.outline`
        // is dbu-space, copied straight from a RenderedShape by
        // Pipeline::hit_test_point). Rects/polygons are stroked along
        // their own boundary; paths are stroked along their *buffered*
        // outline (Geometry::path_to_polygons), matching exactly what
        // Geometry::contains itself tested against, so the highlight
        // traces what's actually clickable, not an invisible centerline.
        static void draw_hover_outline(SkCanvas &canvas, const Scene &scene, const HoverTarget &hover)
        {
            const double scale = scene.scale();
            if (scale <= 0.0)
                return;

            const Point pan = scene.pan();
            auto to_pixel = [&](const Point &p)
            {
                return SkPoint::Make(
                    static_cast<SkScalar>((static_cast<double>(p.x) - static_cast<double>(pan.x)) * scale),
                    static_cast<SkScalar>((static_cast<double>(p.y) - static_cast<double>(pan.y)) * scale));
            };

            SkPaint paint;
            paint.setAntiAlias(true);
            paint.setStyle(SkPaint::kStroke_Style);
            paint.setStrokeWidth(kHoverOutlineStrokeWidth);
            paint.setColor(to_sk_color(kHoverOutlineColor));

            auto stroke_polygon = [&](const Polygon &polygon)
            {
                if (polygon.points.empty())
                    return;

                SkPathBuilder builder;
                builder.moveTo(to_pixel(polygon.points.front()));
                for (size_t i = 1; i < polygon.points.size(); ++i)
                    builder.lineTo(to_pixel(polygon.points[i]));
                builder.close();
                canvas.drawPath(builder.detach(), paint);
            };

            for (const auto &rect : hover.outline.rects)
                stroke_polygon(Geometry::rect_to_polygon(rect));

            for (const auto &polygon : hover.outline.polygons)
                stroke_polygon(polygon);

            for (const auto &path : hover.outline.paths)
                for (const auto &buffered : Geometry::path_to_polygons(path))
                    stroke_polygon(buffered);
        }

        // Draws the live rubber-band drag-select rectangle (UPDATES.md
        // 7.1 item 5), in the same pre-flip pixel space as
        // draw_grid/draw_cursor/draw_hover_outline. No-op if no drag is
        // in progress or no mouse position has been set yet (see
        // Scene::drag_rect_dbu) - the same rect le_mouse_up will
        // eventually hit-test against (Pipeline::hit_test_rect), so what
        // the user sees while dragging matches what actually gets
        // selected on release.
        static void draw_drag_rect(SkCanvas &canvas, const Scene &scene)
        {
            const double scale = scene.scale();
            if (scale <= 0.0)
                return;

            const std::optional<Rect> drag_rect = scene.drag_rect_dbu();
            if (!drag_rect)
                return;

            const Point pan = scene.pan();
            auto to_pixel = [&](const Point &p)
            {
                return SkPoint::Make(
                    static_cast<SkScalar>((static_cast<double>(p.x) - static_cast<double>(pan.x)) * scale),
                    static_cast<SkScalar>((static_cast<double>(p.y) - static_cast<double>(pan.y)) * scale));
            };

            const SkRect rect = SkRect::MakeLTRB(
                to_pixel(drag_rect->ll).x(), to_pixel(drag_rect->ll).y(),
                to_pixel(drag_rect->ur).x(), to_pixel(drag_rect->ur).y());

            SkPaint fill;
            fill.setAntiAlias(true);
            fill.setStyle(SkPaint::kFill_Style);
            fill.setColor(to_sk_color(kDragRectFillColor));
            canvas.drawRect(rect, fill);

            SkPaint stroke;
            stroke.setAntiAlias(true);
            stroke.setStyle(SkPaint::kStroke_Style);
            stroke.setStrokeWidth(kDragRectStrokeWidth);
            stroke.setColor(to_sk_color(kDragRectStrokeColor));
            canvas.drawRect(rect, stroke);
        }

        // Draws an X spanning `bounds` - CUT layers' FillPattern::CROSS,
        // drawn directly rather than tiled since CUT geometry (vias) is
        // almost always one small rect per shape, not a large area a
        // repeating texture would suit (see also pattern_shader's comment).
        static void draw_cross(SkCanvas &canvas, const SkRect &bounds, const SkPaint &paint)
        {
            canvas.drawLine(bounds.left(), bounds.top(), bounds.right(), bounds.bottom(), paint);
            canvas.drawLine(bounds.left(), bounds.bottom(), bounds.right(), bounds.top(), paint);
        }

        // Paint/font construction hoisted out of the per-shape loop - one
        // ViewLayerStyle applies to every shape in the group.
        static void draw_group(SkCanvas &canvas, const std::vector<PixelShape> &group, const ViewLayerStyle &style)
        {
            const bool has_fill = style.fill_color.a > 0;
            const bool has_outline = style.outline_color.a > 0;
            const bool is_cross = style.fill_pattern == FillPattern::CROSS;

            SkPaint fill;
            fill.setAntiAlias(true);
            fill.setStyle(SkPaint::kFill_Style);
            if (sk_sp<SkShader> shader = pattern_shader(style.fill_pattern, to_sk_color(style.outline_color)))
            {
                // A paint's alpha still modulates its shader's own output
                // alpha even though its RGB is ignored - leaving fill_color
                // (translucent, alpha ~100) as this paint's color would
                // silently wash out every already-opaque pattern pixel to
                // ~40% opacity. Full alpha here so the tile's own baked-in
                // alpha (opaque pattern, transparent gaps) passes through
                // unmodulated.
                fill.setShader(std::move(shader));
                fill.setAlphaf(1.0f);
            }
            else
            {
                fill.setColor(to_sk_color(style.fill_color));
            }

            SkPaint stroke;
            stroke.setAntiAlias(true);
            stroke.setStyle(SkPaint::kStroke_Style);
            stroke.setColor(to_sk_color(style.outline_color));

            // Labels use the outline color (always opaque in every default
            // ViewLayerStyle, unlike fill) - there's no dedicated label
            // color yet, revisit if that turns out to matter visually.
            // Font is the one exception to this class's "hoist paint/font
            // out of the per-shape loop" rule above: since each PixelText
            // now carries its own computed size, the font has to be built
            // per-label instead of once per group (see the text loop
            // below) - not a hot path, text labels are a small minority of
            // draw calls relative to shapes/rects/paths, so the extra
            // per-label SkFont construction is a non-issue.
            SkPaint text_paint;
            text_paint.setAntiAlias(true);
            text_paint.setColor(to_sk_color(style.outline_color));

            for (const auto &shape : group)
            {
                for (const auto &r : shape.rects)
                {
                    SkRect rect = SkRect::MakeLTRB(static_cast<SkScalar>(r.ll.x), static_cast<SkScalar>(r.ll.y),
                                                   static_cast<SkScalar>(r.ur.x), static_cast<SkScalar>(r.ur.y));
                    if (is_cross)
                    {
                        if (has_outline)
                            draw_cross(canvas, rect, stroke);
                    }
                    else if (has_fill)
                        canvas.drawRect(rect, fill);
                    if (has_outline)
                        canvas.drawRect(rect, stroke);
                }

                for (const auto &poly : shape.polygons)
                {
                    SkPath path = to_sk_path(poly, /*close=*/true);
                    if (is_cross)
                    {
                        if (has_outline)
                            draw_cross(canvas, path.getBounds(), stroke);
                    }
                    else if (has_fill)
                        canvas.drawPath(path, fill);
                    if (has_outline)
                        canvas.drawPath(path, stroke);
                }

                // Paths are wire centerlines with a width, not filled
                // polygons - drawn as a solid stroked line using the fill
                // color (the layer's "main" color), not the outline color.
                if (has_fill)
                {
                    SkPaint wire = fill;
                    wire.setStyle(SkPaint::kStroke_Style);
                    wire.setStrokeCap(SkPaint::kButt_Cap);
                    for (const auto &p : shape.paths)
                    {
                        wire.setStrokeWidth(static_cast<SkScalar>(p.width));
                        canvas.drawPath(to_sk_path(p.polygon, /*close=*/false), wire);
                    }
                }

                if (has_outline)
                {
                    // rasterize() applies a whole-canvas Y-flip on top of
                    // this picture (see its own comment) so shape geometry
                    // ends up correctly oriented - but that same flip would
                    // also mirror glyph rendering upside-down, since Skia
                    // has no notion that text is directionally special.
                    // Counter-flip locally around each label's own anchor
                    // point so the two cancel out and glyphs stay upright;
                    // the label's position still moves with the whole-canvas
                    // flip (translate happens first, at the untouched
                    // anchor coordinates), only its own rendering doesn't.
                    // This does mean this SkPicture's text only renders
                    // right-side-up when drawn through rasterize()'s flip -
                    // fine today since that's the only consumer, but a
                    // future direct-to-canvas consumer would need the same
                    // whole-canvas flip applied for text to still be upright.
                    for (const auto &t : shape.texts)
                    {
                        canvas.save();
                        canvas.translate(static_cast<SkScalar>(t.location.x), static_cast<SkScalar>(t.location.y));
                        canvas.scale(1, -1);
                        SkFont font(default_typeface(), static_cast<SkScalar>(t.size));
                        canvas.drawString(t.label.c_str(), 0, 0, font, text_paint);
                        canvas.restore();
                    }
                }
            }
        }

        // Draws a white outline around every piece of `shape` (UPDATES.md
        // 7) - rects/polygons stroked along their own boundary; paths
        // stroked along their centerline at their own width plus a margin
        // on each side (kSelectionPathHaloMarginPx), so a selected wire
        // reads as highlighted rather than simply recolored white. Called
        // once per selected PixelShape, in a pass that runs after every
        // group has already been drawn (see build_picture) so the
        // outline is never obscured by a shape on a higher layer.
        static void draw_selection_outline(SkCanvas &canvas, const PixelShape &shape)
        {
            SkPaint stroke;
            stroke.setAntiAlias(true);
            stroke.setStyle(SkPaint::kStroke_Style);
            stroke.setStrokeWidth(kSelectionOutlineStrokeWidth);
            stroke.setColor(to_sk_color(kSelectionOutlineColor));

            for (const auto &r : shape.rects)
            {
                const SkRect rect = SkRect::MakeLTRB(static_cast<SkScalar>(r.ll.x), static_cast<SkScalar>(r.ll.y),
                                                      static_cast<SkScalar>(r.ur.x), static_cast<SkScalar>(r.ur.y));
                canvas.drawRect(rect, stroke);
            }

            for (const auto &poly : shape.polygons)
                canvas.drawPath(to_sk_path(poly, /*close=*/true), stroke);

            if (!shape.paths.empty())
            {
                SkPaint halo;
                halo.setAntiAlias(true);
                halo.setStyle(SkPaint::kStroke_Style);
                halo.setStrokeCap(SkPaint::kButt_Cap);
                halo.setColor(to_sk_color(kSelectionOutlineColor));

                for (const auto &p : shape.paths)
                {
                    halo.setStrokeWidth(static_cast<SkScalar>(p.width) + 2.0f * kSelectionPathHaloMarginPx);
                    canvas.drawPath(to_sk_path(p.polygon, /*close=*/false), halo);
                }
            }
        }

        // Bundles the PixelBuffer view together with the raster surface
        // that owns its backing memory, so the CachedStage below keeps
        // that memory alive for as long as the cache entry is valid -
        // PixelBuffer itself holds no ownership, just a view into this.
        // Also what makes compose_with_overlays's cheap image-snapshot reuse
        // possible - the surface has to still be alive to snapshot it.
        struct RasterizedFrame
        {
            sk_sp<SkSurface> surface;
            PixelBuffer buffer;
        };

        /// @brief The actual rasterize implementation - `rasterize()` and
        /// `compose_with_overlays()` are both thin callers, the latter using
        /// the returned RasterizedFrame's `surface` directly (not just its
        /// `buffer`) to snapshot it cheaply rather than re-drawing
        /// `picture`. dbu-space y increases upward (physical layout
        /// convention); PixelShape's own transform maps that straight
        /// through with no flip, so without correction here, increasing
        /// dbu y would end up increasing *pixel row index* - i.e. the
        /// design's "up" would render toward the bottom of the buffer,
        /// since Skia's own canvas is y-down. Corrected with one
        /// canvas-level flip (`translate` + `scale(1,-1)`) applied once
        /// before drawing the whole picture, not by changing
        /// `transform_to_pixels`'s per-shape math or reversing output rows
        /// after the fact - cheapest place to do it, and keeps that
        /// already-tested transform simple. A whole-canvas flip mirrors
        /// glyph rendering too, though (Skia has no notion that text is
        /// directionally special) - `draw_group`'s text-drawing loop
        /// counter-flips locally around each label's own anchor so glyphs
        /// stay upright under this; see its own comment for why that
        /// couples `build_picture`'s `SkPicture` to being drawn through
        /// this specific flip to render text right-side-up.
        ///
        /// Uses an explicit `kRGBA_8888_SkColorType` raster surface, not
        /// `SkImageInfo::MakeN32Premul` (which picks Skia's *platform*-
        /// native `kN32_SkColorType` - BGRA on some platforms, RGBA on
        /// others): this project develops on macOS but targets Linux
        /// servers, and a pixel buffer meant to eventually cross into
        /// Flutter needs the same byte layout on both, not whatever the
        /// build machine's native order happens to be. Clears to fully
        /// transparent first - the real "nothing drawn here" state for
        /// content meant to be composited into a Flutter texture.
        ///
        /// The returned RasterizedFrame's `buffer.data` points into
        /// `surface` - valid until the next call that invalidates this
        /// cache entry, same convention as `build_picture`'s returned
        /// `sk_sp<SkPicture>&`. `peekPixels` is unchecked here (unlike
        /// e.g. render_preview.cpp's defensive check): it's documented to
        /// always succeed for a surface this function itself just created
        /// via `SkSurfaces::Raster`.
        ///
        /// Cache key is `{AbstractId, viewport_version, visibility_version,
        /// selection_version}` - every one of `picture`'s own upstream
        /// triggers (`build_picture`'s cache key exactly). This has to be
        /// kept in sync with `build_picture`'s key by hand: CachedStage's
        /// `get` only ever compares the key tuple, never `picture` itself,
        /// so if `picture`'s content can change for a reason this key
        /// doesn't capture, this returns a stale frame for a *different*
        /// picture than the one just passed in - exactly what happened
        /// before `selection_version` was added here (build_picture
        /// correctly recomputed a new SkPicture with the selection
        /// outline baked in, but this cache still matched on the old key
        /// and returned the pre-selection rasterized bytes).
        const RasterizedFrame &rasterize_frame(const sk_sp<SkPicture> &picture, const Scene &scene)
        {
            const auto key = std::tuple{scene.current_abstract(), scene.viewport_version(), scene.visibility_version(), scene.selection_version()};
            return rasterized_.get(key, [&]
                                   {
                const int width = scene.viewport_width_px();
                const int height = scene.viewport_height_px();

                // SkSurfaces::Raster returns null for non-positive
                // dimensions (Scene's default-constructed viewport size,
                // or simply not having called set_viewport_size() yet) -
                // an empty (all-null/zero) PixelBuffer rather than a null
                // surface->getCanvas() dereference, matching this
                // project's "degrade gracefully rather than crash on
                // unset/invalid state" convention elsewhere (e.g. an
                // unknown AbstractId in Pipeline::generate_shapes).
                if (width <= 0 || height <= 0)
                    return RasterizedFrame{};

                const SkImageInfo info = SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
                sk_sp<SkSurface> surface = SkSurfaces::Raster(info);

                SkCanvas *canvas = surface->getCanvas();
                canvas->clear(SK_ColorTRANSPARENT);
                canvas->translate(0, static_cast<SkScalar>(height));
                canvas->scale(1, -1);
                canvas->drawPicture(picture);

                SkPixmap pixmap;
                surface->peekPixels(&pixmap);

                return RasterizedFrame{
                    .surface = std::move(surface),
                    .buffer = PixelBuffer{
                        .data = static_cast<const uint8_t *>(pixmap.addr()),
                        .width = width,
                        .height = height,
                        .row_bytes = pixmap.rowBytes(),
                    },
                }; });
        }

        CachedStage<std::tuple<AbstractId, uint64_t, uint64_t>, std::map<ViewLayerId, std::vector<PixelShape>>> pixel_shapes_;
        CachedStage<std::tuple<AbstractId, uint64_t, uint64_t, uint64_t>, sk_sp<SkPicture>> picture_;
        CachedStage<std::tuple<uint64_t, uint64_t, uint64_t>, sk_sp<SkPicture>> overlay_picture_;
        CachedStage<std::tuple<AbstractId, uint64_t, uint64_t, uint64_t>, RasterizedFrame> rasterized_;
        CachedStage<std::tuple<AbstractId, uint64_t, uint64_t, uint64_t, uint64_t>, RasterizedFrame> composed_;
    };
}
