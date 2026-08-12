#pragma once
#include "pixel_types.hpp"
#include "../database/database.hpp"
#include "../geometry/geometry.hpp"
#include "../scene/scene.hpp"
#include "../view_style/view_style.hpp"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkFont.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkRect.h"
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkShader.h"
#include "include/core/SkSurface.h"
#include "include/core/SkTileMode.h"
#include "include/core/SkTypeface.h"
#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

/// @brief Style constants and free Skia drawing helper functions shared by
/// several of Renderer's stage classes (BuildPictureStage,
/// BuildOverlayPictureStage, BuildSelectionOverlayPictureStage) - these
/// were `private static` members of the old monolithic `Renderer` class;
/// moved out to namespace scope so more than one stage class can use them
/// without depending on `Renderer` itself.
namespace le
{
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
    sk_sp<SkTypeface> default_typeface();

    inline SkColor to_sk_color(Color c) { return SkColorSetARGB(c.a, c.r, c.g, c.b); }

    inline SkPath to_sk_path(const PixelPolygon &poly, bool close)
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
    inline constexpr int kPatternTileSize = 12;

    // Spacing between stripes, in screen pixels.
    inline constexpr SkScalar kDiagonalStripePeriod = 8.0f;

    // SkShader's kRepeat tiling only ever translates by exact multiples
    // of the tile's own size - so for the "redundant offset lines,
    // let the canvas clip them" technique below to reconstruct a
    // truly continuous periodic hatch (not a phase-shifted zigzag
    // between tiles), the tile size *must* be an exact multiple of
    // kDiagonalStripePeriod. 3x gives a reasonable amount of visible
    // repetition per tile without an oversized offscreen surface.
    // Don't change kDiagonalStripePeriod without keeping this in sync.
    inline constexpr int kDiagonalStripeTileSize = static_cast<int>(kDiagonalStripePeriod) * 3;

    // Minimum on-screen text size in pixels regardless of how thin the
    // labeled geometry is - keeps labels legible at any zoom level
    // instead of shrinking to unreadable specks on hair-thin paths/
    // polygon arms. Placeholder default, easily tuned.
    inline constexpr double kMinLabelPixelSize = 8.0;

    // Fraction of the local geometry width actually used for text
    // size, so a label doesn't touch/overflow the edges of the shape
    // it's on. Placeholder default, easily tuned.
    inline constexpr double kLabelWidthRatio = 0.6;

    // Below this on-screen pixel spacing, a grid dot tier (minor or
    // major, checked independently) is hidden entirely rather than
    // smearing into a solid wash as the view zooms out - see
    // draw_grid's own comment. Placeholder default, easily tuned.
    inline constexpr double kMinGridDotPixelSpacing = 8.0;

    // Major and minor dots are drawn the same size - only kMajorGridColor's
    // brightness distinguishes the two tiers.
    inline constexpr float kGridDotRadius = 1.0f;

    // Grid dots/axis lines are UI chrome, not design geometry -
    // deliberately muted/neutral so they don't compete visually with
    // real shapes; kMajorGridColor is brighter/more opaque than
    // kMinorGridColor so the major tier still reads as bolder despite
    // being drawn at the same radius. Placeholder defaults, easily tuned.
    inline constexpr Color kMinorGridColor = {128, 128, 128, 120};
    inline constexpr Color kMajorGridColor = {255, 255, 255, 230};
    inline constexpr Color kAxisLineColor = {255, 255, 255, 160};

    // Origin marker (UPDATES.md 5.4) - fully opaque, and a color
    // distinct from both the grid (gray/white) and the cursor box
    // (red) so it isn't confused with either. Fixed on-screen size
    // regardless of zoom, same rationale as kCursorBoxSizePx.
    inline constexpr Color kOriginMarkerColor = {255, 200, 0, 255};
    inline constexpr float kOriginMarkerStrokeWidth = 2.0f;
    inline constexpr float kOriginMarkerSizePx = 16.0f;

