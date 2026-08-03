#include "../render.hpp"
#include "include/core/SkBitmap.h"
#include "include/core/SkColor.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkSurface.h"
#include <gtest/gtest.h>

using namespace le;

namespace
{
    // Builds a Root with one Technology, an M1 layer, a matching
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
            sk_sp<SkSurface> surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height));
            surface->getCanvas()->clear(SK_ColorTRANSPARENT);
            surface->getCanvas()->drawPicture(picture);

            SkBitmap bitmap;
            bitmap.allocPixels(SkImageInfo::MakeN32Premul(width, height));
            surface->readPixels(bitmap, 0, 0);
            return bitmap.getColor(x, y);
        }

        Root root;
        TechnologyId technology_id;
        LayerId m1;
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
    ASSERT_EQ(pixel_shapes.front().rects.size(), 1u);
    const auto &r = pixel_shapes.front().rects.front();
    EXPECT_DOUBLE_EQ(r.ll.x, (10 - 5) * 2.0);
    EXPECT_DOUBLE_EQ(r.ll.y, (20 - 5) * 2.0);
    EXPECT_DOUBLE_EQ(r.ur.x, (30 - 5) * 2.0);
    EXPECT_DOUBLE_EQ(r.ur.y, (40 - 5) * 2.0);
}

TEST_F(RenderFixture, TransformToPixelsHandlesPolygonsAndPaths)
{
    add_obstruction_shape(Shape{
        .layer_name = "M1",
        .paths = {Path{.polygon = Polygon{.points = {{0, 0}, {10, 0}}}, .width = 4}},
        .polygons = {Polygon{.points = {{0, 0}, {10, 0}, {10, 10}, {0, 10}}}},
    });

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{0, 0});
    scene.set_scale(2.0);
    scene.set_viewport_size(100, 100);

    const auto &shapes = pipeline.run(root, scene, view_layers);
    const auto &pixel_shapes = renderer.transform_to_pixels(shapes, scene);

    ASSERT_EQ(pixel_shapes.size(), 1u);
    const auto &ps = pixel_shapes.front();
    ASSERT_EQ(ps.polygons.size(), 1u);
    ASSERT_EQ(ps.polygons.front().points.size(), 4u);
    EXPECT_DOUBLE_EQ(ps.polygons.front().points[2].x, 20.0);
    EXPECT_DOUBLE_EQ(ps.polygons.front().points[2].y, 20.0);

    ASSERT_EQ(ps.paths.size(), 1u);
    EXPECT_DOUBLE_EQ(ps.paths.front().width, 8.0); // 4 dbu * scale 2.0
    ASSERT_EQ(ps.paths.front().polygon.points.size(), 2u);
    EXPECT_DOUBLE_EQ(ps.paths.front().polygon.points[1].x, 20.0);
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
    const auto &picture = renderer.build_picture(pixel_shapes, scene, view_layers);

    // Sampled well inside the 10..30 rect (at 20,20), away from any
    // antialiased edge - the exact fully-covered fill color.
    SkColor pixel = sample_pixel(picture, 100, 100, 20, 20);
    const ViewLayerData *view_layer = view_layers.get(pixel_shapes.front().view_layer);
    ASSERT_NE(view_layer, nullptr);
    EXPECT_EQ(SkColorGetR(pixel), view_layer->style.fill_color.r);
    EXPECT_EQ(SkColorGetG(pixel), view_layer->style.fill_color.g);
    EXPECT_EQ(SkColorGetB(pixel), view_layer->style.fill_color.b);
    EXPECT_EQ(SkColorGetA(pixel), view_layer->style.fill_color.a);
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
    const auto &picture = renderer.build_picture(pixel_shapes, scene, view_layers);

    SkColor pixel = sample_pixel(picture, 100, 100, 20, 20);
    EXPECT_EQ(SkColorGetA(pixel), 0); // nothing drawn - no style to draw it with
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
    renderer.build_picture(pixel_shapes, scene, view_layers);
    renderer.build_picture(pixel_shapes, scene, view_layers);
    EXPECT_EQ(renderer.picture_calls(), 1u);

    scene.set_layer_visible(view_layers.find(m1, ViewLayerPurpose::OBSTRUCTION), false);
    renderer.build_picture(pixel_shapes, scene, view_layers);
    EXPECT_EQ(renderer.picture_calls(), 2u);
}
