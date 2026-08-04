#include "../view_style.hpp"
#include <gtest/gtest.h>

using namespace le;

namespace
{
    struct ViewStyleFixture : public ::testing::Test
    {
        void SetUp() override
        {
            technology_id = root.create_technology(TechnologyData{.database_units_microns = 1000.0});
            m1 = root.create_layer(LayerData{.technology = technology_id, .name = "M1", .type = "ROUTING"});
            m2 = root.create_layer(LayerData{.technology = technology_id, .name = "M2", .type = "ROUTING"});
            view_layers = ViewLayerSet::build_for_technology(root, technology_id);
        }

        Root root;
        TechnologyId technology_id;
        LayerId m1;
        LayerId m2;
        ViewLayerSet view_layers;
    };
}

TEST_F(ViewStyleFixture, CreatesTerminalAndObstructionPerLayerPlusOneBoundary)
{
    // 2 layers x 2 purposes + 1 special BOUNDARY = 5.
    EXPECT_EQ(view_layers.all().size(), 5u);
}

TEST_F(ViewStyleFixture, FindResolvesDistinctViewLayersPerLayerAndPurpose)
{
    ViewLayerId m1_terminal = view_layers.find(m1, ViewLayerPurpose::TERMINAL);
    ViewLayerId m1_obstruction = view_layers.find(m1, ViewLayerPurpose::OBSTRUCTION);
    ViewLayerId m2_terminal = view_layers.find(m2, ViewLayerPurpose::TERMINAL);

    ASSERT_TRUE(m1_terminal.valid());
    ASSERT_TRUE(m1_obstruction.valid());
    ASSERT_TRUE(m2_terminal.valid());

    EXPECT_NE(m1_terminal, m1_obstruction);
    EXPECT_NE(m1_terminal, m2_terminal);
}

TEST_F(ViewStyleFixture, FindReturnsInvalidForUnknownLayer)
{
    LayerId unknown{999, 0};
    EXPECT_FALSE(view_layers.find(unknown, ViewLayerPurpose::TERMINAL).valid());
}

TEST_F(ViewStyleFixture, BoundaryViewLayerHasNoAssociatedLayer)
{
    ViewLayerId boundary_id = view_layers.boundary_view_layer();
    ASSERT_TRUE(boundary_id.valid());

    const ViewLayerData *boundary = view_layers.get(boundary_id);
    ASSERT_NE(boundary, nullptr);
    EXPECT_EQ(boundary->purpose, ViewLayerPurpose::BOUNDARY);
    EXPECT_FALSE(boundary->layer.valid());
}

TEST_F(ViewStyleFixture, GetReturnsCorrectDataForEachViewLayer)
{
    ViewLayerId m1_obstruction_id = view_layers.find(m1, ViewLayerPurpose::OBSTRUCTION);
    const ViewLayerData *data = view_layers.get(m1_obstruction_id);

    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data->name, "M1/OBSTRUCTION");
    EXPECT_EQ(data->purpose, ViewLayerPurpose::OBSTRUCTION);
    EXPECT_EQ(data->layer, m1);
}

TEST_F(ViewStyleFixture, DifferentLayersGetDifferentColors)
{
    const ViewLayerData *m1_data = view_layers.get(view_layers.find(m1, ViewLayerPurpose::TERMINAL));
    const ViewLayerData *m2_data = view_layers.get(view_layers.find(m2, ViewLayerPurpose::TERMINAL));

    ASSERT_NE(m1_data, nullptr);
    ASSERT_NE(m2_data, nullptr);

    const Color &m1_color = m1_data->style.outline_color;
    const Color &m2_color = m2_data->style.outline_color;
    EXPECT_FALSE(m1_color.r == m2_color.r && m1_color.g == m2_color.g && m1_color.b == m2_color.b);
}

TEST_F(ViewStyleFixture, TerminalAndObstructionOfSameLayerShareTheSameColor)
{
    // No fill-pattern differentiation yet (a future update) - for now
    // TERMINAL and OBSTRUCTION of the same physical Layer are visually
    // identical.
    const ViewLayerData *terminal = view_layers.get(view_layers.find(m1, ViewLayerPurpose::TERMINAL));
    const ViewLayerData *obstruction = view_layers.get(view_layers.find(m1, ViewLayerPurpose::OBSTRUCTION));

    ASSERT_NE(terminal, nullptr);
    ASSERT_NE(obstruction, nullptr);

    const Color &t = terminal->style.outline_color;
    const Color &o = obstruction->style.outline_color;
    EXPECT_EQ(t.r, o.r);
    EXPECT_EQ(t.g, o.g);
    EXPECT_EQ(t.b, o.b);
    EXPECT_EQ(t.a, o.a);
    EXPECT_EQ(terminal->style.fill_color.a, obstruction->style.fill_color.a);
}

TEST_F(ViewStyleFixture, BoundaryColorIsUnaffectedByThePerLayerPalette)
{
    const ViewLayerData *boundary = view_layers.get(view_layers.boundary_view_layer());
    ASSERT_NE(boundary, nullptr);

    EXPECT_EQ(boundary->style.outline_color.r, 255);
    EXPECT_EQ(boundary->style.outline_color.g, 255);
    EXPECT_EQ(boundary->style.outline_color.b, 255);
    EXPECT_EQ(boundary->style.fill_color.a, 0);
}

TEST(ViewStylePalette, ColorCyclesWithMoreLayersThanPaletteEntries)
{
    // 31 layers - one more than the 30-color palette - should wrap back to
    // the first color rather than reading out of bounds (the ported-from
    // sibling code this replaces has that exact off-by-one bug - see
    // layer_color's comment).
    Root root;
    TechnologyId technology_id = root.create_technology(TechnologyData{.database_units_microns = 1000.0});
    std::vector<LayerId> layers;
    for (int i = 0; i < 31; ++i)
        layers.push_back(root.create_layer(LayerData{.technology = technology_id, .name = "L" + std::to_string(i), .type = "ROUTING"}));

    ViewLayerSet view_layers = ViewLayerSet::build_for_technology(root, technology_id);

    const ViewLayerData *first = view_layers.get(view_layers.find(layers.front(), ViewLayerPurpose::TERMINAL));
    const ViewLayerData *wrapped = view_layers.get(view_layers.find(layers.back(), ViewLayerPurpose::TERMINAL));

    ASSERT_NE(first, nullptr);
    ASSERT_NE(wrapped, nullptr);
    EXPECT_EQ(first->style.outline_color.r, wrapped->style.outline_color.r);
    EXPECT_EQ(first->style.outline_color.g, wrapped->style.outline_color.g);
    EXPECT_EQ(first->style.outline_color.b, wrapped->style.outline_color.b);
}
