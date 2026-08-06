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
    EXPECT_EQ(scene.minor_grid_spacing(), 5);
    EXPECT_EQ(scene.major_grid_spacing(), 50);
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
    scene.set_layer_name_visible("M1", false);
    EXPECT_EQ(scene.viewport_version(), 3u);
}

TEST(Scene, GridSpacingRoundTrips)
{
    Scene scene;
    scene.set_minor_grid_spacing(10);
    scene.set_major_grid_spacing(100);

    EXPECT_EQ(scene.minor_grid_spacing(), 10);
    EXPECT_EQ(scene.major_grid_spacing(), 100);
}

TEST(Scene, GridSpacingIgnoresNonPositiveValues)
{
    Scene scene;
    scene.set_minor_grid_spacing(10);
    scene.set_major_grid_spacing(100);

    scene.set_minor_grid_spacing(0);
    scene.set_minor_grid_spacing(-5);
    EXPECT_EQ(scene.minor_grid_spacing(), 10); // unchanged

    scene.set_major_grid_spacing(0);
    scene.set_major_grid_spacing(-5);
    EXPECT_EQ(scene.major_grid_spacing(), 100); // unchanged
}

TEST(Scene, GridSpacingSettersBumpVisibilityVersion)
{
    // The grid is part of the rendered picture (see Renderer::draw_grid),
    // so changing its spacing must invalidate the same render cache
    // layer visibility does - unlike selectability, which doesn't.
    Scene scene;
    EXPECT_EQ(scene.visibility_version(), 0u);

    scene.set_minor_grid_spacing(10);
    EXPECT_EQ(scene.visibility_version(), 1u);

    scene.set_major_grid_spacing(100);
    EXPECT_EQ(scene.visibility_version(), 2u);

    // A rejected (non-positive) value must not bump it.
    scene.set_minor_grid_spacing(-5);
    EXPECT_EQ(scene.visibility_version(), 2u);
}

TEST(Scene, MousePositionDefaultsToUnset)
{
    Scene scene;
    EXPECT_FALSE(scene.has_mouse_position());
    EXPECT_EQ(scene.mouse_version(), 0u);
    EXPECT_FALSE(scene.mouse_dbu_position().has_value());
    EXPECT_FALSE(scene.snapped_mouse_position().has_value());
}

TEST(Scene, SetMousePositionBumpsMouseVersionNotViewportOrVisibilityVersion)
{
    // A mouse move must invalidate only the cheap cursor-overlay cache
    // (see Renderer::build_cursor_picture/compose_with_cursor), not the
    // expensive design rasterize cache keyed on viewport/visibility
    // version - see scene.hpp's own comment on mouse_version_.
    Scene scene;
    scene.set_pan(Point{0, 0});
    scene.set_viewport_size(100, 100);
    const uint64_t viewport_version_before = scene.viewport_version();
    const uint64_t visibility_version_before = scene.visibility_version();

    scene.set_mouse_position(10, 20);
    EXPECT_TRUE(scene.has_mouse_position());
    EXPECT_EQ(scene.mouse_version(), 1u);
    EXPECT_EQ(scene.viewport_version(), viewport_version_before);
    EXPECT_EQ(scene.visibility_version(), visibility_version_before);

    scene.set_mouse_position(11, 20);
    EXPECT_EQ(scene.mouse_version(), 2u);
}

TEST(Scene, MouseDbuPositionUndoesPanScaleAndYFlip)
{
    // Pixel space is top-left origin/y-down (matches le_render_pixel_buffer's
    // output image and le_zoom's x/y); dbu space is y-up - see scene.hpp's
    // mouse_dbu_position comment for the exact inverse formula.
    Scene scene;
    scene.set_pan(Point{100, 200});
    scene.set_scale(2.0);
    scene.set_viewport_size(50, 40);

    scene.set_mouse_position(10, 10);
    const std::optional<Point> dbu = scene.mouse_dbu_position();
    ASSERT_TRUE(dbu.has_value());
    EXPECT_EQ(dbu->x, 100 + 10 / 2); // pan.x + x_px / scale
    EXPECT_EQ(dbu->y, 200 + (40 - 10) / 2); // pan.y + (height - y_px) / scale
}

