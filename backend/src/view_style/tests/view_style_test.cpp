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
