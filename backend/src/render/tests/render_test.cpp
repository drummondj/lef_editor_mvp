#include "../render.hpp"
#include "include/core/SkBitmap.h"
#include "include/core/SkColor.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkSurface.h"
#include <algorithm>
#include <array>
#include <gtest/gtest.h>

using namespace le;

namespace
{
    SkColor to_sk_color(Color c) { return SkColorSetARGB(c.a, c.r, c.g, c.b); }

    // Builds a Root with one Technology, an M1 and M2 layer, a matching
    // ViewLayerSet, and one empty Abstract - the common scaffolding every
    // test below attaches to. Reuses PipelineFixture's shape of setup
    // (see pipeline_test.cpp) since Renderer sits directly on top of
    // Pipeline's output.
    struct RenderFixture : public ::testing::Test
    {
        void SetUp() override
        {
            technology_id = root.create_technology(TechnologyData{.database_units_microns = 1000.0});
            m1 = root.create_layer(LayerData{.technology = technology_id, .name = "M1", .type = "ROUTING"});
            m2 = root.create_layer(LayerData{.technology = technology_id, .name = "M2", .type = "ROUTING"});
            view_layers = ViewLayerSet::build_for_technology(root, technology_id);
            abstract_id = root.create_abstract(AbstractData{});
        }

        TerminalId add_terminal_shape(const Shape &shape)
        {
            TerminalId terminal_id = root.create_terminal(TerminalData{.abstract = abstract_id});
            root.create_terminal_port(TerminalPortData{.terminal = terminal_id, .shapes = {shape}});
            return terminal_id;
        }

        ObstructionId add_obstruction_shape(const Shape &shape)
        {
            return root.create_obstruction(ObstructionData{.abstract = abstract_id, .shapes = {shape}});
        }

        // Rasterizes `picture` into a fresh, explicitly-cleared-to-transparent
        // surface and reads back one pixel. Compositing translucent (or
        // opaque) source content over a fully transparent destination is
        // exactly the source color (Porter-Duff "over" with a zero
        // destination) - so this recovers the exact drawn color at a fully
        // covered (non-antialiased-edge) pixel, no blending math needed.
        SkColor sample_pixel(const sk_sp<SkPicture> &picture, int width, int height, int x, int y)
        {
            return rasterize(picture, width, height).getColor(x, y);
        }

        SkBitmap rasterize(const sk_sp<SkPicture> &picture, int width, int height)
        {
            sk_sp<SkSurface> surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height));
            surface->getCanvas()->clear(SK_ColorTRANSPARENT);
            surface->getCanvas()->drawPicture(picture);

            SkBitmap bitmap;
            bitmap.allocPixels(SkImageInfo::MakeN32Premul(width, height));
            surface->readPixels(bitmap, 0, 0);
            return bitmap;
        }

        // A tiled FillPattern (stripes/brick/dots) only covers part of its
        // shape - unlike the old flat fill, a single fixed-point sample can
        // land in a transparent gap by coincidence of the pattern's phase.
        // Scans a region instead and checks whether `rgb` (ignoring alpha -
        // a covered pattern pixel is opaque, an uncovered one transparent,
        // never a partial match) shows up anywhere in it.
        bool region_shows_color(const SkBitmap &bitmap, int x0, int y0, int x1, int y1, SkColor rgb)
        {
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                {
                    SkColor c = bitmap.getColor(x, y);
                    if (SkColorGetA(c) > 0 && SkColorGetR(c) == SkColorGetR(rgb) && SkColorGetG(c) == SkColorGetG(rgb) && SkColorGetB(c) == SkColorGetB(rgb))
                        return true;
                }
            return false;
        }

        Root root;
        TechnologyId technology_id;
        LayerId m1;
        LayerId m2;
        ViewLayerSet view_layers;
        AbstractId abstract_id;
        Pipeline pipeline;
        Renderer renderer;
    };
}

TEST_F(RenderFixture, TransformToPixelsAppliesPanAndScale)
{
    add_obstruction_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {10, 20}, .ur = {30, 40}}}});

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{5, 5});
    scene.set_scale(2.0);
    scene.set_viewport_size(200, 200);

    const auto &shapes = pipeline.run(root, scene, view_layers);
    const auto &pixel_shapes = renderer.transform_to_pixels(shapes, scene);

    ASSERT_EQ(pixel_shapes.size(), 1u);
    const auto &group = pixel_shapes.begin()->second;
    ASSERT_EQ(group.size(), 1u);
    ASSERT_EQ(group.front().rects.size(), 1u);
    const auto &r = group.front().rects.front();
    EXPECT_DOUBLE_EQ(r.ll.x, (10 - 5) * 2.0);
    EXPECT_DOUBLE_EQ(r.ll.y, (20 - 5) * 2.0);
    EXPECT_DOUBLE_EQ(r.ur.x, (30 - 5) * 2.0);
    EXPECT_DOUBLE_EQ(r.ur.y, (40 - 5) * 2.0);
}

TEST_F(RenderFixture, TransformToPixelsHandlesPolygonsPathsAndTexts)
{
    add_obstruction_shape(Shape{
        .layer_name = "M1",
        .paths = {Path{.polygon = Polygon{.points = {{0, 0}, {10, 0}}}, .width = 4}},
        .polygons = {Polygon{.points = {{0, 0}, {10, 0}, {10, 10}, {0, 10}}}},
        .texts = {Text{.label = "note", .location = {5, 5}, .size = 20}},
    });

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{0, 0});
    scene.set_scale(2.0);
    scene.set_viewport_size(100, 100);

    const auto &shapes = pipeline.run(root, scene, view_layers);
    const auto &pixel_shapes = renderer.transform_to_pixels(shapes, scene);

    ASSERT_EQ(pixel_shapes.size(), 1u);
    const auto &group = pixel_shapes.begin()->second;
    ASSERT_EQ(group.size(), 1u);
    const auto &ps = group.front();
    ASSERT_EQ(ps.polygons.size(), 1u);
    ASSERT_EQ(ps.polygons.front().points.size(), 4u);
    EXPECT_DOUBLE_EQ(ps.polygons.front().points[2].x, 20.0);
    EXPECT_DOUBLE_EQ(ps.polygons.front().points[2].y, 20.0);

    ASSERT_EQ(ps.paths.size(), 1u);
    EXPECT_DOUBLE_EQ(ps.paths.front().width, 8.0); // 4 dbu * scale 2.0
    ASSERT_EQ(ps.paths.front().polygon.points.size(), 2u);
    EXPECT_DOUBLE_EQ(ps.paths.front().polygon.points[1].x, 20.0);

    // buffered_outline (transformed from RenderedShape::path_outlines,
    // computed via Geometry::path_to_polygons at generate_shapes time) -
    // a flat-ended, width-4 buffer of dbu (0,0)-(10,0) is the dbu rect
    // (0,-2)-(10,2); at scale 2.0/pan (0,0) that's pixel (0,-4)-(20,4).
    // Checked via bbox, not exact point order/winding, since that's an
    // internal Boost.Geometry buffer detail this test shouldn't couple to.
    ASSERT_EQ(ps.paths.front().buffered_outline.size(), 1u);
    const auto &outline_points = ps.paths.front().buffered_outline.front().points;
    ASSERT_FALSE(outline_points.empty());
    double min_x = outline_points.front().x, max_x = outline_points.front().x;
    double min_y = outline_points.front().y, max_y = outline_points.front().y;
    for (const auto &pt : outline_points)
    {
        min_x = std::min(min_x, pt.x);
        max_x = std::max(max_x, pt.x);
        min_y = std::min(min_y, pt.y);
        max_y = std::max(max_y, pt.y);
    }
    EXPECT_DOUBLE_EQ(min_x, 0.0);
    EXPECT_DOUBLE_EQ(max_x, 20.0);
    EXPECT_DOUBLE_EQ(min_y, -4.0);
    EXPECT_DOUBLE_EQ(max_y, 4.0);

    ASSERT_EQ(ps.texts.size(), 1u);
    EXPECT_EQ(ps.texts.front().label, "note");
    EXPECT_DOUBLE_EQ(ps.texts.front().location.x, 10.0);
    EXPECT_DOUBLE_EQ(ps.texts.front().location.y, 10.0);
    // 20 dbu * scale 2.0 * kLabelWidthRatio (0.6) = 24.0 - well above the
    // minimum floor, so the ratio (not the floor) determines this value.
    EXPECT_DOUBLE_EQ(ps.texts.front().size, 24.0);
}