TEST(Scene, SnappedMousePositionRoundsToNearestMinorGridMultiple)
{
    Scene scene;
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);
    scene.set_minor_grid_spacing(10);

    // dbu (23, 100 - 27) = (23, 73); nearest multiples of 10 are 20 and 70.
    scene.set_mouse_position(23, 27);
    const std::optional<Point> snapped = scene.snapped_mouse_position();
    ASSERT_TRUE(snapped.has_value());
    EXPECT_EQ(snapped->x, 20);
    EXPECT_EQ(snapped->y, 70);
}

TEST(Scene, ClearMousePositionResetsHasMousePositionAndBumpsVersionOnlyIfItWasSet)
{
    Scene scene;
    scene.set_mouse_position(5, 5);
    ASSERT_EQ(scene.mouse_version(), 1u);

    scene.clear_mouse_position();
    EXPECT_FALSE(scene.has_mouse_position());
    EXPECT_FALSE(scene.mouse_dbu_position().has_value());
    EXPECT_EQ(scene.mouse_version(), 2u);

    // Clearing an already-clear position is a no-op - must not bump the
    // version again (would otherwise invalidate caches for nothing).
    scene.clear_mouse_position();
    EXPECT_EQ(scene.mouse_version(), 2u);
}

TEST(Scene, LayerNameVisibilityDefaultsToTrueUntilSet)
{
    Scene scene;

    EXPECT_TRUE(scene.is_layer_name_visible("M1"));

    scene.set_layer_name_visible("M1", false);
    EXPECT_FALSE(scene.is_layer_name_visible("M1"));

    scene.set_layer_name_visible("M1", true);
    EXPECT_TRUE(scene.is_layer_name_visible("M1"));

    // A different, never-toggled layer name is unaffected.
    EXPECT_TRUE(scene.is_layer_name_visible("M2"));
}

TEST(Scene, PurposeVisibilityDefaultsToTrueUntilSet)
{
    Scene scene;

    EXPECT_TRUE(scene.is_purpose_visible(ViewLayerPurpose::OBSTRUCTION));

    scene.set_purpose_visible(ViewLayerPurpose::OBSTRUCTION, false);
    EXPECT_FALSE(scene.is_purpose_visible(ViewLayerPurpose::OBSTRUCTION));

    scene.set_purpose_visible(ViewLayerPurpose::OBSTRUCTION, true);
    EXPECT_TRUE(scene.is_purpose_visible(ViewLayerPurpose::OBSTRUCTION));

    // A different, never-toggled purpose is unaffected.
    EXPECT_TRUE(scene.is_purpose_visible(ViewLayerPurpose::TERMINAL));
}

TEST(Scene, IsViewLayerVisibleIsTheAndOfBothAxes)
{
    Scene scene;

    // Both axes default true -> visible.
    EXPECT_TRUE(scene.is_view_layer_visible("M1", ViewLayerPurpose::TERMINAL));

    // Layer name off, purpose still on -> not visible.
    scene.set_layer_name_visible("M1", false);
    EXPECT_FALSE(scene.is_view_layer_visible("M1", ViewLayerPurpose::TERMINAL));
    // A different layer name is unaffected by M1's toggle.
    EXPECT_TRUE(scene.is_view_layer_visible("M2", ViewLayerPurpose::TERMINAL));

    // Layer name back on, but now the purpose is off -> still not visible.
    scene.set_layer_name_visible("M1", true);
    scene.set_purpose_visible(ViewLayerPurpose::TERMINAL, false);
    EXPECT_FALSE(scene.is_view_layer_visible("M1", ViewLayerPurpose::TERMINAL));
    // OBSTRUCTION on the same layer is unaffected by the TERMINAL toggle.
    EXPECT_TRUE(scene.is_view_layer_visible("M1", ViewLayerPurpose::OBSTRUCTION));

    // Both axes on again -> visible.
    scene.set_purpose_visible(ViewLayerPurpose::TERMINAL, true);
    EXPECT_TRUE(scene.is_view_layer_visible("M1", ViewLayerPurpose::TERMINAL));
}

