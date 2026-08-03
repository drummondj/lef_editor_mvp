#include "../pipeline_cache.hpp"
#include <gtest/gtest.h>

using namespace le;

namespace
{
    // Same scaffolding as pipeline_test.cpp - one Technology, M1/M2 layers,
    // a matching ViewLayerSet, and one Abstract with a terminal and an
    // obstruction so every stage has real work to do.
    struct PipelineCacheFixture : public ::testing::Test
    {
        void SetUp() override
        {
            technology_id = root.create_technology(TechnologyData{.database_units_microns = 1000.0});
            m1 = root.create_layer(LayerData{.technology = technology_id, .name = "M1", .type = "ROUTING"});
            m2 = root.create_layer(LayerData{.technology = technology_id, .name = "M2", .type = "ROUTING"});
            view_layers = ViewLayerSet::build_for_technology(root, technology_id);

            abstract_id = root.create_abstract(AbstractData{});
            TerminalId terminal_id = root.create_terminal(TerminalData{.abstract = abstract_id});
            root.create_terminal_port(TerminalPortData{
                .terminal = terminal_id,
                .shapes = {Shape{.layer_name = "M1", .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}}},
            });
            root.create_obstruction(ObstructionData{
                .abstract = abstract_id,
                .shapes = {Shape{.layer_name = "M1", .rects = {Rect{.ll = {20, 20}, .ur = {30, 30}}}}},
            });

            other_abstract_id = root.create_abstract(AbstractData{});

            scene.set_current_abstract(abstract_id);
            scene.set_pan(Point{0, 0});
            scene.set_scale(1.0);
            scene.set_viewport_size(100, 100);
        }

        Root root;
        TechnologyId technology_id;
        LayerId m1;
        LayerId m2;
        ViewLayerSet view_layers;
        AbstractId abstract_id;
        AbstractId other_abstract_id;
        Scene scene;
    };
}

TEST_F(PipelineCacheFixture, RepeatedRunWithNoChangesHitsCacheForEveryStage)
{
    PipelineCache cache;
    cache.run(root, scene, view_layers);
    cache.run(root, scene, view_layers);

    EXPECT_EQ(cache.generate_calls(), 1u);
    EXPECT_EQ(cache.viewport_filter_calls(), 1u);
    EXPECT_EQ(cache.resolve_calls(), 1u);
    EXPECT_EQ(cache.layer_filter_calls(), 1u);
}

TEST_F(PipelineCacheFixture, ViewportOnlyChangeRecomputesViewportAndLayerFilterButNotGenerateOrResolve)
{
    // resolve_view_layers only depends on AbstractId (via generate_shapes's
    // output) plus root/view_layers - a pan/zoom-only change must not
    // re-trigger it, unlike generate_shapes's original (pre-reorder) cache
    // design where resolve was chained to the viewport filter.
    PipelineCache cache;
    cache.run(root, scene, view_layers);

    scene.set_pan(Point{1, 1});
    cache.run(root, scene, view_layers);

    EXPECT_EQ(cache.generate_calls(), 1u);
    EXPECT_EQ(cache.resolve_calls(), 1u);
    EXPECT_EQ(cache.viewport_filter_calls(), 2u);
    EXPECT_EQ(cache.layer_filter_calls(), 2u);
}

TEST_F(PipelineCacheFixture, VisibilityOnlyChangeRecomputesOnlyTheLayerFilterStage)
{
    PipelineCache cache;
    cache.run(root, scene, view_layers);

    scene.set_layer_visible(view_layers.find(m2, ViewLayerPurpose::OBSTRUCTION), false);
    cache.run(root, scene, view_layers);

    EXPECT_EQ(cache.generate_calls(), 1u);
    EXPECT_EQ(cache.viewport_filter_calls(), 1u);
    EXPECT_EQ(cache.resolve_calls(), 1u);
    EXPECT_EQ(cache.layer_filter_calls(), 2u);
}

TEST_F(PipelineCacheFixture, AbstractChangeRecomputesEveryStage)
{
    PipelineCache cache;
    cache.run(root, scene, view_layers);

    scene.set_current_abstract(other_abstract_id);
    cache.run(root, scene, view_layers);

    EXPECT_EQ(cache.generate_calls(), 2u);
    EXPECT_EQ(cache.viewport_filter_calls(), 2u);
    EXPECT_EQ(cache.resolve_calls(), 2u);
    EXPECT_EQ(cache.layer_filter_calls(), 2u);
}

TEST_F(PipelineCacheFixture, CachedResultMatchesUncachedRun)
{
    PipelineCache cache;
    const auto &cached_result = cache.run(root, scene, view_layers);
    auto uncached_result = Pipeline::run(root, scene, view_layers);

    ASSERT_EQ(cached_result.size(), uncached_result.size());
    for (size_t i = 0; i < cached_result.size(); ++i)
    {
        EXPECT_EQ(cached_result[i].shape.layer_name, uncached_result[i].shape.layer_name);
        EXPECT_EQ(cached_result[i].view_layer, uncached_result[i].view_layer);
    }
}