TEST_F(RenderFixture, TransformToPixelsFloorsTinyTextSizeToAMinimumPixelSize)
{
    // A hair-thin label size (e.g. from a very thin path/polygon arm) must
    // not shrink to an unreadable speck - transform_to_pixels clamps to a
    // minimum pixel size regardless of how small the scaled geometry size
    // would otherwise be. 1 dbu * scale 1.0 * ratio would be well under 1px
    // unclamped; assert it's floored to something actually legible instead.
    add_obstruction_shape(Shape{
        .layer_name = "M1",
        .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}},
        .texts = {Text{.label = "tiny", .location = {5, 5}, .size = 1}},
    });

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);

    const auto &shapes = pipeline.run(root, scene, view_layers);
    const auto &pixel_shapes = renderer.transform_to_pixels(shapes, scene);

    ASSERT_EQ(pixel_shapes.size(), 1u);
    const auto &ps = pixel_shapes.begin()->second.front();
    ASSERT_EQ(ps.texts.size(), 1u);
    EXPECT_GT(ps.texts.front().size, 5.0); // unclamped would be 1*1.0*0.6 = 0.6
}

TEST_F(RenderFixture, TransformToPixelsReusesCacheUntilViewportVersionChanges)
{
    add_obstruction_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {10, 10}, .ur = {30, 30}}}});

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);

    const auto &shapes = pipeline.run(root, scene, view_layers);
    renderer.transform_to_pixels(shapes, scene);
    renderer.transform_to_pixels(shapes, scene);
    EXPECT_EQ(renderer.transform_calls(), 1u);

    scene.set_pan(Point{1, 1});
    renderer.transform_to_pixels(shapes, scene);
    EXPECT_EQ(renderer.transform_calls(), 2u);
}

TEST_F(RenderFixture, TransformTinyShapesToPixelsAppliesPanAndScale)
{
    // Sub-pixel dot at dbu (25,25) - 0x0 bbox.
    add_terminal_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {25, 25}, .ur = {25, 25}}}});

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{5, 5});
    scene.set_scale(2.0);
    scene.set_viewport_size(200, 200);

    const auto &tiny_shapes = pipeline.run_tiny_shapes(root, scene, view_layers);
    const auto &tiny_pixel_shapes = renderer.transform_tiny_shapes_to_pixels(tiny_shapes, scene);

    ASSERT_EQ(tiny_pixel_shapes.size(), 1u);
    const auto &group = tiny_pixel_shapes.begin()->second;
    ASSERT_EQ(group.size(), 1u);
    EXPECT_DOUBLE_EQ(group.front().x, (25 - 5) * 2.0);
    EXPECT_DOUBLE_EQ(group.front().y, (25 - 5) * 2.0);
}

TEST_F(RenderFixture, TransformTinyShapesToPixelsReusesCacheUntilViewportVersionChanges)
{
    add_terminal_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {5, 5}, .ur = {5, 5}}}});

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);

    const auto &tiny_shapes = pipeline.run_tiny_shapes(root, scene, view_layers);
    renderer.transform_tiny_shapes_to_pixels(tiny_shapes, scene);
    renderer.transform_tiny_shapes_to_pixels(tiny_shapes, scene);
    EXPECT_EQ(renderer.tiny_shapes_transform_calls(), 1u);

    scene.set_pan(Point{1, 1});
    renderer.transform_tiny_shapes_to_pixels(tiny_shapes, scene);
    EXPECT_EQ(renderer.tiny_shapes_transform_calls(), 2u);
}

TEST_F(RenderFixture, BuildTinyShapesPictureDrawsASingleOpaquePixelAtEachDotLocation)
{
    add_terminal_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {40, 40}, .ur = {40, 40}}}});

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);

    const auto &tiny_shapes = pipeline.run_tiny_shapes(root, scene, view_layers);
    const auto &tiny_pixel_shapes = renderer.transform_tiny_shapes_to_pixels(tiny_shapes, scene);
    const auto &picture = renderer.build_tiny_shapes_picture(tiny_pixel_shapes, scene, view_layers);

    ASSERT_EQ(tiny_pixel_shapes.size(), 1u);
    const ViewLayerData *view_layer = view_layers.get(tiny_pixel_shapes.begin()->first);
    ASSERT_NE(view_layer, nullptr);

    SkColor pixel = sample_pixel(picture, 100, 100, 40, 40);
    EXPECT_EQ(pixel, to_sk_color(view_layer->style.outline_color));
}

TEST_F(RenderFixture, BuildTinyShapesPictureIsEmptyWhenThereAreNoTinyShapes)
{
    add_terminal_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {10, 10}, .ur = {30, 30}}}}); // normal-sized, not tiny

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);

    const auto &tiny_shapes = pipeline.run_tiny_shapes(root, scene, view_layers);
    ASSERT_TRUE(tiny_shapes.empty());
    const auto &tiny_pixel_shapes = renderer.transform_tiny_shapes_to_pixels(tiny_shapes, scene);
    const auto &picture = renderer.build_tiny_shapes_picture(tiny_pixel_shapes, scene, view_layers);

    SkColor pixel = sample_pixel(picture, 100, 100, 20, 20);
    EXPECT_EQ(SkColorGetA(pixel), 0);
}

TEST_F(RenderFixture, BuildTinyShapesPictureRespectsLayerVisibility)
{
    add_terminal_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {40, 40}, .ur = {40, 40}}}});

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);
    scene.set_layer_name_visible("M1", false);

    const auto &tiny_shapes = pipeline.run_tiny_shapes(root, scene, view_layers);
    ASSERT_TRUE(tiny_shapes.empty()); // whole M1 group dropped by tiny_shapes_by_layer_visibility

    const auto &tiny_pixel_shapes = renderer.transform_tiny_shapes_to_pixels(tiny_shapes, scene);
    const auto &picture = renderer.build_tiny_shapes_picture(tiny_pixel_shapes, scene, view_layers);

    SkColor pixel = sample_pixel(picture, 100, 100, 40, 40);
    EXPECT_EQ(SkColorGetA(pixel), 0);
}

TEST_F(RenderFixture, BuildPictureFillsInteriorPixelWithLayerStyleColor)
{
    add_terminal_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {10, 10}, .ur = {30, 30}}}});

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);

    const auto &shapes = pipeline.run(root, scene, view_layers);
    const auto &pixel_shapes = renderer.transform_to_pixels(shapes, scene);
    const auto &picture = renderer.build_picture(pixel_shapes, scene, view_layers, root);

    // M1 is ROUTING, so its TERMINAL fill is a tiled diagonal-stripe
    // pattern (see FillPattern), not a flat wash - scan a strip inside the
    // 10..30 rect (away from both the outline hairline and the label at
    // the centroid, 20,20) for the layer's own color instead of asserting
    // on one exact pixel.
    SkBitmap bitmap = rasterize(picture, 100, 100);
    const ViewLayerData *view_layer = view_layers.get(shapes.begin()->first);
    ASSERT_NE(view_layer, nullptr);
    EXPECT_TRUE(region_shows_color(bitmap, 11, 11, 15, 29, to_sk_color(view_layer->style.outline_color)));
}

TEST_F(RenderFixture, BuildPictureDrawsEachLayerGroupWithItsOwnStyle)
{
    add_obstruction_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {10, 10}, .ur = {30, 30}}}});
    add_terminal_shape(Shape{.layer_name = "M2", .rects = {Rect{.ll = {50, 10}, .ur = {70, 30}}}}); // non-overlapping

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);

    const auto &shapes = pipeline.run(root, scene, view_layers);
    ASSERT_EQ(shapes.size(), 2u); // two distinct groups: M1/OBSTRUCTION, M2/TERMINAL
    const auto &pixel_shapes = renderer.transform_to_pixels(shapes, scene);
    const auto &picture = renderer.build_picture(pixel_shapes, scene, view_layers, root);
    SkBitmap bitmap = rasterize(picture, 100, 100);

    // M1/OBSTRUCTION is BRICK (any OBSTRUCTION is); M2/TERMINAL is ROUTING's
    // diagonal stripes - neither covers every pixel, so scan each shape's
    // own rect (inset from its outline hairline) for its own color rather
    // than sampling one exact point.
    SkColor m1_color = to_sk_color(view_layers.get(view_layers.find(m1, ViewLayerPurpose::OBSTRUCTION))->style.outline_color);
    SkColor m2_color = to_sk_color(view_layers.get(view_layers.find(m2, ViewLayerPurpose::TERMINAL))->style.outline_color);

    EXPECT_TRUE(region_shows_color(bitmap, 11, 11, 29, 29, m1_color));
    EXPECT_FALSE(region_shows_color(bitmap, 11, 11, 29, 29, m2_color)); // M2's color doesn't leak into M1's rect

    EXPECT_TRUE(region_shows_color(bitmap, 51, 11, 69, 29, m2_color));
    EXPECT_FALSE(region_shows_color(bitmap, 51, 11, 69, 29, m1_color)); // M1's color doesn't leak into M2's rect
}

