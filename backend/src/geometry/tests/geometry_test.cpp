#include "../geometry.hpp"
#include <gtest/gtest.h>

using namespace le;

namespace
{
    // le::Point/Rect are plain generated aggregates with no operator== - compare fields directly.
    void expect_point_eq(const Point &actual, const Point &expected)
    {
        EXPECT_EQ(actual.x, expected.x);
        EXPECT_EQ(actual.y, expected.y);
    }

    void expect_bounds(const std::vector<Point> &points, Point expected_min, Point expected_max)
    {
        ASSERT_FALSE(points.empty());
        int64_t min_x = points.front().x, max_x = min_x;
        int64_t min_y = points.front().y, max_y = min_y;
        for (const auto &p : points)
        {
            min_x = std::min(min_x, p.x);
            max_x = std::max(max_x, p.x);
            min_y = std::min(min_y, p.y);
            max_y = std::max(max_y, p.y);
        }
        EXPECT_EQ(min_x, expected_min.x);
        EXPECT_EQ(min_y, expected_min.y);
        EXPECT_EQ(max_x, expected_max.x);
        EXPECT_EQ(max_y, expected_max.y);
    }
}

TEST(Geometry, RectToPolygonIsClosedAndAxisAligned)
{
    Rect rect{.ll = {0, 0}, .ur = {10, 20}};
    Polygon polygon = Geometry::rect_to_polygon(rect);

    ASSERT_EQ(polygon.points.size(), 5u);
    expect_point_eq(polygon.points.front(), polygon.points.back());
}

TEST(Geometry, BboxOfShapeCoversAllRects)
{
    Shape shape;
    shape.rects.push_back(Rect{.ll = {0, 0}, .ur = {10, 10}});
    shape.rects.push_back(Rect{.ll = {5, -5}, .ur = {20, 5}});

    std::optional<Rect> box = Geometry::bbox(shape);
    ASSERT_TRUE(box.has_value());
    expect_point_eq(box->ll, Point{0, -5});
    expect_point_eq(box->ur, Point{20, 10});
}

TEST(Geometry, BboxOfShapeCoversPolygonsAndPaths)
{
    Shape shape;
    shape.polygons.push_back(Polygon{.points = {{0, 0}, {0, 10}, {10, 10}, {10, 0}, {0, 0}}});
    // Centerline (20,0)-(20,10), width 4 -> half-width 2 expands the bbox by 2 on every side.
    shape.paths.push_back(Path{.polygon = Polygon{.points = {{20, 0}, {20, 10}}}, .width = 4});

    std::optional<Rect> box = Geometry::bbox(shape);
    ASSERT_TRUE(box.has_value());
    expect_point_eq(box->ll, Point{0, -2});
    expect_point_eq(box->ur, Point{22, 12});
}

TEST(Geometry, BboxOfEmptyShapeIsNullopt)
{
    EXPECT_FALSE(Geometry::bbox(Shape{}).has_value());
    EXPECT_FALSE(Geometry::bbox(std::vector<Shape>{}).has_value());
    EXPECT_FALSE(Geometry::bbox(std::vector<const Shape *>{}).has_value());
}

TEST(Geometry, BboxOfShapeVectorUnionsAllShapes)
{
    Shape a;
    a.rects.push_back(Rect{.ll = {0, 0}, .ur = {5, 5}});
    Shape b;
    b.rects.push_back(Rect{.ll = {10, 10}, .ur = {15, 15}});

    std::optional<Rect> box = Geometry::bbox(std::vector<Shape>{a, b});
    ASSERT_TRUE(box.has_value());
    expect_point_eq(box->ll, Point{0, 0});
    expect_point_eq(box->ur, Point{15, 15});
}

TEST(Geometry, BboxOfShapePointerVectorUnionsAllShapes)
{
    Shape a;
    a.rects.push_back(Rect{.ll = {0, 0}, .ur = {5, 5}});
    Shape b;
    b.rects.push_back(Rect{.ll = {10, 10}, .ur = {15, 15}});

    std::optional<Rect> box = Geometry::bbox(std::vector<const Shape *>{&a, &b});
    ASSERT_TRUE(box.has_value());
    expect_point_eq(box->ll, Point{0, 0});
    expect_point_eq(box->ur, Point{15, 15});
}