    // Small marker at each label's own anchor point (UPDATES.md item
    // 8.3) - a large label can overlap the shape it's labeling, so
    // this pins down exactly which point get_label_location chose.
    // Distinct from kOriginMarkerSizePx's own abstract-origin marker
    // (one global reference point, fixed color) - this is per-label
    // and drawn in the label's own layer color (see draw_group), not
    // a new global color, so it visually reads as "belonging to"
    // that label - color, not size, is what keeps the two from being
    // confused. Placeholder defaults, easily tuned.
    inline constexpr float kLabelOriginMarkerSizePx = 16.0f;
    inline constexpr float kLabelOriginMarkerStrokeWidth = 2.0f;

    // Grid-snap indicator box (UPDATES.md 5.2) - fully opaque so it
    // stays visible over any layer color/pattern underneath. Fixed
    // on-screen size regardless of zoom (see draw_cursor) - 7x7px
    // with a 1px stroke leaves a ~6px transparent interior, comfortably
    // enough for the grid dot itself (kGridDotRadius, 2px diameter) to
    // show through centered inside the box rather than being overdrawn
    // by the stroke.
    inline constexpr Color kCursorBoxColor = {255, 0, 0, 255};
    inline constexpr float kCursorBoxStrokeWidth = 1.0f;
    inline constexpr float kCursorBoxSizePx = 7.0f;

    // Hover outline (UPDATES.md 7.1 item 1) - opaque yellow, distinct
    // from the grid/origin marker/cursor box colors above. Traces the
    // actual geometry of the hovered piece (see draw_hover_outline),
    // so unlike the cursor box/origin marker this isn't a fixed
    // on-screen size - it scales with the shape like real geometry.
    inline constexpr Color kHoverOutlineColor = {255, 255, 0, 255};
    inline constexpr float kHoverOutlineStrokeWidth = 2.0f;

    // Selection outline (UPDATES.md 7) - opaque white, distinct from
    // every other overlay color above (and from every layer's own
    // default palette color, none of which are pure white). Every
    // reachable selection (click or drag - see Pipeline::hit_test_rect)
    // always records a specific piece, so draw_selected_piece_outline
    // is the only consumer of these constants.
    inline constexpr Color kSelectionOutlineColor = {255, 255, 255, 255};
    inline constexpr float kSelectionOutlineStrokeWidth = 2.0f;

    // Rubber-band drag-select rectangle (UPDATES.md 7.1 item 5) - a
    // translucent fill so covered shapes stay visible underneath, plus
    // a solid stroke for a crisp edge. Blue, a color family not
    // already used by the grid/origin marker/cursor box/hover outline
    // above.
    inline constexpr Color kDragRectFillColor = {80, 160, 255, 60};
    inline constexpr Color kDragRectStrokeColor = {80, 160, 255, 220};
    inline constexpr float kDragRectStrokeWidth = 2.0f;

    // Rectangle-zoom drag (UPDATES.md 9.3) - same translucent-fill +
    // solid-stroke style as the select-drag rectangle above, but a
    // different color family (green, not yet used by any other
    // overlay) so the two gestures read as visually distinct before
    // release, not just after - draw_drag_rect picks between the two
    // based on Scene::drag_kind().
    inline constexpr Color kZoomDragRectFillColor = {80, 255, 160, 60};
    inline constexpr Color kZoomDragRectStrokeColor = {80, 255, 160, 220};