TEST_F(RenderFixture, BuildPictureDrawsAPatternFilledOutlinedPathWithACenterline)
{
    // Regression: draw_group used to draw an outline-colored "border"
    // stroke almost as wide as the path itself, directly underneath a
    // pattern-shaded stroke at the path's own width - since both use the
    // same base color (a layer's outline_color is also pattern_shader's
    // own tile color), the border acted as an opaque same-color backing
    // plate showing straight through every transparent gap in the
    // pattern, so a PATH always rendered as one solid block regardless of
    // its layer's fill pattern. Fixed to fill/outline a PATH's buffered
    // outline the same way a real POLYGON already renders (real pattern
    // fill, thin boundary), plus a centerline so it still reads as a wire.
    add_obstruction_shape(Shape{.layer_name = "M1", .paths = {Path{.polygon = Polygon{.points = {{10, 20}, {30, 20}}}, .width = 4}}});

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);

    const auto &shapes = pipeline.run(root, scene, view_layers);
    const auto &pixel_shapes = renderer.transform_to_pixels(shapes, scene);
    const auto &picture = renderer.build_picture(pixel_shapes, scene, view_layers, root);
    SkBitmap bitmap = rasterize(picture, 100, 100);

    const SkColor outline_color = to_sk_color(view_layers.get(view_layers.find(m1, ViewLayerPurpose::OBSTRUCTION))->style.outline_color);

    // Buffered outline (flat ends, width 4 -> +/-2 perpendicular) is
    // dbu/pixel (10,18)-(30,22) at this pan/scale. This path is narrow
    // (width 4) - too thin to also test "fill is patterned, not solid"
    // here without the boundary/centerline dominating the whole strip;
    // that regression is covered separately, with a wide-enough path to
    // leave real room for it (see
    // BuildPictureFillsAShortWidePathWithAPatternNotASolidBlock below).
    EXPECT_TRUE(region_shows_color(bitmap, 15, 17, 25, 19, outline_color)); // top boundary
    EXPECT_TRUE(region_shows_color(bitmap, 15, 21, 25, 23, outline_color)); // bottom boundary

    // Well beyond even the buffered width - nothing drawn.
    EXPECT_EQ(SkColorGetA(sample_pixel(picture, 100, 100, 20, 10)), 0);

    // Centerline: a continuous stroke along dbu y=20 (the path's own
    // polygon, unaffected by the fill pattern's phase) - checked by
    // fraction of the row actually covered rather than a single pixel,
    // since BRICK's own mortar joints are also full-width horizontal
    // lines and could coincidentally land on any given row. The
    // centerline row should be covered edge-to-edge; comparing against
    // neighboring rows isn't reliable (a mortar joint could legitimately
    // land on any of them), so this only asserts the centerline's own
    // row is (near-)fully covered - proving *a* continuous horizontal
    // stroke exists exactly at y=20, not just pattern coverage in
    // general (already proven transparent elsewhere in this same row
    // range above).
    int covered = 0;
    for (int x = 11; x <= 29; ++x)
        if (SkColorGetA(bitmap.getColor(x, 20)) > 0)
            ++covered;
    EXPECT_GT(covered, 15); // most of the 19px span, not just scattered pattern dots
}

TEST_F(RenderFixture, BuildPictureFillsAShortWidePathWithAPatternNotASolidBlock)
{
    // Regression: the actual reported bug - a stress-test-shaped path
    // whose width is comparable to its own length (unlike a real wire,
    // much longer than it is wide) rendered as one solid-colored block
    // with no visible fill pattern at all, since the old wide
    // outline-colored "border" stroke and the pattern-shaded "wire"
    // stroke shared the same base color - the border showed straight
    // through every transparent gap in the pattern. Width=length=20,
    // matching stress_data.hpp's own synthetic path shape exactly.
    add_obstruction_shape(Shape{.layer_name = "M1", .paths = {Path{.polygon = Polygon{.points = {{10, 20}, {30, 20}}}, .width = 20}}});

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);

    const auto &shapes = pipeline.run(root, scene, view_layers);
    const auto &pixel_shapes = renderer.transform_to_pixels(shapes, scene);
    const auto &picture = renderer.build_picture(pixel_shapes, scene, view_layers, root);
    SkBitmap bitmap = rasterize(picture, 100, 100);

    // Buffered outline is dbu/pixel (10,10)-(30,30) - well inside it
    // (away from the boundary hairline and the y=20 centerline), a solid
    // block would show outline_color at every pixel; a real pattern must
    // show at least one fully transparent gap too.
    bool found_gap = false;
    for (int x = 14; x <= 26 && !found_gap; ++x)
        for (int y = 14; y <= 18 && !found_gap; ++y)
            if (SkColorGetA(bitmap.getColor(x, y)) == 0)
                found_gap = true;
    EXPECT_TRUE(found_gap);
}

TEST_F(RenderFixture, BuildPictureDrawsTerminalLabelAsOpaqueTextOverTranslucentFill)
{
    TerminalId terminal_id = root.create_terminal(TerminalData{.abstract = abstract_id, .name = "A1"});
    root.create_terminal_port(TerminalPortData{.terminal = terminal_id, .shapes = {Shape{.layer_name = "M1", .rects = {Rect{.ll = {10, 10}, .ur = {30, 30}}}}}});

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);

    const auto &shapes = pipeline.run(root, scene, view_layers);
    const auto &pixel_shapes = renderer.transform_to_pixels(shapes, scene);
    const auto &picture = renderer.build_picture(pixel_shapes, scene, view_layers, root);

    // The label is drawn at the shape's centroid (20,20), using the
    // opaque outline color (alpha 255) - unlike the surrounding translucent
    // fill (alpha 120 for TERMINAL). Scan a small interior region (1px
    // inset from the rect's own hairline outline at the 10/30 edges, so
    // that stroke's own opaque pixels aren't mistaken for text) for any
    // fully-opaque pixel, rather than asserting on one exact glyph pixel -
    // precise glyph rasterization (AA/hinting) isn't this test's concern.
    SkBitmap bitmap = rasterize(picture, 100, 100);
    bool found_opaque_pixel = false;
    for (int y = 11; y <= 29 && !found_opaque_pixel; ++y)
        for (int x = 11; x <= 29 && !found_opaque_pixel; ++x)
            if (SkColorGetA(bitmap.getColor(x, y)) == 255)
                found_opaque_pixel = true;

    EXPECT_TRUE(found_opaque_pixel);
}

TEST_F(RenderFixture, BuildPictureDrawsACrossAtEachLabelsOwnAnchorPoint)
{
    TerminalId terminal_id = root.create_terminal(TerminalData{.abstract = abstract_id, .name = "A1"});
    root.create_terminal_port(TerminalPortData{.terminal = terminal_id, .shapes = {Shape{.layer_name = "M1", .rects = {Rect{.ll = {10, 10}, .ur = {30, 30}}}}}});

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);

    const auto &shapes = pipeline.run(root, scene, view_layers);
    const auto &pixel_shapes = renderer.transform_to_pixels(shapes, scene);
    const auto &picture = renderer.build_picture(pixel_shapes, scene, view_layers, root);

    // A single rect's label lands at its exact center (20,20) under the
    // new get_label_location (UPDATES.md item 8.2 - no fracturing
    // needed for a lone rect). The cross's horizontal arm spans local
    // x in [-4,4] (kLabelOriginMarkerSizePx=8) - sample a point on its
    // *left* side (global x=17, i.e. local x=-3), since drawString's
    // baseline-left origin means the "A1" glyphs are drawn to the right
    // of the anchor, not the left, so this can't be mistaken for glyph
    // pixels the way a point on the right side could.
    const ViewLayerId m1_terminal = view_layers.find(m1, ViewLayerPurpose::TERMINAL);
    const ViewLayerData *view_layer = view_layers.get(m1_terminal);
    ASSERT_NE(view_layer, nullptr);

    const SkColor cross_pixel = sample_pixel(picture, 100, 100, 17, 20);
    EXPECT_EQ(cross_pixel, to_sk_color(view_layer->style.outline_color));

    // Far from the label/cross entirely - nothing drawn here.
    EXPECT_EQ(SkColorGetA(sample_pixel(picture, 100, 100, 90, 90)), 0);
}