TEST(Geometry, RectsOverlap)
{
    Rect a{.ll = {0, 0}, .ur = {10, 10}};
    Rect b{.ll = {5, 5}, .ur = {15, 15}};
    Rect c{.ll = {20, 20}, .ur = {30, 30}};

    EXPECT_TRUE(Geometry::rects_overlap(a, b));
    EXPECT_FALSE(Geometry::rects_overlap(a, c));
}

TEST(Geometry, TransformTranslatesPoints)
{
    Polygon polygon{.points = {{0, 0}, {10, 0}, {10, 10}}};
    Polygon moved = Geometry::transform(polygon, Point{5, -5});

    ASSERT_EQ(moved.points.size(), polygon.points.size());
    expect_point_eq(moved.points[0], Point{5, -5});
    expect_point_eq(moved.points[1], Point{15, -5});
    expect_point_eq(moved.points[2], Point{15, 5});
}

TEST(Geometry, EnsureClosedReturnsInputUnchangedWhenFewerThanTwoPoints)
{
    EXPECT_TRUE(Geometry::ensure_closed({}).empty());

    std::vector<Point> single = {{1, 2}};
    auto result = Geometry::ensure_closed(single);
    ASSERT_EQ(result.size(), 1u);
    expect_point_eq(result[0], Point{1, 2});
}

TEST(Geometry, EnsureClosedAppendsFirstPointWhenOpen)
{
    std::vector<Point> open = {{0, 0}, {10, 0}, {10, 10}};
    auto closed = Geometry::ensure_closed(open);

    ASSERT_EQ(closed.size(), 4u);
    expect_point_eq(closed.back(), closed.front());
}

TEST(Geometry, EnsureClosedLeavesAlreadyClosedPolygonUnchanged)
{
    std::vector<Point> already_closed = {{0, 0}, {10, 0}, {10, 10}, {0, 0}};
    auto result = Geometry::ensure_closed(already_closed);

    EXPECT_EQ(result.size(), 4u);
}

TEST(Geometry, PathToPolygonsBuffersCenterlineBySymmetricHalfWidth)
{
    // LEF PATH WIDTH is the total trace width, so a width-20 horizontal
    // centerline should buffer to a total height of 20 (10 on each side),
    // not 40 - this is the exact bug path_to_polygons had (see CLAUDE.md).
    Path path{.polygon = Polygon{.points = {{0, 0}, {100, 0}}}, .width = 20};
    auto polygons = Geometry::path_to_polygons(path);

    ASSERT_EQ(polygons.size(), 1u);
    expect_bounds(polygons.front().points, Point{0, -10}, Point{100, 10});
}

TEST(Geometry, UnionShapesReturnsNulloptWhenNoGeometry)
{
    EXPECT_FALSE(Geometry::union_shapes({}).has_value());

    Shape empty_shape;
    EXPECT_FALSE(Geometry::union_shapes({&empty_shape}).has_value());
}

TEST(Geometry, UnionShapesSkipsNullShapePointers)
{
    Shape shape;
    shape.rects.push_back(Rect{.ll = {0, 0}, .ur = {10, 10}});

    auto result = Geometry::union_shapes({nullptr, &shape});
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 1u);
    expect_bounds(result->front().points, Point{0, 0}, Point{10, 10});
}

TEST(Geometry, UnionShapesMergesOverlappingRectsIntoOnePolygon)
{
    Shape a;
    a.rects.push_back(Rect{.ll = {0, 0}, .ur = {10, 10}});
    Shape b;
    b.rects.push_back(Rect{.ll = {5, 5}, .ur = {15, 15}});

    auto result = Geometry::union_shapes({&a, &b});
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 1u); // overlapping -> merged into a single polygon
    expect_bounds(result->front().points, Point{0, 0}, Point{15, 15});
}

TEST(Geometry, UnionShapesIncludesPolygonsAndPathsNotJustRects)
{
    Shape shape;
    shape.polygons.push_back(Polygon{.points = {{0, 0}, {10, 0}, {10, 10}}});
    // Centerline (20,0)-(20,10), width 4 -> buffers to x:[18,22] - disjoint from the triangle.
    shape.paths.push_back(Path{.polygon = Polygon{.points = {{20, 0}, {20, 10}}}, .width = 4});

    auto result = Geometry::union_shapes({&shape});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->size(), 2u); // disjoint -> stay as two separate polygons
}

