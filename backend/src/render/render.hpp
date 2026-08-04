#pragma once
#include "../pipeline/pipeline.hpp"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkFont.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPicture.h"
#include "include/core/SkPictureRecorder.h"
#include "include/core/SkRect.h"
#include "include/core/SkSurface.h"
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

                return result;
            });
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

                return recorder.finishRecordingAsPicture();
            });
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
                };
            }).buffer;
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

        // Paint/font construction hoisted out of the per-shape loop - one
        // ViewLayerStyle applies to every shape in the group.
        static void draw_group(SkCanvas &canvas, const std::vector<PixelShape> &group, const ViewLayerStyle &style)
        {
            const bool has_fill = style.fill_color.a > 0;
            const bool has_outline = style.outline_color.a > 0;

            SkPaint fill;
            fill.setAntiAlias(true);
            fill.setStyle(SkPaint::kFill_Style);
            fill.setColor(to_sk_color(style.fill_color));

            SkPaint stroke;
            stroke.setAntiAlias(true);
            stroke.setStyle(SkPaint::kStroke_Style);
            stroke.setColor(to_sk_color(style.outline_color));

            // Labels use the outline color (always opaque in every default
            // ViewLayerStyle, unlike fill) - there's no dedicated label
            // color yet, revisit if that turns out to matter visually.
            SkFont font(default_typeface(), 12.0f);
            SkPaint text_paint;
            text_paint.setAntiAlias(true);
            text_paint.setColor(to_sk_color(style.outline_color));

            for (const auto &shape : group)
            {
                for (const auto &r : shape.rects)
                {
                    SkRect rect = SkRect::MakeLTRB(static_cast<SkScalar>(r.ll.x), static_cast<SkScalar>(r.ll.y),
                                                    static_cast<SkScalar>(r.ur.x), static_cast<SkScalar>(r.ur.y));
                    if (has_fill)
                        canvas.drawRect(rect, fill);
                    if (has_outline)
                        canvas.drawRect(rect, stroke);
                }

                for (const auto &poly : shape.polygons)
                {
                    SkPath path = to_sk_path(poly, /*close=*/true);
                    if (has_fill)
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