    // Renders one FillPattern into a small transparent-background tile
    // and wraps it in a kRepeat/kRepeat SkShader - this project's
    // answer to "maybe using a shader?" (UPDATES.md 2.3): Skia's
    // SkShader tiling works entirely on a CPU raster surface, no GPU
    // involved, so it fits this project's no-GPU target directly.
    // FillPattern::NONE and ::CROSS return null - NONE draws as today's
    // flat color (no shader needed) and CROSS is drawn directly against
    // each shape's own bounds in draw_group instead of tiled (see its
    // own comment for why a repeating tile is the wrong shape for it).
    inline sk_sp<SkShader> pattern_shader(FillPattern pattern, SkColor color)
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
    // this draws that isn't derived from pipeline output) plus
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
    inline void draw_grid(SkCanvas &canvas, const Scene &scene)
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
    inline void draw_origin_marker(SkCanvas &canvas, const Scene &scene, Point origin_dbu)
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
    inline void draw_cursor(SkCanvas &canvas, const Scene &scene)
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
    inline void draw_hover_outline(SkCanvas &canvas, const Scene &scene, const HoverTarget &hover)
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

    // Draws a white outline (UPDATES.md 7) around `piece`'s own
    // geometry - one selected object's specific clicked/dragged piece
    // (see Scene::SelectedObject), not the whole Terminal/
    // Obstruction's combined geometry. Same dbu->pixel transform *and*
    // same path treatment as draw_hover_outline (`piece` is dbu-space,
    // copied straight from Pipeline::hit_test_point/hit_test_rect's
    // own HoverTarget::outline) - a path traces its *buffered outline
    // polygon* (Geometry::path_to_polygons), not a halo stroke along
    // its own centerline - a halo reads fine for a real wire (long
    // relative to its width, so it looks like a thin glow), but
    // collapses into a solid-looking blob for a path whose width is
    // comparable to its own length - not hypothetical, a real
    // reported bug against synthetic stress-test geometry shaped
    // exactly like that.
    inline void draw_selected_piece_outline(SkCanvas &canvas, const Scene &scene, const Shape &piece)
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

        SkPaint stroke;
        stroke.setAntiAlias(true);
        stroke.setStyle(SkPaint::kStroke_Style);
        stroke.setStrokeWidth(kSelectionOutlineStrokeWidth);
        stroke.setColor(to_sk_color(kSelectionOutlineColor));

        auto stroke_polygon = [&](const Polygon &polygon)
        {
            if (polygon.points.empty())
                return;

            SkPathBuilder builder;
            builder.moveTo(to_pixel(polygon.points.front()));
            for (size_t i = 1; i < polygon.points.size(); ++i)
                builder.lineTo(to_pixel(polygon.points[i]));
            builder.close();
            canvas.drawPath(builder.detach(), stroke);
        };

        for (const auto &rect : piece.rects)
            stroke_polygon(Geometry::rect_to_polygon(rect));

        for (const auto &polygon : piece.polygons)
            stroke_polygon(polygon);

