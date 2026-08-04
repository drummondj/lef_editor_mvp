#pragma once
#include "../database/database.hpp"
#include <boost/geometry.hpp>
#include <boost/geometry/algorithms/buffer.hpp>
#include <boost/geometry/algorithms/union.hpp>
#include <boost/geometry/geometries/register/point.hpp>
#include <boost/geometry/geometries/register/box.hpp>
#include <boost/geometry/geometries/register/linestring.hpp>
#include <boost/geometry/geometries/linestring.hpp>
#include <boost/geometry/geometries/polygon.hpp>
#include <boost/geometry/geometries/multi_polygon.hpp>
#include <boost/geometry/strategies/transform/matrix_transformers.hpp>
#include <boost/geometry/strategies/buffer.hpp>
#include <limits>
#include <optional>

namespace bg = boost::geometry;

BOOST_GEOMETRY_REGISTER_POINT_2D(le::Point, int64_t, bg::cs::cartesian, x, y)
BOOST_GEOMETRY_REGISTER_BOX(le::Rect, le::Point, ll, ur)
BOOST_GEOMETRY_REGISTER_LINESTRING(std::vector<le::Point>)

namespace le
{
    /// @brief Boost.Geometry-backed operations over the database's Point/Rect/Polygon/Path/Shape types.
    class Geometry
    {
    private:
        static void expand_bbox(std::optional<Rect> &out, const Rect &r)
        {
            if (!out)
                out = r;
            else
                bg::expand(*out, r);
        }

        static Rect bbox_of(const Polygon &poly)
        {
            Rect r;
            bg::envelope(poly.points, r);
            return r;
        }

        static Rect bbox_of(const Path &path)
        {
            Rect r;
            bg::envelope(path.polygon.points, r);

            const int64_t half = path.width / 2;
            bg::set<0>(r.ll, bg::get<0>(r.ll) - half);
            bg::set<1>(r.ll, bg::get<1>(r.ll) - half);
            bg::set<0>(r.ur, bg::get<0>(r.ur) + half);
            bg::set<1>(r.ur, bg::get<1>(r.ur) + half);

            return r;
        }

        static void accumulate_bbox(std::optional<Rect> &out, const Shape &shape)
        {
            for (const auto &r : shape.rects)
                expand_bbox(out, r);

            for (const auto &poly : shape.polygons)
                expand_bbox(out, bbox_of(poly));

            for (const auto &path : shape.paths)
                expand_bbox(out, bbox_of(path));
        }

        static bg::model::polygon<Point> to_boost_polygon(const Polygon &polygon)
        {
            bg::model::polygon<Point> bg_polygon;
            std::vector<Point> ring = ensure_closed(polygon.points);
            bg_polygon.outer().assign(ring.begin(), ring.end());
            bg::correct(bg_polygon);
            return bg_polygon;
        }

        static Polygon from_boost_polygon(const bg::model::polygon<Point> &bg_polygon)
        {
            Polygon polygon;
            polygon.points.reserve(bg_polygon.outer().size());

            for (const auto &pt : bg_polygon.outer())
                polygon.points.push_back(Point{
                    static_cast<int64_t>(bg::get<0>(pt)),
                    static_cast<int64_t>(bg::get<1>(pt)),
                });

            polygon.points = ensure_closed(polygon.points);
            return polygon;
        }

    public:
        static std::optional<Rect> bbox(const Shape &shape)
        {
            std::optional<Rect> out;
            accumulate_bbox(out, shape);
            return out;
        }

        static std::optional<Rect> bbox(const std::vector<Shape> &shapes)
        {
            std::optional<Rect> out;

            for (const auto &shape : shapes)
                accumulate_bbox(out, shape);

            return out;
        }

        static std::optional<Rect> bbox(const std::vector<const Shape *> &shapes)
        {
            std::optional<Rect> out;

            for (const auto &shape : shapes)
                accumulate_bbox(out, *shape);

            return out;
        }

        static bool rects_overlap(const Rect &a, const Rect &b)
        {
            return bg::intersects(a, b);
        }

        static Polygon transform(const Polygon &polygon, const Point &offset)
        {
            std::vector<Point> transformed_points;
            bg::strategy::transform::translate_transformer<int64_t, 2, 2> translate(offset.x, offset.y);
            bg::transform(polygon.points, transformed_points, translate);
            return Polygon{.points = transformed_points};
        }