TEST_F(RenderFixture, BuildPictureSkipsShapesWithUnresolvedViewLayer)
{
    add_obstruction_shape(Shape{.layer_name = "DOES_NOT_EXIST", .rects = {Rect{.ll = {10, 10}, .ur = {30, 30}}}});

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);

    const auto &shapes = pipeline.run(root, scene, view_layers);
    ASSERT_EQ(shapes.size(), 1u); // kept by the layer filter despite an invalid ViewLayerId
    const auto &pixel_shapes = renderer.transform_to_pixels(shapes, scene);
    const auto &picture = renderer.build_picture(pixel_shapes, scene, view_layers, root);

    SkColor pixel = sample_pixel(picture, 100, 100, 20, 20);
    EXPECT_EQ(SkColorGetA(pixel), 0); // nothing drawn - no style to draw it with
}

TEST_F(RenderFixture, ComposeWithOverlaysOutlinesOnlyTheSelectedPieceNotTheWholeObject)
{
    const TerminalId terminal_id = root.create_terminal(TerminalData{.abstract = abstract_id});
    const Shape first_piece{.layer_name = "M1", .rects = {Rect{.ll = {10, 10}, .ur = {30, 30}}}};
    root.create_terminal_port(TerminalPortData{.terminal = terminal_id, .shapes = {first_piece}});
    root.create_terminal_port(TerminalPortData{.terminal = terminal_id, .shapes = {Shape{.layer_name = "M1", .rects = {Rect{.ll = {60, 60}, .ur = {80, 80}}}}}});

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);
    scene.select(terminal_id, first_piece);

    const auto &shapes = pipeline.run(root, scene, view_layers);
    const auto &pixel_shapes = renderer.transform_to_pixels(shapes, scene);
    const auto &design_picture = renderer.build_picture(pixel_shapes, scene, view_layers, root);
    const auto &overlay_picture = renderer.build_overlay_picture(scene);
    const auto &selection_overlay_picture = renderer.build_selection_overlay_picture(scene);
    const PixelBuffer &buffer = renderer.compose_with_overlays(design_picture, sk_sp<SkPicture>{}, overlay_picture, selection_overlay_picture, scene);

    // Post-flip (see the Y-flip comment on
    // ComposeWithOverlaysReflectsASelectionChangeEvenWhenComposedOnceBefore) -
    // first rect's left edge (preflip x=9,y=20) -> post-flip y = 100-20 = 80;
    // second rect's left edge (preflip x=59,y=70) -> post-flip y = 100-70 = 30.
    auto is_white = [&](int x, int y)
    {
        const uint8_t *p = buffer.data + static_cast<size_t>(y) * static_cast<size_t>(buffer.row_bytes) + static_cast<size_t>(x) * 4;
        return p[0] == 255 && p[1] == 255 && p[2] == 255 && p[3] > 200;
    };

    EXPECT_TRUE(is_white(9, 80));   // first rect's left edge - the selected piece
    EXPECT_FALSE(is_white(59, 30)); // second rect's left edge - same Terminal, not the selected piece
}

TEST_F(RenderFixture, ComposeWithOverlaysOutlinesAPolygonSelectedPiece)
{
    // draw_selected_piece_outline's polygon branch, otherwise untested -
    // every other piece-outline test above uses a rect-only piece.
    const TerminalId terminal_id = root.create_terminal(TerminalData{.abstract = abstract_id});
    const Shape polygon_piece{.layer_name = "M1", .polygons = {Polygon{.points = {{10, 10}, {30, 10}, {30, 30}, {10, 30}}}}};
    root.create_terminal_port(TerminalPortData{.terminal = terminal_id, .shapes = {polygon_piece}});

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);
    scene.select(terminal_id, polygon_piece);

    const auto &shapes = pipeline.run(root, scene, view_layers);
    const auto &pixel_shapes = renderer.transform_to_pixels(shapes, scene);
    const auto &design_picture = renderer.build_picture(pixel_shapes, scene, view_layers, root);
    const auto &overlay_picture = renderer.build_overlay_picture(scene);
    const auto &selection_overlay_picture = renderer.build_selection_overlay_picture(scene);
    const PixelBuffer &buffer = renderer.compose_with_overlays(design_picture, sk_sp<SkPicture>{}, overlay_picture, selection_overlay_picture, scene);

    // Same square as the rect-piece tests above (10,10)-(30,30) - left
    // edge preflip (9,20) -> post-flip y = 100-20 = 80.
    const uint8_t *p = buffer.data + static_cast<size_t>(80) * static_cast<size_t>(buffer.row_bytes) + static_cast<size_t>(9) * 4;
    EXPECT_EQ(p[0], 255);
    EXPECT_EQ(p[1], 255);
    EXPECT_EQ(p[2], 255);
    EXPECT_GT(p[3], 200);
}

TEST_F(RenderFixture, ComposeWithOverlaysOutlinesAPathSelectedPieceHollow)
{
    // draw_selected_piece_outline's path branch, otherwise untested -
    // every other piece-outline test above uses a rect/polygon piece.
    // Traces the path's *buffered outline polygon* (like
    // draw_hover_outline), not a solid halo stroke along its centerline -
    // so the outline is hollow (edges white, interior not), matching
    // rect/polygon selection instead of reading as a filled blob.
    const TerminalId terminal_id = root.create_terminal(TerminalData{.abstract = abstract_id});
    const Shape path_piece{.layer_name = "M1", .paths = {Path{.polygon = Polygon{.points = {{10, 20}, {30, 20}}}, .width = 4}}};
    root.create_terminal_port(TerminalPortData{.terminal = terminal_id, .shapes = {path_piece}});

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);
    scene.select(terminal_id, path_piece);

    const auto &shapes = pipeline.run(root, scene, view_layers);
    const auto &pixel_shapes = renderer.transform_to_pixels(shapes, scene);
    const auto &design_picture = renderer.build_picture(pixel_shapes, scene, view_layers, root);
    const auto &overlay_picture = renderer.build_overlay_picture(scene);
    const auto &selection_overlay_picture = renderer.build_selection_overlay_picture(scene);
    const PixelBuffer &buffer = renderer.compose_with_overlays(design_picture, sk_sp<SkPicture>{}, overlay_picture, selection_overlay_picture, scene);

    auto is_white = [&](int x, int y)
    {
        const uint8_t *p = buffer.data + static_cast<size_t>(y) * static_cast<size_t>(buffer.row_bytes) + static_cast<size_t>(x) * 4;
        return p[0] == 255 && p[1] == 255 && p[2] == 255 && p[3] > 200;
    };

    // Buffered outline (width 4 -> +/-2 perpendicular, flat ends) is
    // roughly dbu (10,18)-(30,22) - post-flip (viewport 100, scale 1.0):
    // top edge preflip y=22 -> device y=78; bottom edge preflip y=18 ->
    // device y=82; centerline preflip y=20 -> device y=80.
    EXPECT_TRUE(is_white(20, 78));   // top edge of the outline
    EXPECT_TRUE(is_white(20, 82));   // bottom edge of the outline
    EXPECT_FALSE(is_white(20, 80));  // centerline - hollow interior, not filled
    EXPECT_FALSE(is_white(20, 60));  // well outside the path entirely
}

TEST_F(RenderFixture, ComposeWithOverlaysOutlinesAShortWidePathHollowNotFilled)
{
    // Regression: the actual reported bug - a stress-test-shaped path
    // whose width is comparable to its own length (unlike a real wire,
    // which is much longer than it is wide) used to render as a solid
    // white blob, since the old halo approach stroked the centerline at
    // (width + margin), which swallows a short/wide path's whole
    // footprint. Center must now read as hollow, just like the more
    // typical long/thin case above.
    const TerminalId terminal_id = root.create_terminal(TerminalData{.abstract = abstract_id});
    const Shape path_piece{.layer_name = "M1", .paths = {Path{.polygon = Polygon{.points = {{10, 20}, {30, 20}}}, .width = 20}}};
    root.create_terminal_port(TerminalPortData{.terminal = terminal_id, .shapes = {path_piece}});

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);
    scene.select(terminal_id, path_piece);

    const auto &shapes = pipeline.run(root, scene, view_layers);
    const auto &pixel_shapes = renderer.transform_to_pixels(shapes, scene);
    const auto &design_picture = renderer.build_picture(pixel_shapes, scene, view_layers, root);
    const auto &overlay_picture = renderer.build_overlay_picture(scene);
    const auto &selection_overlay_picture = renderer.build_selection_overlay_picture(scene);
    const PixelBuffer &buffer = renderer.compose_with_overlays(design_picture, sk_sp<SkPicture>{}, overlay_picture, selection_overlay_picture, scene);

    auto is_white = [&](int x, int y)
    {
        const uint8_t *p = buffer.data + static_cast<size_t>(y) * static_cast<size_t>(buffer.row_bytes) + static_cast<size_t>(x) * 4;
        return p[0] == 255 && p[1] == 255 && p[2] == 255 && p[3] > 200;
    };

    // width=20, length=20 (matching the stress test's width~=length
    // shape) -> buffered outline roughly dbu (10,10)-(30,30), centered
    // at (20,20) - post-flip center device y = 100-20 = 80.
    EXPECT_FALSE(is_white(20, 80)); // dead center - must be hollow, not a filled blob
}