TEST(Geometry, MergeOverlappingFillsNoOpBelowTwoParts)
{
    Shape shape;
    shape.rects.push_back(Rect{.ll = {0, 0}, .ur = {10, 10}});

    Geometry::merge_overlapping_fills(shape);

    ASSERT_EQ(shape.rects.size(), 1u);
    EXPECT_TRUE(shape.polygons.empty());
}

TEST(Geometry, MergeOverlappingFillsMergesOverlappingRectsIntoOnePolygon)
{
    Shape shape;
    shape.rects.push_back(Rect{.ll = {0, 0}, .ur = {10, 10}});
    shape.rects.push_back(Rect{.ll = {5, 5}, .ur = {15, 15}});

    Geometry::merge_overlapping_fills(shape);

    EXPECT_TRUE(shape.rects.empty());
    ASSERT_EQ(shape.polygons.size(), 1u);
    expect_bounds(shape.polygons.front().points, Point{0, 0}, Point{15, 15});
}

TEST(Geometry, MergeOverlappingFillsKeepsDisjointRectsAsSeparatePolygons)
{
    Shape shape;
    shape.rects.push_back(Rect{.ll = {0, 0}, .ur = {10, 10}});
    shape.rects.push_back(Rect{.ll = {100, 100}, .ur = {110, 110}});

    Geometry::merge_overlapping_fills(shape);

    EXPECT_TRUE(shape.rects.empty());
    EXPECT_EQ(shape.polygons.size(), 2u);
}

TEST(Geometry, MergeOverlappingFillsIncludesExistingPolygons)
{
    Shape shape;
    shape.rects.push_back(Rect{.ll = {0, 0}, .ur = {10, 10}});
    shape.polygons.push_back(Polygon{.points = {{5, 5}, {15, 5}, {15, 15}, {5, 15}, {5, 5}}});

    Geometry::merge_overlapping_fills(shape);

    EXPECT_TRUE(shape.rects.empty());
    ASSERT_EQ(shape.polygons.size(), 1u);
    expect_bounds(shape.polygons.front().points, Point{0, 0}, Point{15, 15});
}

TEST(Geometry, MergeOverlappingFillsLeavesPathsUntouched)
{
    Shape shape;
    shape.rects.push_back(Rect{.ll = {0, 0}, .ur = {10, 10}});
    shape.rects.push_back(Rect{.ll = {5, 5}, .ur = {15, 15}});
    shape.paths.push_back(Path{.polygon = Polygon{.points = {{20, 0}, {20, 10}}}, .width = 4});

    Geometry::merge_overlapping_fills(shape);

    ASSERT_EQ(shape.paths.size(), 1u);
    expect_point_eq(shape.paths.front().polygon.points.front(), Point{20, 0});
}

TEST(Geometry, LabelLocationOfEmptyShapeIsOrigin)
{
    Shape shape;
    Point label = Geometry::get_label_location(shape);
    expect_point_eq(label, Point{0, 0});
}

TEST(Geometry, LabelLocationReturnsBboxCentroidWhenInsideShape)
{
    // Two overlapping rects (exercises the union-merge loop) whose combined
    // area covers its own bbox centroid, so the fast path returns directly.
    Shape shape;
    shape.rects.push_back(Rect{.ll = {0, 0}, .ur = {100, 60}});
    shape.rects.push_back(Rect{.ll = {0, 40}, .ur = {100, 100}});

    Point label = Geometry::get_label_location(shape);
    expect_point_eq(label, Point{50, 50});
}

TEST(Geometry, LabelLocationIncludesPolygonsAndPathsNotJustRects)
{
    // A square polygon plus a small path fully inside it - exercises the
    // shape.polygons/shape.paths loops (all other label-location tests only
    // used shape.rects). The path is entirely inside the polygon, so the
    // merged shape is still just the square and the fast path applies.
    Shape shape;
    shape.polygons.push_back(Polygon{.points = {{0, 0}, {100, 0}, {100, 100}, {0, 100}}});
    shape.paths.push_back(Path{.polygon = Polygon{.points = {{40, 40}, {60, 40}}}, .width = 4});

    Point label = Geometry::get_label_location(shape);
    expect_point_eq(label, Point{50, 50});
}