        static Polygon rect_to_polygon(const Rect &rect)
        {
            std::vector<Point> points;
            points.reserve(5);
            points.push_back(Point{.x = rect.ll.x, .y = rect.ll.y});
            points.push_back(Point{.x = rect.ll.x, .y = rect.ur.y});
            points.push_back(Point{.x = rect.ur.x, .y = rect.ur.y});
            points.push_back(Point{.x = rect.ur.x, .y = rect.ll.y});
            points.push_back(Point{.x = rect.ll.x, .y = rect.ll.y});
            return Polygon{.points = points};
        }

        static std::vector<Polygon> path_to_polygons(const Path &path)
        {
            using BgPolygon = bg::model::polygon<Point>;
            using BgMultiPolygon = bg::model::multi_polygon<BgPolygon>;

            BgMultiPolygon out;
            // distance_symmetric buffers by this distance on EACH side of the
            // centerline (total width = 2x), but LEF's PATH WIDTH is the total
            // trace width, so halve it here rather than doubling every path.
            bg::strategy::buffer::distance_symmetric<double> distance_strategy(path.width / 2.0);
            bg::strategy::buffer::join_miter join_strategy(5);
            bg::strategy::buffer::end_flat end_strategy;
            bg::strategy::buffer::point_square circle_strategy;
            bg::strategy::buffer::side_straight side_strategy;

            bg::buffer(path.polygon.points, out, distance_strategy, side_strategy, join_strategy, end_strategy, circle_strategy);

            std::vector<Polygon> result;
            result.reserve(out.size());

            for (const auto &poly : out)
                result.push_back(from_boost_polygon(poly));

            return result;
        }

        static std::vector<Point> ensure_closed(const std::vector<Point> &points)
        {
            if (points.size() < 2)
                return points;

            std::vector<Point> closed = points;
            const auto &first = closed.front();
            const auto &last = closed.back();

            if (last.x != first.x || last.y != first.y)
                closed.push_back(first);

            return closed;
        }

        static std::optional<std::vector<Polygon>> union_shapes(const std::vector<const Shape *> &shapes)
        {
            std::vector<Polygon> parts;

            // Lower-bound estimate (paths can expand into more than one
            // polygon each via path_to_polygons, so this isn't exact), but
            // still avoids most reallocations compared to reserving nothing.
            size_t estimated_parts = 0;
            for (const auto *shape : shapes)
                if (shape)
                    estimated_parts += shape->rects.size() + shape->polygons.size() + shape->paths.size();
            parts.reserve(estimated_parts);

            for (const auto *shape : shapes)
            {
                if (!shape)
                    continue;

                for (const auto &rect : shape->rects)
                    parts.push_back(rect_to_polygon(rect));

                for (const auto &polygon : shape->polygons)
                    parts.push_back(from_boost_polygon(to_boost_polygon(polygon)));

                for (const auto &path : shape->paths)
                {
                    for (const auto &path_part : path_to_polygons(path))
                        parts.push_back(path_part);
                }
            }

            if (parts.empty())
                return std::nullopt;

            using BgPolygon = bg::model::polygon<Point>;
            using BgMultiPolygon = bg::model::multi_polygon<BgPolygon>;

            std::vector<BgPolygon> bg_parts;
            bg_parts.reserve(parts.size());

            for (const auto &part : parts)
                bg_parts.push_back(to_boost_polygon(part));

            // bg_parts is non-empty here: parts (checked above) and bg_parts
            // always have the same size.
            BgMultiPolygon result;
            result.push_back(bg_parts.front());

            for (size_t i = 1; i < bg_parts.size(); ++i)
            {
                BgMultiPolygon next;
                bg::union_(result, bg_parts[i], next);
                result = std::move(next);
            }

            std::vector<Polygon> merged;
            merged.reserve(result.size());

            for (const auto &poly : result)
                merged.push_back(from_boost_polygon(poly));

            return merged;
        }