TEST_F(RenderFixture, BuildPictureAndRasterizeAreNotInvalidatedBySelectionChanges)
{
    // build_picture has no selection-dependent content at all any more
    // (see its own doc comment - the old whole-object selection-outline
    // pass was both unreachable through the public API and a real,
    // measured O(visible shapes * selection size) cost on every selection
    // change, see BENCHMARKS.md), so neither its own cache key nor
    // rasterize_frame's (kept in sync by hand) includes selection_version
    // any more - a selection change should cost nothing here.
    const TerminalId terminal_id = add_terminal_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {10, 10}, .ur = {30, 30}}}});

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);

    const auto &shapes = pipeline.run(root, scene, view_layers);
    const auto &pixel_shapes = renderer.transform_to_pixels(shapes, scene);
    const auto &picture = renderer.build_picture(pixel_shapes, scene, view_layers, root);
    renderer.rasterize(picture, scene);
    ASSERT_EQ(renderer.picture_calls(), 1u);
    ASSERT_EQ(renderer.rasterize_calls(), 1u);

    scene.select(terminal_id);
    const auto &picture_after = renderer.build_picture(pixel_shapes, scene, view_layers, root);
    renderer.rasterize(picture_after, scene);

    EXPECT_EQ(renderer.picture_calls(), 1u);
    EXPECT_EQ(renderer.rasterize_calls(), 1u);
}

TEST_F(RenderFixture, BuildPictureReusesCacheUntilVisibilityVersionChanges)
{
    add_obstruction_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {10, 10}, .ur = {30, 30}}}});

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);

    const auto &shapes = pipeline.run(root, scene, view_layers);
    const auto &pixel_shapes = renderer.transform_to_pixels(shapes, scene);
    renderer.build_picture(pixel_shapes, scene, view_layers, root);
    renderer.build_picture(pixel_shapes, scene, view_layers, root);
    EXPECT_EQ(renderer.picture_calls(), 1u);

    scene.set_layer_name_visible("M1", false);
    renderer.build_picture(pixel_shapes, scene, view_layers, root);
    EXPECT_EQ(renderer.picture_calls(), 2u);
}

TEST_F(RenderFixture, BuildPictureDrawsSolidAxisLinesAtDbuOrigin)
{
    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);

    const auto &shapes = pipeline.run(root, scene, view_layers); // empty Abstract
    const auto &pixel_shapes = renderer.transform_to_pixels(shapes, scene);
    const auto &picture = renderer.build_picture(pixel_shapes, scene, view_layers, root);

    // pan=(0,0), scale=1.0 -> dbu (x=0) is pixel column 0, dbu (y=0) is
    // pixel row 0 (sample_pixel reads the picture's own unflipped pixel
    // space directly - see its own comment).
    EXPECT_GT(SkColorGetA(sample_pixel(picture, 100, 100, 0, 50)), 0); // on the vertical (x=0) axis line
    EXPECT_GT(SkColorGetA(sample_pixel(picture, 100, 100, 50, 0)), 0); // on the horizontal (y=0) axis line
}

TEST_F(RenderFixture, BuildPictureDrawsAMajorGridDotAtAKnownLatticePoint)
{
    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{0, 0});
    scene.set_scale(2.0); // minor (5dbu*2=10px) and major (50dbu*2=100px) both clear the density floor
    scene.set_viewport_size(200, 200);

    const auto &shapes = pipeline.run(root, scene, view_layers); // empty Abstract
    const auto &pixel_shapes = renderer.transform_to_pixels(shapes, scene);
    const auto &picture = renderer.build_picture(pixel_shapes, scene, view_layers, root);

    // dbu (50,50) is on the default major lattice (multiple of 50) but
    // off both axes -> pixel (100,100) at this pan/scale.
    EXPECT_GT(SkColorGetA(sample_pixel(picture, 200, 200, 100, 100)), 0);
}

TEST_F(RenderFixture, BuildPictureHidesBothGridTiersWhenTooDenseButKeepsAxisLines)
{
    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{0, 0});
    scene.set_scale(0.1); // minor 5*0.1=0.5px, major 50*0.1=5px - both under the 8px floor
    scene.set_viewport_size(100, 100);

    const auto &shapes = pipeline.run(root, scene, view_layers); // empty Abstract
    const auto &pixel_shapes = renderer.transform_to_pixels(shapes, scene);
    const auto &picture = renderer.build_picture(pixel_shapes, scene, view_layers, root);

    // dbu (500,500) -> pixel (50,50) at this scale, and 500 is a multiple
    // of 50 (would be a major dot if that tier weren't suppressed) - away
    // from either axis line, so this isolates the density cutoff itself.
    EXPECT_EQ(SkColorGetA(sample_pixel(picture, 100, 100, 50, 50)), 0);
}

TEST_F(RenderFixture, BuildPictureHidesMinorTierIndependentlyOfMajorTier)
{
    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{0, 0});
    scene.set_scale(0.5); // minor 5*0.5=2.5px (hidden), major 50*0.5=25px (shown)
    scene.set_viewport_size(100, 100);

    const auto &shapes = pipeline.run(root, scene, view_layers); // empty Abstract
    const auto &pixel_shapes = renderer.transform_to_pixels(shapes, scene);
    const auto &picture = renderer.build_picture(pixel_shapes, scene, view_layers, root);

    // dbu (100,100) is on the major lattice (multiple of 50), off-axis,
    // -> pixel (50,50) at this pan/scale: still drawn even though minor
    // dots are hidden, proving the two tiers gate independently.
    EXPECT_GT(SkColorGetA(sample_pixel(picture, 100, 100, 50, 50)), 0);
}

TEST_F(RenderFixture, BuildPictureDrawsOriginMarkerAtTheAbstractsOwnOrigin)
{
    // Not (0,0) - proves the marker tracks AbstractData::origin (UPDATES.md
    // 5.4: "not necessarily at 0,0"), not a hardcoded dbu (0,0).
    const AbstractId origin_abstract_id = root.create_abstract(AbstractData{.origin = Point{33, 71}});

    Scene scene;
    scene.set_current_abstract(origin_abstract_id);
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);

    const auto &shapes = pipeline.run(root, scene, view_layers); // empty Abstract
    const auto &pixel_shapes = renderer.transform_to_pixels(shapes, scene);
    const auto &picture = renderer.build_picture(pixel_shapes, scene, view_layers, root);

    // Origin at dbu (33,71) -> pixel (33,71) at this pan/scale
    // (build_picture's own pre-flip pixel space, no Y-flip). Sampled on
    // the horizontal arm, away from the vertical arm's own stroke.
    const SkColor marker_color = sample_pixel(picture, 100, 100, 40, 71);
    EXPECT_EQ(SkColorGetR(marker_color), 255);
    EXPECT_EQ(SkColorGetG(marker_color), 200);
    EXPECT_EQ(SkColorGetB(marker_color), 0);
    EXPECT_EQ(SkColorGetA(marker_color), 255);

    // Far from the marker's actual position and off any grid dot/axis
    // line - nothing drawn here.
    EXPECT_EQ(SkColorGetA(sample_pixel(picture, 100, 100, 5, 90)), 0);
}

TEST_F(RenderFixture, BuildPictureOmitsOriginMarkerWhenNoAbstractIsSelected)
{
    Scene scene; // current_abstract() left default/invalid - no Design selected
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);

    const auto &shapes = pipeline.run(root, scene, view_layers); // degrades gracefully to empty
    const auto &pixel_shapes = renderer.transform_to_pixels(shapes, scene);
    const auto &picture = renderer.build_picture(pixel_shapes, scene, view_layers, root);

    // Would show the marker's default-origin (0,0) position if
    // root.get_abstract(scene.current_abstract()) weren't null-checked -
    // off any grid dot/axis line, so nothing else could account for alpha
    // being nonzero here either.
    EXPECT_EQ(SkColorGetA(sample_pixel(picture, 100, 100, 5, 5)), 0);
}

