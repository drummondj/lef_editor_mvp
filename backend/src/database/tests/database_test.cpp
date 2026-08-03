#include "../database.hpp"
#include <gtest/gtest.h>

using namespace le;

TEST(Database, CreateAndGetTechnology)
{
    Root root;
    TechnologyId id = root.create_technology(TechnologyData{.database_units_microns = 2000.0});

    const TechnologyData *data = root.get_technology(id);
    ASSERT_NE(data, nullptr);
    EXPECT_DOUBLE_EQ(data->database_units_microns, 2000.0);
}

TEST(Database, LayerIndexedByParentAndName)
{
    Root root;
    TechnologyId technology_id = root.create_technology(TechnologyData{.database_units_microns = 2000.0});
    LayerId layer_id = root.create_layer(LayerData{
        .technology = technology_id,
        .name = "M1",
        .type = "ROUTING",
    });

    EXPECT_EQ(root.get_technology_layers(technology_id), std::vector<LayerId>{layer_id});
    EXPECT_EQ(root.get_layer_by_name("M1"), layer_id);
    EXPECT_EQ(root.get_layer_by_name("does-not-exist"), LayerId{});
}

TEST(Pool, ErasedIdIsNotReused)
{
    Pool<TechnologyData, TechnologyId> pool;
    TechnologyId first = pool.create(TechnologyData{.database_units_microns = 1.0});
    pool.erase(first);
    TechnologyId second = pool.create(TechnologyData{.database_units_microns = 2.0});

    // Same slot index is reused, but the generation is bumped, so the old id
    // must not resolve into the new slot's data.
    EXPECT_EQ(first.index, second.index);
    EXPECT_NE(first.generation, second.generation);
    EXPECT_EQ(pool.get(first), nullptr);
    ASSERT_NE(pool.get(second), nullptr);
    EXPECT_DOUBLE_EQ(pool.get(second)->database_units_microns, 2.0);
}
