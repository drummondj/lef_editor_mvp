#include "../render.hpp"
#include "include/core/SkBitmap.h"
#include "include/core/SkColor.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkSurface.h"
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
        .texts = {Text{.label = "note", .location = {5, 5}}},
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

    ASSERT_EQ(ps.texts.size(), 1u);
    EXPECT_EQ(ps.texts.front().label, "note");
    EXPECT_DOUBLE_EQ(ps.texts.front().location.x, 10.0);
    EXPECT_DOUBLE_EQ(ps.texts.front().location.y, 10.0);
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
    const auto &picture = renderer.build_picture(pixel_shapes, scene, view_layers);
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
    const auto &picture = renderer.build_picture(pixel_shapes, scene, view_layers);

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

    scene.set_layer_name_visible("M1", false);
    renderer.build_picture(pixel_shapes, scene, view_layers);
    EXPECT_EQ(renderer.picture_calls(), 2u);
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
    const auto &picture = renderer.build_picture(pixel_shapes, scene, view_layers);
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
    const auto &picture = renderer.build_picture(pixel_shapes, scene, view_layers);
    const PixelBuffer &buffer = renderer.rasterize(picture, scene);

    ASSERT_EQ(buffer.width, 100);
    ASSERT_EQ(buffer.height, 100);
    const uint8_t *p = buffer.data + static_cast<size_t>(50) * buffer.row_bytes + static_cast<size_t>(50) * 4;
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
    const auto &picture = renderer.build_picture(pixel_shapes, scene, view_layers);
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
    const auto &picture = renderer.build_picture(pixel_shapes, scene, view_layers);
    renderer.rasterize(picture, scene);
    renderer.rasterize(picture, scene);
    EXPECT_EQ(renderer.rasterize_calls(), 1u);

    scene.set_pan(Point{1, 1});
    renderer.rasterize(picture, scene);
    EXPECT_EQ(renderer.rasterize_calls(), 2u);
}