TEST_F(RenderFixture, BuildOverlayPictureDrawsAFixedSizeBoxCenteredOnTheSnappedMousePosition)
{
    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{0, 0});
    scene.set_scale(2.0);
    scene.set_viewport_size(100, 100);
    scene.set_minor_grid_spacing(10);

    // Pixel (40,40) -> dbu (20,30) via mouse_dbu_position's pan/scale/Y-flip
    // inverse - already a multiple of the 10-spacing minor grid, so it
    // snaps to itself, landing at pixel (40,60) in draw_cursor's own
    // pre-flip pixel space (not the Y-flipped image space rasterize()
    // produces). The box is a fixed kCursorBoxSizePx (7px) square
    // centered there - rect left edge at 40-3.5=36.5, stroke (1px)
    // spanning continuous x in [36.0,37.0], so pixel column 36 is fully
    // covered.
    scene.set_mouse_position(40, 40);

    const auto &overlay_picture = renderer.build_overlay_picture(scene);

    EXPECT_GT(SkColorGetA(sample_pixel(overlay_picture, 100, 100, 36, 60)), 0); // left edge of the fixed-size box
    EXPECT_EQ(SkColorGetA(sample_pixel(overlay_picture, 100, 100, 40, 60)), 0); // interior (stroke only, no fill) - where the grid dot itself shows through
}

TEST_F(RenderFixture, BuildOverlayPictureBoxStaysFixedPixelSizeAtHighZoomInsteadOfBallooningWithTheGridCell)
{
    // Regression: the box used to be sized in dbu-space (+-minor_spacing/2
    // around the snapped point), so at high zoom it ballooned along with
    // the grid cell. It's now a fixed on-screen size (kCursorBoxSizePx)
    // regardless of scale.
    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{0, 0});
    scene.set_scale(25.0);
    scene.set_viewport_size(400, 400);
    scene.set_minor_grid_spacing(8);

    // dbu (8,8) -> pixel (200,200) at this scale/pan, and 8 is already a
    // multiple of the 8-spacing minor grid, so it snaps to itself. The
    // old dbu-sized box (+-4dbu = +-100px at this scale) would have
    // covered pixel (210,200); the new fixed-size box does not.
    scene.set_mouse_position(200, 200);

    const auto &overlay_picture = renderer.build_overlay_picture(scene);

    EXPECT_GT(SkColorGetA(sample_pixel(overlay_picture, 400, 400, 196, 200)), 0); // just inside the fixed box's edge
    EXPECT_EQ(SkColorGetA(sample_pixel(overlay_picture, 400, 400, 210, 200)), 0); // far outside the fixed box, but well within where the old dbu-sized box would have drawn
}

TEST_F(RenderFixture, BuildOverlayPictureIsEmptyWhenNoMousePositionSet)
{
    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_viewport_size(100, 100);

    const auto &overlay_picture = renderer.build_overlay_picture(scene);
    EXPECT_EQ(SkColorGetA(sample_pixel(overlay_picture, 100, 100, 50, 50)), 0);
}

TEST_F(RenderFixture, BuildOverlayPictureDrawsEvenWhenTheMinorGridIsTooDenseToShowDots)
{
    // Unlike draw_grid's own dots (which hide below a density floor), the
    // mouse marker is meant to stay visible at all times regardless of
    // whether the grid itself is currently shown.
    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{0, 0});
    scene.set_scale(0.1); // minor 5*0.1=0.5px - under draw_grid's own minor-tier density floor
    scene.set_viewport_size(100, 100);

    // dbu (500,500) -> pixel (50,50) at this pan/scale, and 500 is already
    // a multiple of the default 5dbu minor spacing, so it snaps to itself.
    scene.set_mouse_position(50, 50);

    const auto &overlay_picture = renderer.build_overlay_picture(scene);

    EXPECT_GT(SkColorGetA(sample_pixel(overlay_picture, 100, 100, 46, 50)), 0); // left edge of the fixed-size box
    EXPECT_EQ(SkColorGetA(sample_pixel(overlay_picture, 100, 100, 50, 50)), 0); // interior (stroke only, no fill)
}

TEST_F(RenderFixture, BuildOverlayPictureDrawsHoverOutlineAroundTheHoveredShape)
{
    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{0, 0});
    scene.set_scale(2.0);
    scene.set_viewport_size(100, 100);

    Shape outline;
    outline.rects.push_back(Rect{.ll = {10, 10}, .ur = {20, 20}});
    scene.set_hover(HoverTarget{.origin = TerminalId{1, 0}, .outline = outline});

    const auto &overlay_picture = renderer.build_overlay_picture(scene);

    // dbu rect (10,10)-(20,20) -> pixel (20,20)-(40,40) at this pan/scale
    // (build_overlay_picture's own pre-flip pixel space). Sampled on the
    // left edge, away from any corner.
    const SkColor edge_color = sample_pixel(overlay_picture, 100, 100, 20, 30);
    EXPECT_EQ(SkColorGetR(edge_color), 255);
    EXPECT_EQ(SkColorGetG(edge_color), 255);
    EXPECT_EQ(SkColorGetB(edge_color), 0);
    EXPECT_GT(SkColorGetA(edge_color), 0);

    EXPECT_EQ(SkColorGetA(sample_pixel(overlay_picture, 100, 100, 30, 30)), 0); // interior - stroke only, no fill
}

TEST_F(RenderFixture, BuildOverlayPictureHasNoHoverOutlineWhenNothingIsHovered)
{
    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{0, 0});
    scene.set_scale(2.0);
    scene.set_viewport_size(100, 100);
    scene.set_mouse_position(50, 50); // mouse set, but no hover target

    const auto &overlay_picture = renderer.build_overlay_picture(scene);

    // Would show a yellow outline at this pixel if a stale hover target
    // leaked through - nothing here, since scene.hover() is unset.
    EXPECT_EQ(SkColorGetA(sample_pixel(overlay_picture, 100, 100, 20, 30)), 0);
}

TEST_F(RenderFixture, BuildOverlayPictureDrawsTheLiveDragRectangleWhileDragging)
{
    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{0, 0});
    scene.set_scale(2.0);
    scene.set_viewport_size(200, 200);

    scene.begin_drag(40, 160);        // dbu (20, 20)
    scene.set_mouse_position(160, 40); // dbu (80, 80)

    const auto &overlay_picture = renderer.build_overlay_picture(scene);

    // Normalized drag rect dbu (20,20)-(80,80) -> pixel (40,40)-(160,160)
    // at this pan/scale (build_overlay_picture's own pre-flip pixel
    // space). Left edge stroke (2px, centered on x=40) spans continuous
    // [39,41], so pixel column 39 is fully covered.
    const SkColor edge = sample_pixel(overlay_picture, 200, 200, 39, 100);
    EXPECT_GT(SkColorGetA(edge), 200);          // stroke is close to fully opaque
    EXPECT_GT(SkColorGetB(edge), SkColorGetR(edge)); // blue-dominant

    const SkColor interior = sample_pixel(overlay_picture, 200, 200, 100, 100);
    EXPECT_GT(SkColorGetA(interior), 0);   // translucent fill present
    EXPECT_LT(SkColorGetA(interior), 200); // clearly less opaque than the stroke - it's a fill, not a solid block
}

TEST_F(RenderFixture, BuildOverlayPictureHasNoDragRectangleWhenNotDragging)
{
    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{0, 0});
    scene.set_scale(2.0);
    scene.set_viewport_size(200, 200);
    scene.set_mouse_position(160, 40); // mouse set, but no drag in progress

    const auto &overlay_picture = renderer.build_overlay_picture(scene);
    EXPECT_EQ(SkColorGetA(sample_pixel(overlay_picture, 200, 200, 100, 100)), 0);
}

TEST_F(RenderFixture, BuildOverlayPictureHasNoDragRectangleWhileDraggingWithoutAMousePositionYet)
{
    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{0, 0});
    scene.set_scale(2.0);
    scene.set_viewport_size(200, 200);
    scene.begin_drag(40, 160); // drag started, but set_mouse_position was never called

    const auto &overlay_picture = renderer.build_overlay_picture(scene);
    EXPECT_EQ(SkColorGetA(sample_pixel(overlay_picture, 200, 200, 100, 100)), 0);
}

