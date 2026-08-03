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

TEST(Scene, LayerVisibilityDefaultsToTrueUntilSet)
{
    Scene scene;
    LayerId layer{3, 0};

    EXPECT_TRUE(scene.is_layer_visible(layer));

    scene.set_layer_visible(layer, false);
    EXPECT_FALSE(scene.is_layer_visible(layer));

    scene.set_layer_visible(layer, true);
    EXPECT_TRUE(scene.is_layer_visible(layer));

    // A different, never-toggled layer is unaffected.
    LayerId other_layer{4, 0};
    EXPECT_TRUE(scene.is_layer_visible(other_layer));
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
