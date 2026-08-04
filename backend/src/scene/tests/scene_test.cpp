#include "../scene.hpp"
#include <gtest/gtest.h>

using namespace le;

TEST(Scene, DefaultStateIsSensible)
{
    Scene scene;

    EXPECT_FALSE(scene.current_abstract().valid());
    EXPECT_EQ(scene.pan().x, 0);
    EXPECT_EQ(scene.pan().y, 0);
    EXPECT_DOUBLE_EQ(scene.scale(), 1.0);
    EXPECT_EQ(scene.viewport_width_px(), 0);
    EXPECT_EQ(scene.viewport_height_px(), 0);
    EXPECT_TRUE(scene.selection().empty());
    EXPECT_EQ(scene.viewport_version(), 0u);
    EXPECT_EQ(scene.visibility_version(), 0u);
}

TEST(Scene, CurrentAbstractRoundTrips)
{
    Scene scene;
    AbstractId id{5, 1};
    scene.set_current_abstract(id);
    EXPECT_EQ(scene.current_abstract(), id);
}

TEST(Scene, PanAndViewportRoundTrip)
{
    Scene scene;
    scene.set_pan(Point{100, -200});
    scene.set_viewport_size(1920, 1080);

    EXPECT_EQ(scene.pan().x, 100);
    EXPECT_EQ(scene.pan().y, -200);
    EXPECT_EQ(scene.viewport_width_px(), 1920);
    EXPECT_EQ(scene.viewport_height_px(), 1080);
}

TEST(Scene, SetScaleIgnoresNonPositiveValues)
{
    Scene scene;
    scene.set_scale(2.5);
    ASSERT_DOUBLE_EQ(scene.scale(), 2.5);

    scene.set_scale(0.0);
    EXPECT_DOUBLE_EQ(scene.scale(), 2.5); // unchanged

    scene.set_scale(-1.0);
    EXPECT_DOUBLE_EQ(scene.scale(), 2.5); // unchanged
}

TEST(Scene, ViewportVersionBumpsOnPanScaleAndViewportSizeChangesOnly)
{
    Scene scene;
    EXPECT_EQ(scene.viewport_version(), 0u);

    scene.set_pan(Point{1, 1});
    EXPECT_EQ(scene.viewport_version(), 1u);

    scene.set_scale(2.0);
    EXPECT_EQ(scene.viewport_version(), 2u);

    scene.set_viewport_size(100, 100);
    EXPECT_EQ(scene.viewport_version(), 3u);

    // A rejected (non-positive) scale must not bump the version - nothing
    // about the viewport actually changed.
    scene.set_scale(-1.0);
    EXPECT_EQ(scene.viewport_version(), 3u);

    // Unrelated state (visibility) must not bump it either.
    scene.set_layer_visible(ViewLayerId{1, 0}, false);
    EXPECT_EQ(scene.viewport_version(), 3u);
}

TEST(Scene, LayerVisibilityDefaultsToTrueUntilSet)
{
    // Scene only stores/queries by ViewLayerId - it doesn't know or care
    // about ViewLayerSet, so an arbitrary id is enough to test this.
    Scene scene;
    ViewLayerId view_layer{3, 0};

    EXPECT_TRUE(scene.is_layer_visible(view_layer));

    scene.set_layer_visible(view_layer, false);
    EXPECT_FALSE(scene.is_layer_visible(view_layer));

    scene.set_layer_visible(view_layer, true);
    EXPECT_TRUE(scene.is_layer_visible(view_layer));

    // A different, never-toggled ViewLayer is unaffected.
    ViewLayerId other_view_layer{4, 0};
    EXPECT_TRUE(scene.is_layer_visible(other_view_layer));
}

TEST(Scene, VisibilityVersionBumpsOnlyOnSetLayerVisible)
{
    Scene scene;
    EXPECT_EQ(scene.visibility_version(), 0u);

    scene.set_layer_visible(ViewLayerId{3, 0}, false);
    EXPECT_EQ(scene.visibility_version(), 1u);

    scene.set_layer_visible(ViewLayerId{3, 0}, true);
    EXPECT_EQ(scene.visibility_version(), 2u);

    // Unrelated state (viewport) must not bump it.
    scene.set_pan(Point{5, 5});
    EXPECT_EQ(scene.visibility_version(), 2u);
}