        /// @brief Merges this Shape's own rects and polygons into a
        /// minimal, non-overlapping set of polygons via boost::geometry
        /// union, replacing both in place. Paths are left untouched - they
        /// carry their own width/stroke semantics a fill-polygon union
        /// would lose, and aren't the source of the artifact this fixes.
        /// A Terminal Port's or Obstruction's Shape can legitimately
        /// contain several self-overlapping rects (e.g. LEF's OBS
        /// ITERATE/DO/STEP array syntax generates copies that overlap each
        /// other), and drawing each with a translucent fill double-blends
        /// the overlap into a visibly darker patch - merging first means
        /// each covered pixel is painted once. No-op if there's nothing to
        /// merge (0 or 1 rect/polygon total).
        static void merge_overlapping_fills(Shape &shape)
        {
            if (shape.rects.size() + shape.polygons.size() <= 1)
                return;

            using BgPolygon = bg::model::polygon<Point>;
            using BgMultiPolygon = bg::model::multi_polygon<BgPolygon>;

            std::vector<BgPolygon> parts;
            parts.reserve(shape.rects.size() + shape.polygons.size());

            for (const auto &rect : shape.rects)
                parts.push_back(to_boost_polygon(rect_to_polygon(rect)));

            for (const auto &polygon : shape.polygons)
                parts.push_back(to_boost_polygon(polygon));

            BgMultiPolygon result;
            result.push_back(parts.front());

            for (size_t i = 1; i < parts.size(); ++i)
            {
                BgMultiPolygon next;
                bg::union_(result, parts[i], next);
                result = std::move(next);
            }

            shape.rects.clear();
            shape.polygons.clear();
            shape.polygons.reserve(result.size());

            for (const auto &poly : result)
                shape.polygons.push_back(from_boost_polygon(poly));
        }

        static Point get_label_location(const Shape &shape)
        {
            using BgPolygon = bg::model::polygon<Point>;
            using BgMultiPolygon = bg::model::multi_polygon<BgPolygon>;

            std::vector<BgPolygon> parts;
            // Lower-bound estimate: paths can expand into more than one
            // polygon each via path_to_polygons.
            parts.reserve(shape.rects.size() + shape.polygons.size() + shape.paths.size());

            for (const auto &rect : shape.rects)
                parts.push_back(to_boost_polygon(rect_to_polygon(rect)));

            for (const auto &polygon : shape.polygons)
                parts.push_back(to_boost_polygon(polygon));

            for (const auto &path : shape.paths)
            {
                for (const auto &path_part : path_to_polygons(path))
                    parts.push_back(to_boost_polygon(path_part));
            }

            if (parts.empty())
                return Point{0, 0};

            BgMultiPolygon unioned;
            for (size_t i = 0; i < parts.size(); ++i)
            {
                BgMultiPolygon next;
                if (i == 0)
                {
                    unioned.push_back(parts[i]);
                }
                else
                {
                    bg::union_(unioned, parts[i], next);
                    unioned = std::move(next);
                }
            }

            // bbox(shape) is non-nullopt here: parts (checked above) and this
            // bbox are accumulated from the same shape.rects/polygons/paths.
            const Rect bbox = *Geometry::bbox(shape);

            const int64_t target_x = (bbox.ll.x + bbox.ur.x) / 2;
            const int64_t target_y = (bbox.ll.y + bbox.ur.y) / 2;
            Point target{target_x, target_y};

            for (const auto &poly : unioned)
            {
                if (bg::within(target, poly))
                    return target;
            }

            const int64_t span_x = (bbox.ur.x - bbox.ll.x) > 0 ? (bbox.ur.x - bbox.ll.x) : 1;
            const int64_t span_y = (bbox.ur.y - bbox.ll.y) > 0 ? (bbox.ur.y - bbox.ll.y) : 1;

            const int steps = 11;
            const int64_t step_x = span_x / (steps - 1);
            const int64_t step_y = span_y / (steps - 1);

            std::optional<Point> best;
            int64_t best_dist_sq = std::numeric_limits<int64_t>::max();

            for (const auto &poly : unioned)
            {
                for (int i = 0; i < steps; ++i)
                {
                    for (int j = 0; j < steps; ++j)
                    {
                        Point candidate{
                            bbox.ll.x + i * step_x,
                            bbox.ll.y + j * step_y,
                        };

                        if (!bg::within(candidate, poly))
                            continue;

                        const int64_t dx = candidate.x - target.x;
                        const int64_t dy = candidate.y - target.y;
                        const int64_t dist_sq = dx * dx + dy * dy;

                        if (dist_sq < best_dist_sq)
                        {
                            best_dist_sq = dist_sq;
                            best = candidate;
                        }
                    }
                }
            }

            if (best)
                return *best;

            return target;
        }
    };
}