TEST(Scene, VisibilityVersionBumpsOnSetLayerNameVisibleAndSetPurposeVisible)
{
    Scene scene;
    EXPECT_EQ(scene.visibility_version(), 0u);

    scene.set_layer_name_visible("M1", false);
    EXPECT_EQ(scene.visibility_version(), 1u);

    scene.set_purpose_visible(ViewLayerPurpose::OBSTRUCTION, false);
    EXPECT_EQ(scene.visibility_version(), 2u);

    // Unrelated state (viewport) must not bump it.
    scene.set_pan(Point{5, 5});
    EXPECT_EQ(scene.visibility_version(), 2u);
}

TEST(Scene, LayerNameSelectabilityDefaultsToTrueUntilSet)
{
    Scene scene;

    EXPECT_TRUE(scene.is_layer_name_selectable("M1"));

    scene.set_layer_name_selectable("M1", false);
    EXPECT_FALSE(scene.is_layer_name_selectable("M1"));

    scene.set_layer_name_selectable("M1", true);
    EXPECT_TRUE(scene.is_layer_name_selectable("M1"));

    EXPECT_TRUE(scene.is_layer_name_selectable("M2"));
}

TEST(Scene, PurposeSelectabilityDefaultsToTrueUntilSet)
{
    Scene scene;

    EXPECT_TRUE(scene.is_purpose_selectable(ViewLayerPurpose::OBSTRUCTION));

    scene.set_purpose_selectable(ViewLayerPurpose::OBSTRUCTION, false);
    EXPECT_FALSE(scene.is_purpose_selectable(ViewLayerPurpose::OBSTRUCTION));

    scene.set_purpose_selectable(ViewLayerPurpose::OBSTRUCTION, true);
    EXPECT_TRUE(scene.is_purpose_selectable(ViewLayerPurpose::OBSTRUCTION));

    EXPECT_TRUE(scene.is_purpose_selectable(ViewLayerPurpose::TERMINAL));
}

TEST(Scene, IsViewLayerSelectableIsTheAndOfBothAxes)
{
    Scene scene;
    EXPECT_TRUE(scene.is_view_layer_selectable("M1", ViewLayerPurpose::OBSTRUCTION));

    scene.set_layer_name_selectable("M1", false);
    EXPECT_FALSE(scene.is_view_layer_selectable("M1", ViewLayerPurpose::OBSTRUCTION));

    scene.set_layer_name_selectable("M1", true);
    scene.set_purpose_selectable(ViewLayerPurpose::OBSTRUCTION, false);
    EXPECT_FALSE(scene.is_view_layer_selectable("M1", ViewLayerPurpose::OBSTRUCTION));
}

TEST(Scene, SetLayerNameSelectableAndSetPurposeSelectableDoNotBumpVisibilityVersion)
{
    // Selectability isn't consumed by Pipeline/Renderer caching (unlike
    // visibility) - it must not bump visibility_version(), or every
    // selectability toggle would force an unnecessary re-render.
    Scene scene;
    EXPECT_EQ(scene.visibility_version(), 0u);

    scene.set_layer_name_selectable("M1", false);
    EXPECT_EQ(scene.visibility_version(), 0u);

    scene.set_purpose_selectable(ViewLayerPurpose::OBSTRUCTION, false);
    EXPECT_EQ(scene.visibility_version(), 0u);
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