        for (const auto &path : piece.paths)
            for (const auto &buffered : Geometry::path_to_polygons(path))
                stroke_polygon(buffered);
    }

    // Draws the live rubber-band drag rectangle - drag-select
    // (UPDATES.md 7.1 item 5) or drag-zoom (UPDATES.md 9.3),
    // distinguished by color via Scene::drag_kind() - in the same
    // pre-flip pixel space as draw_grid/draw_cursor/
    // draw_hover_outline. No-op if no drag is in progress or no
    // mouse position has been set yet (see Scene::drag_rect_dbu) -
    // for a select-drag, the same rect le_mouse_up will eventually
    // hit-test against (Pipeline::hit_test_rect), so what the user
    // sees while dragging matches what actually gets selected on
    // release; for a zoom-drag, the same rect le_mouse_up will fit
    // the viewport to (Scene::fit_to_content).
    inline void draw_drag_rect(SkCanvas &canvas, const Scene &scene)
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

        const bool is_zoom = scene.drag_kind() == Scene::DragKind::ZOOM;

        SkPaint fill;
        fill.setAntiAlias(true);
        fill.setStyle(SkPaint::kFill_Style);
        fill.setColor(to_sk_color(is_zoom ? kZoomDragRectFillColor : kDragRectFillColor));
        canvas.drawRect(rect, fill);

        SkPaint stroke;
        stroke.setAntiAlias(true);
        stroke.setStyle(SkPaint::kStroke_Style);
        stroke.setStrokeWidth(kDragRectStrokeWidth);
        stroke.setColor(to_sk_color(is_zoom ? kZoomDragRectStrokeColor : kDragRectStrokeColor));
        canvas.drawRect(rect, stroke);
    }

    // Draws an X spanning `bounds` - CUT layers' FillPattern::CROSS,
    // drawn directly rather than tiled since CUT geometry (vias) is
    // almost always one small rect per shape, not a large area a
    // repeating texture would suit (see also pattern_shader's comment).
    inline void draw_cross(SkCanvas &canvas, const SkRect &bounds, const SkPaint &paint)
    {
        canvas.drawLine(bounds.left(), bounds.top(), bounds.right(), bounds.bottom(), paint);
        canvas.drawLine(bounds.left(), bounds.bottom(), bounds.right(), bounds.top(), paint);
    }

    // Paint/font construction hoisted out of the per-shape loop - one
    // ViewLayerStyle applies to every shape in the group.
    inline void draw_group(SkCanvas &canvas, const std::vector<PixelShape> &group, const ViewLayerStyle &style)
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

        // UPDATES.md item 8.3 - a small cross at each label's own
        // anchor point (see the text loop below), same color as the
        // label text itself and hoisted the same way for the same
        // reason.
        SkPaint label_origin_paint;
        label_origin_paint.setAntiAlias(true);
        label_origin_paint.setStyle(SkPaint::kStroke_Style);
        label_origin_paint.setStrokeWidth(kLabelOriginMarkerStrokeWidth);
        label_origin_paint.setColor(to_sk_color(style.outline_color));

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

            // A path's buffered_outline (PixelPath, transformed from
            // RenderedShape::path_outlines - computed once at
            // Pipeline::generate_shapes time, not here) is filled and
            // outlined exactly like a real PixelPolygon just above -
            // fill first (the layer's real pattern, not a solid
            // stroke), then a thin outline-colored boundary. A wide
            // solid stroke used to be drawn as a "border" directly
            // underneath a pattern-shaded stroke at the path's own
            // width - since both used the same base color (a layer's
            // outline_color is also pattern_shader's tile color), that
            // border acted as an opaque same-color backing plate
            // showing straight through every transparent gap in the
            // pattern, so a PATH always read as one solid block
            // regardless of its layer's fill pattern (see
            // BENCHMARKS.md). Then a thin centerline stroke along the
            // path's own original polygon, so it still reads as a
            // wire rather than just another filled/outlined shape -
            // reuses `stroke` (same hairline width/color as the
            // boundary) rather than a new named color.
            for (const auto &p : shape.paths)
            {
                if (has_fill)
                    for (const auto &poly : p.buffered_outline)
                        canvas.drawPath(to_sk_path(poly, /*close=*/true), fill);

                if (has_outline)
                {
                    for (const auto &poly : p.buffered_outline)
                        canvas.drawPath(to_sk_path(poly, /*close=*/true), stroke);

                    canvas.drawPath(to_sk_path(p.polygon, /*close=*/false), stroke);
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

                    // A "+" is symmetric under the y-flip scale(1,-1)
                    // just applied, so unlike the glyphs below, this
                    // doesn't need to counter-flip anything itself -
                    // drawn in the same local, already-translated
                    // coordinate system so it tracks the label exactly.
                    const SkScalar half = kLabelOriginMarkerSizePx / 2.0f;
                    canvas.drawLine(-half, 0, half, 0, label_origin_paint);
                    canvas.drawLine(0, -half, 0, half, label_origin_paint);

                    SkFont font(default_typeface(), static_cast<SkScalar>(t.size));
                    canvas.drawString(t.label.c_str(), 0, 0, font, text_paint);
                    canvas.restore();
                }
            }
        }
    }
}