TEST(Geometry, LabelLocationFallsBackToGridSearchWhenCentroidIsInAHole)
{
    // A square frame (hole in the middle): the bbox centroid (50,50) falls in
    // the hole, so the fast path fails and the grid-search fallback must find
    // a point that's actually inside the frame material instead.
    Shape shape;
    shape.rects.push_back(Rect{.ll = {0, 0}, .ur = {100, 20}});
    shape.rects.push_back(Rect{.ll = {0, 80}, .ur = {100, 100}});
    shape.rects.push_back(Rect{.ll = {0, 20}, .ur = {20, 80}});
    shape.rects.push_back(Rect{.ll = {80, 20}, .ur = {100, 80}});

    Point label = Geometry::get_label_location(shape);
    expect_point_eq(label, Point{10, 50});
}

TEST(Geometry, LabelLocationHandlesZeroWidthBoundingBox)
{
    // A zero-width (degenerate vertical-line) rect: span_x == 0, exercising
    // the "clamp span to at least 1" guard that avoids a divide-by-zero grid
    // step. Nothing is ever strictly "within" a zero-area shape, so both the
    // fast path and the grid search fail and the raw centroid is returned.
    Shape shape;
    shape.rects.push_back(Rect{.ll = {50, 0}, .ur = {50, 100}});

    Point label = Geometry::get_label_location(shape);
    expect_point_eq(label, Point{50, 50});
}

TEST(Geometry, LabelLocationHandlesZeroHeightBoundingBox)
{
    // Same as above but for span_y == 0 (degenerate horizontal-line rect).
    Shape shape;
    shape.rects.push_back(Rect{.ll = {0, 50}, .ur = {100, 50}});

    Point label = Geometry::get_label_location(shape);
    expect_point_eq(label, Point{50, 50});
}

TEST(Geometry, LabelLocationReturnsRawCentroidWhenGridSearchFindsNoInteriorPoint)
{
    // Two disjoint bars with a gap between them: the bbox centroid (50,50)
    // isn't inside either bar, and the grid step (span/10=10) happens to
    // land exactly on the bars' edges for every sample, never strictly
    // inside - so even the grid-search fallback comes up empty and the
    // function returns the raw (off-shape) centroid as a last resort.
    Shape shape;
    shape.rects.push_back(Rect{.ll = {0, 0}, .ur = {10, 100}});
    shape.rects.push_back(Rect{.ll = {90, 0}, .ur = {100, 100}});

    Point label = Geometry::get_label_location(shape);
    expect_point_eq(label, Point{50, 50});
}

TEST(Geometry, LocalWidthAtRectReturnsMinDimension)
{
    // Non-square rect: the smaller dimension is the "width" a label
    // should be sized to, not the larger one or some average of the two.
    Shape shape;
    shape.rects.push_back(Rect{.ll = {0, 0}, .ur = {100, 20}});

    EXPECT_DOUBLE_EQ(Geometry::local_width_at(shape, Point{50, 10}), 20.0);
}

TEST(Geometry, LocalWidthAtPathReturnsPathWidthDirectly)
{
    // A Path's thickness is already known exactly - local_width_at should
    // read path.width directly rather than derive anything from its
    // (much larger) buffered bbox.
    Shape shape;
    shape.paths.push_back(Path{.polygon = Polygon{.points = {{0, 0}, {100, 0}}}, .width = 6});

    EXPECT_DOUBLE_EQ(Geometry::local_width_at(shape, Point{50, 0}), 6.0);
}

TEST(Geometry, LocalWidthAtSquarePolygonMeasuresDistanceToBoundaryNotFilledArea)
{
    // A point well inside a 100x100 square polygon: distance to the
    // nearest edge is 50 on every side, so width = 2*50 = 100. This is
    // also a regression guard for the "distance to a filled polygon is 0
    // for any interior point" pitfall - a buggy implementation that
    // measured distance to the filled area instead of its boundary would
    // return 0.0 here instead of 100.0.
    Shape shape;
    shape.polygons.push_back(Polygon{.points = {{0, 0}, {100, 0}, {100, 100}, {0, 100}}});

    EXPECT_NEAR(Geometry::local_width_at(shape, Point{50, 50}), 100.0, 0.01);
}

TEST(Geometry, LocalWidthAtLShapedPolygonUsesArmThicknessNotBbox)
{
    // An L-shaped (non-convex) polygon: a 100x30 bottom arm plus a 30x100
    // vertical arm, so the overall bbox is 100x100 - but a point centered
    // in the vertical arm's own 30-wide interior should be sized to that
    // arm's ~30 thickness, nowhere near the 100-wide bbox. This is the
    // regression test that would fail if sizing were ever "simplified"
    // back to a bbox-based approach - the whole point of this feature.
    Shape shape;
    shape.polygons.push_back(Polygon{.points = {
                                          {0, 0},
                                          {100, 0},
                                          {100, 30},
                                          {30, 30},
                                          {30, 100},
                                          {0, 100},
                                      }});

    const double width = Geometry::local_width_at(shape, Point{15, 70});
    EXPECT_NEAR(width, 30.0, 0.01);
    EXPECT_LT(width, 50.0); // far below the shape's own 100-wide bbox
}