TEST(Scene, SelectDeselectAndClear)
{
    Scene scene;
    TerminalId terminal{1, 0};
    ObstructionId obstruction{2, 0};

    scene.select(terminal);
    scene.select(obstruction);
    EXPECT_TRUE(scene.is_selected(terminal));
    EXPECT_TRUE(scene.is_selected(obstruction));
    EXPECT_EQ(scene.selection().size(), 2u);

    // Selecting the same ref again must not duplicate it.
    scene.select(terminal);
    EXPECT_EQ(scene.selection().size(), 2u);

    scene.deselect(terminal);
    EXPECT_FALSE(scene.is_selected(terminal));
    EXPECT_TRUE(scene.is_selected(obstruction));
    EXPECT_EQ(scene.selection().size(), 1u);

    scene.clear_selection();
    EXPECT_TRUE(scene.selection().empty());
}

TEST(Scene, FitToContentCentersAndScalesToFillTheTighterAxis)
{
    Scene scene;
    scene.set_viewport_size(1000, 1000);

    // 100x50 dbu content, 20px padding on each side -> usable 960px, scale
    // bound by the wider (x) axis: 960 / 100 = 9.6.
    Rect bbox{.ll = Point{0, 0}, .ur = Point{100, 50}};
    scene.fit_to_content(bbox, 20);

    EXPECT_DOUBLE_EQ(scene.scale(), 9.6);

    // Content is centered: leftover space on y is (1000/9.6 - 50) ~= 54.17,
    // half of that offsets pan below the bbox's own ll.y.
    const double expected_pan_y = 0 - (1000.0 / 9.6 - 50.0) / 2.0;
    EXPECT_EQ(scene.pan().x, 0 - static_cast<int64_t>((1000.0 / 9.6 - 100.0) / 2.0));
    EXPECT_EQ(scene.pan().y, static_cast<int64_t>(expected_pan_y));
}

TEST(Scene, FitToContentWithNoBboxFallsBackToDefaultScaleAndPan)
{
    Scene scene;
    scene.set_viewport_size(500, 500);
    scene.set_scale(3.0);
    scene.set_pan(Point{10, 10});

    scene.fit_to_content(std::nullopt, 10);

    EXPECT_DOUBLE_EQ(scene.scale(), 1.0);
    EXPECT_EQ(scene.pan().x, 0);
    EXPECT_EQ(scene.pan().y, 0);
}

TEST(Scene, FitToContentWithZeroSizedViewportFallsBackToDefault)
{
    Scene scene;
    Rect bbox{.ll = Point{0, 0}, .ur = Point{100, 100}};
    scene.fit_to_content(bbox, 10);

    EXPECT_DOUBLE_EQ(scene.scale(), 1.0);
    EXPECT_EQ(scene.pan().x, 0);
    EXPECT_EQ(scene.pan().y, 0);
}

TEST(Scene, FitToContentWithDegenerateZeroWidthBboxDoesNotDivideByZero)
{
    Scene scene;
    scene.set_viewport_size(200, 200);

    // Zero-width content (e.g. a single vertical line) - scale must be
    // bounded by the y axis only, not blow up via a 1/0 on x.
    Rect bbox{.ll = Point{5, 0}, .ur = Point{5, 100}};
    scene.fit_to_content(bbox, 0);

    EXPECT_DOUBLE_EQ(scene.scale(), 2.0); // 200 / 100
}

TEST(Scene, SelectionDistinguishesObjectKindsWithTheSameRawId)
{
    // TerminalId{5,0} and ObstructionId{5,0} share the same {index,
    // generation}, but are different types - the variant must not confuse them.
    Scene scene;
    TerminalId terminal{5, 0};
    ObstructionId obstruction{5, 0};

    scene.select(terminal);
    EXPECT_TRUE(scene.is_selected(terminal));
    EXPECT_FALSE(scene.is_selected(obstruction));
}
