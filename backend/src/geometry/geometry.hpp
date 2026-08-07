#pragma once
#include "../database/database.hpp"
#include <algorithm>
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

        // Cheap containment pre-check shared by find_hit_piece and
        // local_width_at, ahead of an expensive bg::within/path_to_polygons
        // call - see find_hit_piece's own doc comment for the measured
        // cost this guards against (~17ms/call on the 1M-shape stress
        // design without it, dominated by path_to_polygons).
        static bool point_in_rect(const Point &point, const Rect &rect)
        {
            return point.x >= rect.ll.x && point.x <= rect.ur.x && point.y >= rect.ll.y && point.y <= rect.ur.y;
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

        // Assigns straight from `polygon.points` into `bg_polygon`'s own
        // ring storage - one copy, not two. ensure_closed() (used as-is by
        // the other call site below, which needs the closed vector back)
        // would work here too but builds and returns its own intermediate
        // std::vector<Point> first, which this then has to copy *again*
        // via .assign() - avoided by closing the ring in place instead.
        static bg::model::polygon<Point> to_boost_polygon(const Polygon &polygon)
        {
            bg::model::polygon<Point> bg_polygon;
            bg_polygon.outer().assign(polygon.points.begin(), polygon.points.end());

            if (!polygon.points.empty())
            {
                const auto &first = polygon.points.front();
                const auto &last = polygon.points.back();
                if (last.x != first.x || last.y != first.y)
                    bg_polygon.outer().push_back(first);
            }

            bg::correct(bg_polygon);
            return bg_polygon;
        }

        // bg::distance(point, ring) treats a *closed ring* as an areal
        // geometry, same trap as bg::distance(point, polygon) - it's 0 for
        // any interior point, not the boundary distance you'd expect.
        // Copying the ring's points into an explicit linestring forces
        // Boost.Geometry to treat it as a 1-D path instead, giving the
        // correct nearest-edge distance regardless of whether `point` is
        // inside or outside.
        static double distance_to_boundary(const bg::model::polygon<Point> &bg_polygon, const Point &point)
        {
            const auto &ring = bg::exterior_ring(bg_polygon);
            bg::model::linestring<Point> boundary(ring.begin(), ring.end());
            return bg::distance(point, boundary);
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

        /// @brief The local "width" (thickness) of `shape` at `point`: the
        /// diameter of the largest circle centered at `point` that still
        /// fits inside whichever rect/polygon/path of `shape` contains it.
        /// Used to size a label to the actual geometry it sits on rather
        /// than the shape's overall bounding box, which is wrong for a
        /// long, thin, non-convex (e.g. L-shaped) polygon.
        ///
        /// - Rects are checked first (cheapest, exact): if `point` falls
        ///   inside one, its width is simply `min(dx, dy)` - no boost call.
        /// - Paths are checked next: if `point` falls inside one's
        ///   buffered polygon (via `path_to_polygons`), its width is
        ///   `path.width` directly - a Path's thickness is already known
        ///   exactly and uniform along its whole centerline, so no
        ///   distance computation applies.
        /// - Polygons are checked last: if `point` falls inside one,
        ///   `2 * distance_to_boundary(...)` approximates local thickness
        ///   there - distance to the *boundary*, not the filled area
        ///   (`bg::distance` against either the filled polygon or its own
        ///   closed exterior ring directly both return 0 for any interior
        ///   point, treating them as areal geometry - see
        ///   distance_to_boundary's own comment for why the ring's points
        ///   are copied into an explicit linestring first). This stays
        ///   correct for non-convex/L-shaped polygons because it's local
        ///   to `point`, unlike an area/perimeter ratio over the whole
        ///   polygon (perimeter sums the concave boundary too, inflating
        ///   the ratio; area/max(dim) conflates both arms of an L into one
        ///   wrong average for either arm).
        ///
        /// Like find_hit_piece, polygons/paths are pre-checked against a
        /// cheap bbox (point_in_rect) before the real bg::within/
        /// path_to_polygons call, for the same measured reason - see that
        /// function's own doc comment.
        ///
        /// If `point` falls inside none of the shape's pieces (e.g. it
        /// came from get_label_location's own last-resort raw-centroid
        /// fallback, which can land outside every piece for disjoint
        /// geometry - see its own tests), falls back to whichever
        /// individual piece's own bbox center is nearest to `point`, and
        /// returns that piece's width by the same rules above - a
        /// meaningful per-piece answer, not the whole shape's bbox (which
        /// could span disjoint pieces and grossly overstate their actual
        /// width). Returns 0.0 for a shape with no geometry at all.
        static double local_width_at(const Shape &shape, const Point &point)
        {
            for (const auto &rect : shape.rects)
            {
                if (point_in_rect(point, rect))
                    return static_cast<double>(std::min(rect.ur.x - rect.ll.x, rect.ur.y - rect.ll.y));
            }

            for (const auto &path : shape.paths)
            {
                if (!point_in_rect(point, bbox_of(path)))
                    continue;

                for (const auto &part : path_to_polygons(path))
                {
                    if (bg::within(point, to_boost_polygon(part)))
                        return static_cast<double>(path.width);
                }
            }

            for (const auto &polygon : shape.polygons)
            {
                if (!point_in_rect(point, bbox_of(polygon)))
                    continue;

                const auto bg_polygon = to_boost_polygon(polygon);
                if (bg::within(point, bg_polygon))
                    return 2.0 * distance_to_boundary(bg_polygon, point);
            }

            // Fallback: point isn't inside any single piece - use whichever
            // piece's own bbox center is nearest to point, and that
            // piece's own width (by the same rules as above).
            std::optional<double> best_width;
            int64_t best_dist_sq = std::numeric_limits<int64_t>::max();

            auto consider = [&](const Point &center, double width)
            {
                const int64_t dx = center.x - point.x;
                const int64_t dy = center.y - point.y;
                const int64_t dist_sq = dx * dx + dy * dy;
                if (dist_sq < best_dist_sq)
                {
                    best_dist_sq = dist_sq;
                    best_width = width;
                }
            };

            for (const auto &rect : shape.rects)
            {
                const Point center{(rect.ll.x + rect.ur.x) / 2, (rect.ll.y + rect.ur.y) / 2};
                consider(center, static_cast<double>(std::min(rect.ur.x - rect.ll.x, rect.ur.y - rect.ll.y)));
            }

            for (const auto &path : shape.paths)
            {
                const Rect bbox = bbox_of(path);
                const Point center{(bbox.ll.x + bbox.ur.x) / 2, (bbox.ll.y + bbox.ur.y) / 2};
                consider(center, static_cast<double>(path.width));
            }

            for (const auto &polygon : shape.polygons)
            {
                const Rect bbox = bbox_of(polygon);
                const Point center{(bbox.ll.x + bbox.ur.x) / 2, (bbox.ll.y + bbox.ur.y) / 2};
                const auto bg_polygon = to_boost_polygon(polygon);
                consider(center, 2.0 * distance_to_boundary(bg_polygon, center));
            }

            return best_width.value_or(0.0);
        }

        /// @brief Like `contains`, but returns a copy of just the single
        /// rect/polygon/path piece that contains `point` (as its own
        /// one-piece Shape, same `layer_name`), not the whole `shape` -
        /// `shape` can bundle several rects/polygons/paths together (e.g.
        /// several RECT statements in one LEF PORT, or an OBS's array of
        /// rects), and click/hover hit-testing must identify only the one
        /// piece actually under `point`, not the whole group (UPDATES.md
        /// 7.1 - highlighting every rect in the group when only one was
        /// under the cursor was a real reported bug). nullopt if no piece
        /// contains `point`.
        ///
        /// Polygons/paths are pre-checked against a cheap bbox (see
        /// point_in_rect above, and bbox_of for paths - already accounts
        /// for width) before the real `bg::within`/`path_to_polygons`
        /// test - a real measured cost, not speculation: BM_HitTestPoint
        /// (pipeline_benchmark.cpp) against the 1M-shape stress design
        /// initially measured ~17ms/call without it (dominated by
        /// path_to_polygons - a real Boost.Geometry buffer operation -
        /// running on every visible path regardless of whether the query
        /// point was anywhere near it), clearly too slow to run on every
        /// pointer-move event (see le_set_mouse_position). With the
        /// pre-check, most candidates are rejected by four integer
        /// comparisons before any Boost.Geometry call - see
        /// BENCHMARKS.md for the before/after numbers.
        static std::optional<Shape> find_hit_piece(const Shape &shape, const Point &point)
        {
            for (const auto &rect : shape.rects)
            {
                if (point_in_rect(point, rect))
                    return Shape{.layer_name = shape.layer_name, .rects = {rect}};
            }

            for (const auto &polygon : shape.polygons)
            {
                if (!point_in_rect(point, bbox_of(polygon)))
                    continue;

                if (bg::within(point, to_boost_polygon(polygon)))
                    return Shape{.layer_name = shape.layer_name, .polygons = {polygon}};
            }

            for (const auto &path : shape.paths)
            {
                if (!point_in_rect(point, bbox_of(path)))
                    continue;

                for (const auto &part : path_to_polygons(path))
                {
                    if (bg::within(point, to_boost_polygon(part)))
                        return Shape{.layer_name = shape.layer_name, .paths = {path}};
                }
            }

            return std::nullopt;
        }

        /// @brief True if `point` falls within `shape`'s actual drawn
        /// geometry (any rect/polygon/path piece) - used where only a
        /// yes/no answer is needed. See find_hit_piece for the same test
        /// when the specific piece itself is also needed (e.g. hover
        /// highlighting).
        static bool contains(const Shape &shape, const Point &point)
        {
            return find_hit_piece(shape, point).has_value();
        }

        /// @brief True if `shape`'s bbox is entirely inside `container` -
        /// used for rubber-band drag-select (UPDATES.md 7.1 item 5:
        /// "completely enclosed by the selection rectangle"). Exact, not
        /// an approximation: for an axis-aligned rectangle, "every point
        /// of shape is inside container" and "shape's tightest bounding
        /// box is inside container" are equivalent - if the bbox fits, so
        /// does everything inside it; if any point of the shape were
        /// outside, the bbox (being the tightest fit around the shape)
        /// would be too. So no per-rect/polygon/path testing is needed
        /// here, unlike `contains`. False for a shape with no geometry
        /// (no bbox).
        static bool fully_enclosed(const Rect &container, const Shape &shape)
        {
            const auto bbox = Geometry::bbox(shape);
            if (!bbox)
                return false;

            return bbox->ll.x >= container.ll.x && bbox->ll.y >= container.ll.y &&
                   bbox->ur.x <= container.ur.x && bbox->ur.y <= container.ur.y;
        }
    };
}