TEST(Geometry, LocalWidthAtFallsBackToNearestPieceWhenPointOutsideEveryPiece)
{
    // Same two disjoint 10-wide bars as
    // LabelLocationReturnsRawCentroidWhenGridSearchFindsNoInteriorPoint,
    // whose raw-centroid fallback (50,50) sits in the gap between them,
    // outside both. local_width_at must fall back to the nearest bar's
    // own width (10), not the shape's overall ~90-wide bbox (which would
    // grossly overstate either bar's actual thickness).
    Shape shape;
    shape.rects.push_back(Rect{.ll = {0, 0}, .ur = {10, 100}});
    shape.rects.push_back(Rect{.ll = {90, 0}, .ur = {100, 100}});

    EXPECT_DOUBLE_EQ(Geometry::local_width_at(shape, Point{50, 50}), 10.0);
}

TEST(Geometry, LocalWidthAtOfEmptyShapeIsZero)
{
    Shape shape;
    EXPECT_DOUBLE_EQ(Geometry::local_width_at(shape, Point{0, 0}), 0.0);
}

TEST(Geometry, ContainsIsTrueInsideARectAndFalseOutside)
{
    Shape shape;
    shape.rects.push_back(Rect{.ll = {0, 0}, .ur = {10, 10}});

    EXPECT_TRUE(Geometry::contains(shape, Point{5, 5}));
    EXPECT_TRUE(Geometry::contains(shape, Point{0, 0})); // on the boundary counts
    EXPECT_FALSE(Geometry::contains(shape, Point{11, 5}));
}

TEST(Geometry, ContainsUsesRealPointInPolygonNotBboxForAnLShape)
{
    // Same L-shaped polygon as LocalWidthAtLShapedPolygonUsesArmThicknessNotBbox:
    // a point inside the overall 100x100 bbox but in the notch cut out of
    // the L (not inside either arm) must not count as contained - a bbox
    // check would wrongly say yes.
    Shape shape;
    shape.polygons.push_back(Polygon{.points = {
                                          {0, 0},
                                          {100, 0},
                                          {100, 30},
                                          {30, 30},
                                          {30, 100},
                                          {0, 100},
                                      }});

    EXPECT_TRUE(Geometry::contains(shape, Point{15, 70}));  // inside the vertical arm
    EXPECT_TRUE(Geometry::contains(shape, Point{70, 15}));  // inside the horizontal arm
    EXPECT_FALSE(Geometry::contains(shape, Point{70, 70})); // in the notch - within the bbox, outside the L
}

TEST(Geometry, ContainsUsesThePathsBufferedOutlineNotItsCenterline)
{
    // A horizontal path with width 10 centered on y=0: a point 3 above the
    // centerline is inside the buffered/drawn outline (half-width 5) but
    // would miss a naive "on the centerline" test.
    Shape shape;
    shape.paths.push_back(Path{.polygon = Polygon{.points = {{0, 0}, {100, 0}}}, .width = 10});

    EXPECT_TRUE(Geometry::contains(shape, Point{50, 3}));
    EXPECT_FALSE(Geometry::contains(shape, Point{50, 20})); // well outside the buffered width
}

TEST(Geometry, ContainsOfEmptyShapeIsFalse)
{
    Shape shape;
    EXPECT_FALSE(Geometry::contains(shape, Point{0, 0}));
}

TEST(Geometry, FindHitPieceReturnsOnlyTheOneRectHitNotEveryRectInTheShape)
{
    // Regression: a Shape can bundle several rects together (e.g. several
    // RECT statements in one LEF PORT) - hover highlighting must isolate
    // just the one piece under the cursor, not the whole group (a real
    // reported bug: hovering one rect highlighted every rect on the same
    // Terminal).
    Shape shape;
    shape.layer_name = "M1";
    shape.rects.push_back(Rect{.ll = {0, 0}, .ur = {10, 10}});
    shape.rects.push_back(Rect{.ll = {100, 100}, .ur = {110, 110}});

    const auto piece = Geometry::find_hit_piece(shape, Point{5, 5});
    ASSERT_TRUE(piece.has_value());
    EXPECT_EQ(piece->layer_name, "M1");
    ASSERT_EQ(piece->rects.size(), 1u);
    EXPECT_EQ(piece->rects.front().ll.x, 0);
    EXPECT_TRUE(piece->polygons.empty());
    EXPECT_TRUE(piece->paths.empty());
}

