#pragma once
#include "../pipeline/pipeline.hpp"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkPicture.h"
#include "include/core/SkPictureRecorder.h"
#include "include/core/SkRect.h"
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

    /// @brief A Shape transformed from dbu-space to pixel-space (Scene's
    /// `pixel = (dbu - pan) * scale`), mirroring Shape's own
    /// rects/polygons/paths structure with double coordinates instead of
    /// Rect/Polygon/Path's integer dbu ones. No Y-axis flip is applied here
    /// - whether pixel space should be flipped for a particular screen/
    /// texture convention is a later, unresolved concern (e.g. when
    /// actually blitting to a Flutter texture), not decided by this
    /// transform. Text isn't transformed yet - same scope boundary as
    /// Pipeline's viewport filter (see its comment): text rendering needs
    /// its own handling later.
    struct PixelShape
    {
        std::vector<PixelRect> rects;
        std::vector<PixelPolygon> polygons;
        std::vector<PixelPath> paths;
        ViewLayerId view_layer;
    };

    /// @brief Transforms Pipeline's filtered dbu-space output into pixel
    /// space, then records it into an SkPicture via Skia draw calls. Two
    /// more CachedStage-backed stages (see pipeline.hpp), keyed the same
    /// way Pipeline::filter_by_layer_visibility's output already is -
    /// AbstractId + Scene::viewport_version() + Scene::visibility_version()
    /// already cover everything the pixel transform depends on (pan/scale/
    /// viewport-size all bump viewport_version()).
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
        const std::vector<PixelShape> &transform_to_pixels(const std::vector<RenderedShape> &shapes, const Scene &scene)
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

                std::vector<PixelShape> result;
                result.reserve(shapes.size());

                for (const auto &rs : shapes)
                {
                    PixelShape ps{.view_layer = rs.view_layer};

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

                    result.push_back(std::move(ps));
                }

                return result;
            });
        }

        /// @brief Record the pixel-space shapes into an SkPicture, sized to
        /// the Scene's viewport. Each shape's ViewLayerStyle (outline/fill
        /// Color) comes from `view_layers`; a shape whose ViewLayerId
        /// doesn't resolve to a known ViewLayer (see Pipeline's layer
        /// filter comment on why that's kept, not dropped) is skipped here
        /// - there's no style to draw it with.
        const sk_sp<SkPicture> &build_picture(const std::vector<PixelShape> &shapes, const Scene &scene, const ViewLayerSet &view_layers)
        {
            const auto key = std::tuple{scene.current_abstract(), scene.viewport_version(), scene.visibility_version()};
            return picture_.get(key, [&]
            {
                SkPictureRecorder recorder;
                SkCanvas *canvas = recorder.beginRecording(
                    SkRect::MakeWH(static_cast<SkScalar>(scene.viewport_width_px()), static_cast<SkScalar>(scene.viewport_height_px())));

                for (const auto &ps : shapes)
                {
                    const ViewLayerData *view_layer = view_layers.get(ps.view_layer);
                    if (!view_layer)
                        continue;

                    draw_shape(*canvas, ps, view_layer->style);
                }

                return recorder.finishRecordingAsPicture();
            });
        }

        // Number of times each stage actually recomputed - exposed purely
        // to make cache hits/misses observable in tests.
        uint64_t transform_calls() const { return pixel_shapes_.call_count(); }
        uint64_t picture_calls() const { return picture_.call_count(); }

    private:
        static SkColor to_sk_color(Color c) { return SkColorSetARGB(c.a, c.r, c.g, c.b); }

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

        static void draw_shape(SkCanvas &canvas, const PixelShape &shape, const ViewLayerStyle &style)
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

            // Paths are wire centerlines with a width, not filled polygons
            // - drawn as a solid stroked line using the fill color (the
            // layer's "main" color), not the separate outline color.
            for (const auto &p : shape.paths)
            {
                if (!has_fill)
                    continue;

                SkPaint wire = fill;
                wire.setStyle(SkPaint::kStroke_Style);
                wire.setStrokeWidth(static_cast<SkScalar>(p.width));
                wire.setStrokeCap(SkPaint::kButt_Cap);
                canvas.drawPath(to_sk_path(p.polygon, /*close=*/false), wire);
            }
        }

        CachedStage<std::tuple<AbstractId, uint64_t, uint64_t>, std::vector<PixelShape>> pixel_shapes_;
        CachedStage<std::tuple<AbstractId, uint64_t, uint64_t>, sk_sp<SkPicture>> picture_;
    };
}