TEST_F(RenderFixture, BuildSelectionOverlayPictureDoesNotRecomputeWhenOnlyMouseMoves)
{
    // Regression: build_overlay_picture used to bundle selected-piece
    // outlines together with mouse-driven chrome (drag rect/cursor/hover)
    // in one SkPicture keyed partly on mouse_version() - so every mouse
    // move (which bumps mouse_version on every pointer event) forced a
    // full re-walk of the selection list, re-buffering every selected
    // PATH piece's outline (Geometry::path_to_polygons) from scratch even
    // though the selection itself hadn't changed - cost scaling with
    // selection size on every pointer event. Splitting the two pictures
    // means a pure mouse move only recomputes the cheap mouse-only
    // picture now; this is the test that would have failed before that
    // split.
    const TerminalId terminal_id = root.create_terminal(TerminalData{.abstract = abstract_id});
    const Shape path_piece{.layer_name = "M1", .paths = {Path{.polygon = Polygon{.points = {{10, 20}, {30, 20}}}, .width = 4}}};
    root.create_terminal_port(TerminalPortData{.terminal = terminal_id, .shapes = {path_piece}});

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);
    scene.select(terminal_id, path_piece);

    renderer.build_selection_overlay_picture(scene);
    renderer.build_overlay_picture(scene);
    ASSERT_EQ(renderer.selection_overlay_picture_calls(), 1u);
    ASSERT_EQ(renderer.overlay_picture_calls(), 1u);

    // Move the mouse several times - selection is untouched throughout.
    scene.set_mouse_position(5, 5);
    renderer.build_overlay_picture(scene);
    renderer.build_selection_overlay_picture(scene);
    scene.set_mouse_position(6, 6);
    renderer.build_overlay_picture(scene);
    renderer.build_selection_overlay_picture(scene);
    scene.set_mouse_position(7, 7);
    renderer.build_overlay_picture(scene);
    renderer.build_selection_overlay_picture(scene);

    EXPECT_EQ(renderer.overlay_picture_calls(), 4u);           // recomputed on every mouse move, as intended - cheap
    EXPECT_EQ(renderer.selection_overlay_picture_calls(), 1u); // never recomputed - selection never changed
}

TEST_F(RenderFixture, ComposeWithOverlaysDoesNotReRasterizeTheSelectionOverlayWhenOnlyMouseMoves)
{
    // Regression, one level deeper than
    // BuildSelectionOverlayPictureDoesNotRecomputeWhenOnlyMouseMoves
    // above: recording selection_overlay_picture only once isn't enough
    // on its own - compose_with_overlays used to *replay* that SkPicture
    // directly (canvas->drawPicture) on every call regardless, and
    // measured benchmarks (see BENCHMARKS.md) showed SkPicture replay
    // cost, like recording cost, scales with the number of recorded draw
    // ops. So a mouse-only change still cost proportionally to selection
    // size even after the picture-recording fix above. Fixed by
    // rasterizing the selection overlay into its own cached image
    // (rasterize_selection_overlay_frame) and blitting that instead of
    // replaying the picture - this test would have failed before that
    // fix, since rasterize_selection_overlay_calls() would still have
    // been 1 (it's the replay, not the rasterization, that used to
    // repeat) - it exists to guard call-count-only tests like the one
    // above from missing this exact class of regression again.
    add_obstruction_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {10, 10}, .ur = {30, 30}}}});
    const TerminalId terminal_id = root.create_terminal(TerminalData{.abstract = abstract_id});
    const Shape path_piece{.layer_name = "M1", .paths = {Path{.polygon = Polygon{.points = {{40, 40}, {60, 40}}}, .width = 4}}};
    root.create_terminal_port(TerminalPortData{.terminal = terminal_id, .shapes = {path_piece}});

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);
    scene.select(terminal_id, path_piece);

    const auto &shapes = pipeline.run(root, scene, view_layers);
    const auto &pixel_shapes = renderer.transform_to_pixels(shapes, scene);
    const auto &design_picture = renderer.build_picture(pixel_shapes, scene, view_layers, root);

    auto compose_once = [&]
    {
        const auto &overlay_picture = renderer.build_overlay_picture(scene);
        const auto &selection_overlay_picture = renderer.build_selection_overlay_picture(scene);
        renderer.compose_with_overlays(design_picture, sk_sp<SkPicture>{}, overlay_picture, selection_overlay_picture, scene);
    };

    scene.set_mouse_position(1, 1);
    compose_once();
    ASSERT_EQ(renderer.rasterize_selection_overlay_calls(), 1u);

    scene.set_mouse_position(2, 2);
    compose_once();
    scene.set_mouse_position(3, 3);
    compose_once();
    scene.set_mouse_position(4, 4);
    compose_once();

    EXPECT_EQ(renderer.compose_calls(), 4u);                   // cheap composite did recompute every time
    EXPECT_EQ(renderer.rasterize_selection_overlay_calls(), 1u); // selection raster reused, not recomputed
}

TEST_F(RenderFixture, ComposeWithOverlaysDoesNotReRasterizeDesignWhenOnlyMouseMoves)
{
    // This is the whole point of the design/overlay-picture split (see
    // UPDATES.md 5.2's own flagged perf concern, and Renderer::
    // compose_with_overlays's doc comment): a mouse-move must not force a
    // full re-rasterize of a potentially design-sized picture on every
    // pointer event, only the cheap composite step.
    add_obstruction_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {10, 10}, .ur = {30, 30}}}});

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);

    const auto &shapes = pipeline.run(root, scene, view_layers);
    const auto &pixel_shapes = renderer.transform_to_pixels(shapes, scene);
    const auto &design_picture = renderer.build_picture(pixel_shapes, scene, view_layers, root);

    scene.set_mouse_position(10, 10);
    const auto &overlay_picture_1 = renderer.build_overlay_picture(scene);
    const auto &selection_overlay_picture_1 = renderer.build_selection_overlay_picture(scene);
    renderer.compose_with_overlays(design_picture, sk_sp<SkPicture>{}, overlay_picture_1, selection_overlay_picture_1, scene);
    ASSERT_EQ(renderer.rasterize_calls(), 1u);
    ASSERT_EQ(renderer.overlay_picture_calls(), 1u);
    ASSERT_EQ(renderer.selection_overlay_picture_calls(), 1u);
    ASSERT_EQ(renderer.compose_calls(), 1u);

    // Move the mouse only - viewport/visibility versions (and therefore
    // the design content itself) are untouched.
    scene.set_mouse_position(20, 20);
    const auto &overlay_picture_2 = renderer.build_overlay_picture(scene);
    const auto &selection_overlay_picture_2 = renderer.build_selection_overlay_picture(scene);
    renderer.compose_with_overlays(design_picture, sk_sp<SkPicture>{}, overlay_picture_2, selection_overlay_picture_2, scene);

    EXPECT_EQ(renderer.rasterize_calls(), 1u);      // design frame reused, not recomputed
    EXPECT_EQ(renderer.overlay_picture_calls(), 2u); // cheap overlay picture did recompute
    EXPECT_EQ(renderer.selection_overlay_picture_calls(), 1u); // no selection - never recomputes here
    EXPECT_EQ(renderer.compose_calls(), 2u);        // cheap composite did recompute
}

TEST_F(RenderFixture, ComposeWithOverlaysReflectsASelectionChangeEvenWhenComposedOnceBefore)
{
    // Regression: le_render_pixel_buffer (api.cpp) calls exactly this
    // build_picture -> build_overlay_picture -> compose_with_overlays
    // chain - compose_with_overlays's own cache used to be keyed on
    // {AbstractId, viewport_version, visibility_version, mouse_version},
    // missing selection_version, so composing once before selecting
    // (warming the cache) then again after selecting returned the
    // pre-selection composited bytes unchanged, even with a correctly
    // updated design_picture passed in both times. Selects with a piece
    // (every reachable selection through the public API always has one -
    // see Pipeline::hit_test_rect), which is what actually produces a
    // visible outline (via build_selection_overlay_picture) to detect a
    // stale composite with.
    const Shape piece{.layer_name = "M1", .rects = {Rect{.ll = {10, 10}, .ur = {30, 30}}}};
    const ObstructionId obstruction_id = add_obstruction_shape(piece);

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);

    auto compose_and_sample_edge = [&]
    {
        const auto &shapes = pipeline.run(root, scene, view_layers);
        const auto &pixel_shapes = renderer.transform_to_pixels(shapes, scene);
        const auto &design_picture = renderer.build_picture(pixel_shapes, scene, view_layers, root);
        const auto &overlay_picture = renderer.build_overlay_picture(scene);
        const auto &selection_overlay_picture = renderer.build_selection_overlay_picture(scene);
        const PixelBuffer &buffer = renderer.compose_with_overlays(design_picture, sk_sp<SkPicture>{}, overlay_picture, selection_overlay_picture, scene);
        // Same Y-flip as rasterize() - see BuildPictureAndRasterizeAreNotInvalidatedBySelectionChanges's comment.
        const uint8_t *p = buffer.data + static_cast<size_t>(80) * buffer.row_bytes + static_cast<size_t>(9) * 4;
        return std::array<uint8_t, 4>{p[0], p[1], p[2], p[3]};
    };

    const auto before = compose_and_sample_edge(); // warms compose_with_overlays's cache with no selection
    scene.select(obstruction_id, piece);
    const auto after = compose_and_sample_edge();

    EXPECT_NE(before, after);
    EXPECT_EQ(after[0], 255);
    EXPECT_EQ(after[1], 255);
    EXPECT_EQ(after[2], 255);
    EXPECT_GT(after[3], 200);
}