TEST(Geometry, FindHitPieceReturnsOnlyTheOnePolygonHitNotEveryPolygonInTheShape)
{
    Shape shape;
    shape.polygons.push_back(Polygon{.points = {{0, 0}, {10, 0}, {10, 10}, {0, 10}}});
    shape.polygons.push_back(Polygon{.points = {{100, 100}, {110, 100}, {110, 110}, {100, 110}}});

    const auto piece = Geometry::find_hit_piece(shape, Point{5, 5});
    ASSERT_TRUE(piece.has_value());
    ASSERT_EQ(piece->polygons.size(), 1u);
    EXPECT_TRUE(piece->rects.empty());
}

TEST(Geometry, FindHitPieceReturnsNulloptOnAMiss)
{
    Shape shape;
    shape.rects.push_back(Rect{.ll = {0, 0}, .ur = {10, 10}});

    EXPECT_FALSE(Geometry::find_hit_piece(shape, Point{500, 500}).has_value());
}

TEST(Geometry, FullyEnclosedIsTrueWhenShapeFitsEntirelyInsideTheContainer)
{
    Shape shape;
    shape.rects.push_back(Rect{.ll = {10, 10}, .ur = {20, 20}});

    EXPECT_TRUE(Geometry::fully_enclosed(Rect{.ll = {0, 0}, .ur = {30, 30}}, shape));
}

TEST(Geometry, FullyEnclosedIsFalseWhenShapeOnlyPartiallyOverlaps)
{
    Shape shape;
    shape.rects.push_back(Rect{.ll = {10, 10}, .ur = {20, 20}});

    // Container only covers the left half of the shape's bbox.
    EXPECT_FALSE(Geometry::fully_enclosed(Rect{.ll = {0, 0}, .ur = {15, 30}}, shape));
}

TEST(Geometry, FullyEnclosedIsFalseWhenShapeIsCompletelyOutsideTheContainer)
{
    Shape shape;
    shape.rects.push_back(Rect{.ll = {100, 100}, .ur = {110, 110}});

    EXPECT_FALSE(Geometry::fully_enclosed(Rect{.ll = {0, 0}, .ur = {30, 30}}, shape));
}

TEST(Geometry, FullyEnclosedOfEmptyShapeIsFalse)
{
    Shape shape;
    EXPECT_FALSE(Geometry::fully_enclosed(Rect{.ll = {0, 0}, .ur = {100, 100}}, shape));
}

TEST(Geometry, FullyEnclosedPiecesReturnsOnlyTheIndividuallyEnclosedPieces)
{
    // A bundled multi-piece Shape where some pieces fit inside the
    // container and others don't - the per-piece analog of
    // fully_enclosed, which only ever answers for the whole bundle.
    Shape shape{
        .layer_name = "M1",
        .rects = {
            Rect{.ll = {10, 10}, .ur = {20, 20}},   // inside
            Rect{.ll = {100, 100}, .ur = {110, 110}}, // outside
        },
        .polygons = {
            Polygon{.points = {{15, 15}, {18, 15}, {18, 18}, {15, 18}}}, // inside
        },
    };

    const auto pieces = Geometry::fully_enclosed_pieces(Rect{.ll = {0, 0}, .ur = {30, 30}}, shape);

    ASSERT_EQ(pieces.size(), 2u);
    EXPECT_EQ(pieces[0].rects.size(), 1u);
    EXPECT_EQ(pieces[0].rects.front().ll.x, 10);
    EXPECT_EQ(pieces[1].polygons.size(), 1u);
    EXPECT_EQ(pieces[0].layer_name, "M1"); // layer_name carried onto each single-piece Shape
}

TEST(Geometry, FullyEnclosedPiecesIsEmptyWhenNoPieceFits)
{
    Shape shape;
    shape.rects.push_back(Rect{.ll = {100, 100}, .ur = {110, 110}});

    EXPECT_TRUE(Geometry::fully_enclosed_pieces(Rect{.ll = {0, 0}, .ur = {30, 30}}, shape).empty());
}
