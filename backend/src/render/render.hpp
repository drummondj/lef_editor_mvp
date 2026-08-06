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
#include <cstdint>
#include <map>
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
                            ps.texts.push_back(PixelText{.label = t.label, .location = to_pixel(t.location)});

                        pixel_group.push_back(std::move(ps));
                    }

                    result.emplace(view_layer, std::move(pixel_group));
                }

                return result; });
        }

        /// @brief Record the pixel-space shapes into an SkPicture, sized to
        /// the Scene's viewport, drawn in map order (bottom-up, see
        /// Pipeline::filter_by_layer_visibility's comment). Each group's
        /// ViewLayerStyle (outline/fill Color) comes from `view_layers`; a
        /// ViewLayerId that doesn't resolve to a known ViewLayer (see
        /// Pipeline's layer filter comment on why that's kept, not dropped)
        /// has its whole group skipped here - there's no style to draw it
        /// with.
        const sk_sp<SkPicture> &build_picture(const std::map<ViewLayerId, std::vector<PixelShape>> &shapes, const Scene &scene, const ViewLayerSet &view_layers)
        {
            const auto key = std::tuple{scene.current_abstract(), scene.viewport_version(), scene.visibility_version()};
            return picture_.get(key, [&]
                                {
                SkPictureRecorder recorder;
                SkCanvas *canvas = recorder.beginRecording(
                    SkRect::MakeWH(static_cast<SkScalar>(scene.viewport_width_px()), static_cast<SkScalar>(scene.viewport_height_px())));

                for (const auto &[view_layer_id, group] : shapes)
                {
                    const ViewLayerData *view_layer = view_layers.get(view_layer_id);
                    if (!view_layer)
                        continue;

                    draw_group(*canvas, group, view_layer->style);
                }

                return recorder.finishRecordingAsPicture(); });
        }

        /// @brief Rasterize `picture` into a raw RGBA8888 PixelBuffer sized
        /// to the Scene's viewport - the step this project's README's open
        /// design questions flagged the Y-axis-flip decision as belonging
        /// to (not `transform_to_pixels`, which stays a pure, unflipped
        /// dbu->pixel map either way). dbu-space y increases upward
        /// (physical layout convention); PixelShape's own transform maps
        /// that straight through with no flip, so without correction here,
        /// increasing dbu y would end up increasing *pixel row index* -
        /// i.e. the design's "up" would render toward the bottom of the
        /// buffer, since Skia's own canvas is y-down. Corrected with one
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
        /// The returned PixelBuffer's `data` points into the backing
        /// raster surface kept alive inside this cache entry (no extra
        /// copy) - valid until the next call that invalidates it, same
        /// convention as `build_picture`'s returned `sk_sp<SkPicture>&`.
        /// `peekPixels` is unchecked here (unlike e.g. render_preview.cpp's
        /// defensive check): it's documented to always succeed for a
        /// surface this function itself just created via `SkSurfaces::Raster`.
        const PixelBuffer &rasterize(const sk_sp<SkPicture> &picture, const Scene &scene)
        {
            const auto key = std::tuple{scene.current_abstract(), scene.viewport_version(), scene.visibility_version()};
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
                }; })
                .buffer;
        }

        // Number of times each stage actually recomputed - exposed purely
        // to make cache hits/misses observable in tests.
        uint64_t transform_calls() const { return pixel_shapes_.call_count(); }
        uint64_t picture_calls() const { return picture_.call_count(); }
        uint64_t rasterize_calls() const { return rasterized_.call_count(); }

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
            SkFont font(default_typeface(), 24.0f);
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
                        canvas.drawString(t.label.c_str(), 0, 0, font, text_paint);
                        canvas.restore();
                    }
                }
            }
        }

        // Bundles the PixelBuffer view together with the raster surface
        // that owns its backing memory, so the CachedStage below keeps
        // that memory alive for as long as the cache entry is valid -
        // PixelBuffer itself holds no ownership, just a view into this.
        struct RasterizedFrame
        {
            sk_sp<SkSurface> surface;
            PixelBuffer buffer;
        };

        CachedStage<std::tuple<AbstractId, uint64_t, uint64_t>, std::map<ViewLayerId, std::vector<PixelShape>>> pixel_shapes_;
        CachedStage<std::tuple<AbstractId, uint64_t, uint64_t>, sk_sp<SkPicture>> picture_;
        CachedStage<std::tuple<AbstractId, uint64_t, uint64_t>, RasterizedFrame> rasterized_;
    };
}