TEST_F(RenderFixture, RasterizeFlipsYSoHigherDbuYEndsUpNearerTheTopOfTheBuffer)
{
    // M1 (red, palette index 0) sits at low dbu y; M2 (green, palette
    // index 1) sits at high dbu y - physically "above" M1 in the design.
    // If rasterize() didn't flip, M2 (the "up" shape) would end up nearer
    // the bottom of the buffer instead.
    add_obstruction_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {10, 10}, .ur = {30, 30}}}});
    add_obstruction_shape(Shape{.layer_name = "M2", .rects = {Rect{.ll = {10, 70}, .ur = {30, 90}}}});

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);

    const auto &shapes = pipeline.run(root, scene, view_layers);
    const auto &pixel_shapes = renderer.transform_to_pixels(shapes, scene);
    const auto &picture = renderer.build_picture(pixel_shapes, scene, view_layers, root);
    const PixelBuffer &buffer = renderer.rasterize(picture, scene);
    ASSERT_NE(buffer.data, nullptr);

    auto channel_at = [&](int x, int y, int channel)
    {
        return buffer.data[static_cast<size_t>(y) * buffer.row_bytes + static_cast<size_t>(x) * 4 + static_cast<size_t>(channel)];
    };

    // OBSTRUCTION's BRICK pattern doesn't cover every pixel - unlike the
    // old flat fill, a single fixed point can land in a transparent gap by
    // coincidence of the pattern's phase. Scan each shape's own 20x20
    // region (inset from its outline hairline) for any pixel carrying its
    // color instead.
    auto region_has_channel = [&](int x0, int y0, int x1, int y1, int channel)
    {
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                if (channel_at(x, y, channel) > 0)
                    return true;
        return false;
    };

    // Near the top of the buffer (rows 11..29): M2's green, not M1's red.
    EXPECT_TRUE(region_has_channel(11, 11, 29, 29, 1));
    EXPECT_FALSE(region_has_channel(11, 11, 29, 29, 0));

    // Near the bottom of the buffer (rows 71..89): M1's red, not M2's green.
    EXPECT_TRUE(region_has_channel(11, 71, 29, 89, 0));
    EXPECT_FALSE(region_has_channel(11, 71, 29, 89, 1));
}

TEST_F(RenderFixture, RasterizeBytesArePremultipliedRgba8888RegardlessOfPlatform)
{
    // A hand-built picture with a known translucent color, rather than
    // going through build_picture()'s own ViewLayerStyle/FillPattern -
    // every default FillPattern other than NONE (used only by BOUNDARY
    // today) draws fully opaque pattern strokes/dots against a transparent
    // background, not a translucent flat wash, so there's no fixed pixel
    // build_picture's own output would reliably put a partial-alpha color
    // at. This test's actual concern - rasterize()'s premultiply byte math -
    // doesn't depend on FillPattern at all, so it's tested in isolation.
    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_viewport_size(100, 100);

    constexpr SkColor kTranslucentColor = SkColorSetARGB(120, 200, 90, 40);
    SkPictureRecorder recorder;
    SkCanvas *record_canvas = recorder.beginRecording(SkRect::MakeWH(100, 100));
    SkPaint paint;
    paint.setColor(kTranslucentColor);
    record_canvas->drawRect(SkRect::MakeLTRB(10, 10, 30, 30), paint);
    sk_sp<SkPicture> picture = recorder.finishRecordingAsPicture();

    const PixelBuffer &buffer = renderer.rasterize(picture, scene);
    ASSERT_NE(buffer.data, nullptr);

    // dbu-space concerns don't apply to this hand-built picture - (25,25)
    // in its own local coordinates ends up at device row 100-25=75 after
    // rasterize()'s Y-flip, well inside the 10..30 rect drawn above, away
    // from any antialiased edge.
    const uint8_t *p = buffer.data + static_cast<size_t>(75) * buffer.row_bytes + static_cast<size_t>(25) * 4;

    // Mirrors SkMulDiv255Round's exact rounding (SkMathPriv.h) rather than
    // an approximation, so this isn't coincidentally right for one alpha
    // value and wrong for another.
    auto premultiply = [](uint8_t c, uint8_t a)
    {
        unsigned prod = static_cast<unsigned>(c) * a + 128;
        return static_cast<uint8_t>((prod + (prod >> 8)) >> 8);
    };

    EXPECT_EQ(p[0], premultiply(SkColorGetR(kTranslucentColor), SkColorGetA(kTranslucentColor))); // R
    EXPECT_EQ(p[1], premultiply(SkColorGetG(kTranslucentColor), SkColorGetA(kTranslucentColor))); // G
    EXPECT_EQ(p[2], premultiply(SkColorGetB(kTranslucentColor), SkColorGetA(kTranslucentColor))); // B
    EXPECT_EQ(p[3], SkColorGetA(kTranslucentColor));                                              // A
}

TEST_F(RenderFixture, RasterizeClearsToTransparentWhereNothingIsDrawn)
{
    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);

    const auto &shapes = pipeline.run(root, scene, view_layers); // empty Abstract, nothing drawn
    const auto &pixel_shapes = renderer.transform_to_pixels(shapes, scene);
    const auto &picture = renderer.build_picture(pixel_shapes, scene, view_layers, root);
    const PixelBuffer &buffer = renderer.rasterize(picture, scene);

    ASSERT_EQ(buffer.width, 100);
    ASSERT_EQ(buffer.height, 100);
    // (50,50) would land exactly on the default grid's major dot lattice
    // (dbu (50,50), a multiple of both the default 5 minor and 50 major
    // spacing) - build_picture always draws the background grid now (see
    // Renderer::draw_grid), so "nothing drawn" means away from any grid
    // dot/axis line, not literally the whole buffer. dbu (52,53) sits
    // safely off every lattice point.
    const uint8_t *p = buffer.data + static_cast<size_t>(47) * buffer.row_bytes + static_cast<size_t>(52) * 4;
    EXPECT_EQ(p[3], 0);
}

TEST_F(RenderFixture, RasterizeWithZeroSizedViewportDoesNotCrashAndReturnsEmptyBuffer)
{
    // Scene's default-constructed viewport size (0x0, e.g. before
    // set_viewport_size() has ever been called) used to crash:
    // SkSurfaces::Raster returns null for non-positive dimensions, and
    // the old code dereferenced that null surface unconditionally.
    Scene scene;
    scene.set_current_abstract(abstract_id);

    const auto &shapes = pipeline.run(root, scene, view_layers);
    const auto &pixel_shapes = renderer.transform_to_pixels(shapes, scene);
    const auto &picture = renderer.build_picture(pixel_shapes, scene, view_layers, root);
    const PixelBuffer &buffer = renderer.rasterize(picture, scene);

    EXPECT_EQ(buffer.data, nullptr);
    EXPECT_EQ(buffer.width, 0);
    EXPECT_EQ(buffer.height, 0);
    EXPECT_EQ(buffer.row_bytes, 0u);
}

TEST_F(RenderFixture, RasterizeReusesCacheUntilViewportVersionChanges)
{
    add_obstruction_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {10, 10}, .ur = {30, 30}}}});

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);

    const auto &shapes = pipeline.run(root, scene, view_layers);
    const auto &pixel_shapes = renderer.transform_to_pixels(shapes, scene);
    const auto &picture = renderer.build_picture(pixel_shapes, scene, view_layers, root);
    renderer.rasterize(picture, scene);
    renderer.rasterize(picture, scene);
    EXPECT_EQ(renderer.rasterize_calls(), 1u);

    scene.set_pan(Point{1, 1});
    renderer.rasterize(picture, scene);
    EXPECT_EQ(renderer.rasterize_calls(), 2u);
}
