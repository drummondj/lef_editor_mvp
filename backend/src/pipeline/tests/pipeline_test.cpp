#include "../pipeline.hpp"
#include <gtest/gtest.h>

using namespace le;

namespace
{
    // Builds a Root with one Technology, an M1 and M2 layer, a matching
    // ViewLayerSet, and one empty Abstract - the common scaffolding every
    // test below attaches to.
    struct PipelineFixture : public ::testing::Test
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

        Root root;
        TechnologyId technology_id;
        LayerId m1;
        LayerId m2;
        ViewLayerSet view_layers;
        AbstractId abstract_id;
        Pipeline pipeline;
    };
}

TEST_F(PipelineFixture, GenerateShapesCollectsPortAndObstructionShapes)
{
    add_terminal_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}});
    add_obstruction_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {20, 20}, .ur = {30, 30}}}});

    const auto &shapes = pipeline.generate_shapes(root, abstract_id, view_layers);
    EXPECT_EQ(shapes.size(), 2u);
}

TEST_F(PipelineFixture, GenerateShapesForUnknownAbstractIsEmpty)
{
    AbstractId unknown{999, 0};
    EXPECT_TRUE(pipeline.generate_shapes(root, unknown, view_layers).empty());
}

TEST_F(PipelineFixture, GenerateShapesResolvesViewLayerByOrigin)
{
    add_terminal_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {0, 0}, .ur = {1, 1}}}});
    add_obstruction_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {0, 0}, .ur = {1, 1}}}});

    const auto &shapes = pipeline.generate_shapes(root, abstract_id, view_layers);
    ASSERT_EQ(shapes.size(), 2u);
    EXPECT_EQ(shapes[0].view_layer, view_layers.find(m1, ViewLayerPurpose::TERMINAL));
    EXPECT_EQ(shapes[1].view_layer, view_layers.find(m1, ViewLayerPurpose::OBSTRUCTION));
    EXPECT_NE(shapes[0].view_layer, shapes[1].view_layer);
}

TEST_F(PipelineFixture, GenerateShapesIncludesAbstractBoundaryResolvedToBoundaryViewLayer)
{
    root.get_abstract(abstract_id)->boundary = {Polygon{.points = {{0, 0}, {0, 100}, {100, 100}, {100, 0}, {0, 0}}}};

    const auto &shapes = pipeline.generate_shapes(root, abstract_id, view_layers);
    ASSERT_EQ(shapes.size(), 1u);
    EXPECT_EQ(shapes.front().shape.layer_name, "BOUNDARY");
    EXPECT_EQ(shapes.front().view_layer, view_layers.boundary_view_layer());
}

TEST_F(PipelineFixture, GenerateShapesLeavesViewLayerInvalidForUnresolvableLayerName)
{
    add_obstruction_shape(Shape{.layer_name = "DOES_NOT_EXIST", .rects = {Rect{.ll = {0, 0}, .ur = {1, 1}}}});

    const auto &shapes = pipeline.generate_shapes(root, abstract_id, view_layers);
    ASSERT_EQ(shapes.size(), 1u);
    EXPECT_FALSE(shapes.front().view_layer.valid());
}

TEST_F(PipelineFixture, GenerateShapesMergesOverlappingRectsWithinATerminalPortShape)
{
    add_terminal_shape(Shape{
        .layer_name = "M1",
        .rects = {
            Rect{.ll = {0, 0}, .ur = {10, 10}},
            Rect{.ll = {5, 5}, .ur = {15, 15}},
        },
    });

    const auto &shapes = pipeline.generate_shapes(root, abstract_id, view_layers);
    ASSERT_EQ(shapes.size(), 1u);
    EXPECT_TRUE(shapes.front().shape.rects.empty());
    ASSERT_EQ(shapes.front().shape.polygons.size(), 1u); // overlapping -> merged into one polygon
}

TEST_F(PipelineFixture, GenerateShapesMergesOverlappingRectsWithinAnObstructionShape)
{
    add_obstruction_shape(Shape{
        .layer_name = "M1",
        .rects = {
            Rect{.ll = {0, 0}, .ur = {10, 10}},
            Rect{.ll = {5, 5}, .ur = {15, 15}},
        },
    });

    const auto &shapes = pipeline.generate_shapes(root, abstract_id, view_layers);
    ASSERT_EQ(shapes.size(), 1u);
    EXPECT_TRUE(shapes.front().shape.rects.empty());
    ASSERT_EQ(shapes.front().shape.polygons.size(), 1u); // overlapping -> merged into one polygon
}

TEST_F(PipelineFixture, GenerateShapesDoesNotMergeRectsAcrossDifferentPortsOrObstructions)
{
    // Two disjoint rects, but each its own separate Shape (own port) -
    // merging is scoped per-Shape, not across a Terminal's whole geometry.
    TerminalId terminal_id = root.create_terminal(TerminalData{.abstract = abstract_id, .name = "D4"});
    root.create_terminal_port(TerminalPortData{.terminal = terminal_id, .shapes = {Shape{.layer_name = "M1", .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}}}});
    root.create_terminal_port(TerminalPortData{.terminal = terminal_id, .shapes = {Shape{.layer_name = "M1", .rects = {Rect{.ll = {5, 5}, .ur = {15, 15}}}}}});

    const auto &shapes = pipeline.generate_shapes(root, abstract_id, view_layers);
    ASSERT_EQ(shapes.size(), 2u);
    EXPECT_EQ(shapes[0].shape.rects.size(), 1u);
    EXPECT_EQ(shapes[1].shape.rects.size(), 1u);
}

TEST_F(PipelineFixture, GenerateShapesAddsTerminalLabelAtComputedLocation)
{
    TerminalId terminal_id = root.create_terminal(TerminalData{.abstract = abstract_id, .name = "A1"});
    root.create_terminal_port(TerminalPortData{.terminal = terminal_id, .shapes = {Shape{.layer_name = "M1", .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}}}});

    const auto &shapes = pipeline.generate_shapes(root, abstract_id, view_layers);
    ASSERT_EQ(shapes.size(), 1u);
    ASSERT_EQ(shapes.front().shape.texts.size(), 1u);
    EXPECT_EQ(shapes.front().shape.texts.front().label, "A1");
    // get_label_location on a single {0,0}-{10,10} rect returns its centroid.
    EXPECT_EQ(shapes.front().shape.texts.front().location.x, 5);
    EXPECT_EQ(shapes.front().shape.texts.front().location.y, 5);
}

TEST_F(PipelineFixture, GenerateShapesLabelsOnlyTheFirstPortsFirstShape)
{
    TerminalId terminal_id = root.create_terminal(TerminalData{.abstract = abstract_id, .name = "B2"});
    root.create_terminal_port(TerminalPortData{.terminal = terminal_id, .shapes = {Shape{.layer_name = "M1", .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}}}});
    root.create_terminal_port(TerminalPortData{.terminal = terminal_id, .shapes = {Shape{.layer_name = "M1", .rects = {Rect{.ll = {20, 0}, .ur = {30, 10}}}}}});

    const auto &shapes = pipeline.generate_shapes(root, abstract_id, view_layers);
    ASSERT_EQ(shapes.size(), 2u);
    ASSERT_EQ(shapes[0].shape.texts.size(), 1u);
    EXPECT_EQ(shapes[0].shape.texts.front().label, "B2");
    EXPECT_TRUE(shapes[1].shape.texts.empty());
}

TEST_F(PipelineFixture, GenerateShapesAddsOneLabelPerDistinctLayer)
{
    TerminalId terminal_id = root.create_terminal(TerminalData{.abstract = abstract_id, .name = "C3"});
    root.create_terminal_port(TerminalPortData{.terminal = terminal_id, .shapes = {Shape{.layer_name = "M1", .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}}}});
    root.create_terminal_port(TerminalPortData{.terminal = terminal_id, .shapes = {Shape{.layer_name = "M2", .rects = {Rect{.ll = {20, 0}, .ur = {30, 10}}}}}});

    const auto &shapes = pipeline.generate_shapes(root, abstract_id, view_layers);
    ASSERT_EQ(shapes.size(), 2u);

    // M1 shape (index 0) gets its own label at its own centroid.
    ASSERT_EQ(shapes[0].shape.texts.size(), 1u);
    EXPECT_EQ(shapes[0].shape.texts.front().label, "C3");
    EXPECT_EQ(shapes[0].shape.texts.front().location.x, 5);
    EXPECT_EQ(shapes[0].shape.texts.front().location.y, 5);

    // M2 shape (index 1) gets its own separate label at its own centroid -
    // not the M1 one, and not left without a label.
    ASSERT_EQ(shapes[1].shape.texts.size(), 1u);
    EXPECT_EQ(shapes[1].shape.texts.front().label, "C3");
    EXPECT_EQ(shapes[1].shape.texts.front().location.x, 25);
    EXPECT_EQ(shapes[1].shape.texts.front().location.y, 5);
}

TEST_F(PipelineFixture, GenerateShapesReusesCacheForSameAbstractId)
{
    add_terminal_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}});

    pipeline.generate_shapes(root, abstract_id, view_layers);
    pipeline.generate_shapes(root, abstract_id, view_layers);
    EXPECT_EQ(pipeline.generate_calls(), 1u);

    AbstractId other = root.create_abstract(AbstractData{});
    pipeline.generate_shapes(root, other, view_layers);
    EXPECT_EQ(pipeline.generate_calls(), 2u);
}

TEST_F(PipelineFixture, FilterByViewportAndSizeKeepsShapesInsideViewport)
{
    Scene scene;
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);

    std::vector<RenderedShape> shapes = {
        RenderedShape{.shape = Shape{.layer_name = "M1", .rects = {Rect{.ll = {10, 10}, .ur = {20, 20}}}}, .view_layer = {}},
    };
    const auto &result = pipeline.filter_by_viewport_and_size(shapes, scene);
    EXPECT_EQ(result.size(), 1u);
}

TEST_F(PipelineFixture, FilterByViewportAndSizeDropsShapesWithNoGeometry)
{
    // Geometry::bbox() doesn't account for Shape::texts, so a text-only
    // shape (no rects/polygons/paths) has no bbox at all.
    Scene scene;
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);

    std::vector<RenderedShape> shapes = {
        RenderedShape{.shape = Shape{.layer_name = "M1", .texts = {Text{.label = "A1", .location = {5, 5}}}}, .view_layer = {}},
    };
    EXPECT_TRUE(pipeline.filter_by_viewport_and_size(shapes, scene).empty());
}

TEST_F(PipelineFixture, FilterByViewportAndSizeDropsShapesOutsideViewport)
{
    Scene scene;
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);

    std::vector<RenderedShape> shapes = {
        RenderedShape{.shape = Shape{.layer_name = "M1", .rects = {Rect{.ll = {1000, 1000}, .ur = {1010, 1010}}}}, .view_layer = {}},
    };
    EXPECT_TRUE(pipeline.filter_by_viewport_and_size(shapes, scene).empty());
}

TEST_F(PipelineFixture, FilterByViewportAndSizeDropsSubPixelDotsButKeepsThinLongShapes)
{
    Scene scene;
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0); // 1 dbu == 1 px, so the sub-pixel threshold is 1 dbu
    scene.set_viewport_size(200, 200);

    RenderedShape dot{.shape = Shape{.layer_name = "M1", .rects = {Rect{.ll = {5, 5}, .ur = {5, 5}}}}, .view_layer = {}};             // 0x0
    RenderedShape thin_long_line{.shape = Shape{.layer_name = "M1", .rects = {Rect{.ll = {5, 5}, .ur = {5, 105}}}}, .view_layer = {}}; // 0 wide, 100 tall

    const auto &result = pipeline.filter_by_viewport_and_size(std::vector<RenderedShape>{dot, thin_long_line}, scene);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result.front().shape.rects.front().ur.y, 105);
}

TEST_F(PipelineFixture, FilterByViewportAndSizeReusesCacheUntilViewportVersionChanges)
{
    Scene scene;
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);
    std::vector<RenderedShape> shapes = {
        RenderedShape{.shape = Shape{.layer_name = "M1", .rects = {Rect{.ll = {10, 10}, .ur = {20, 20}}}}, .view_layer = {}},
    };

    pipeline.filter_by_viewport_and_size(shapes, scene);
    pipeline.filter_by_viewport_and_size(shapes, scene);
    EXPECT_EQ(pipeline.viewport_filter_calls(), 1u);

    scene.set_pan(Point{1, 1});
    pipeline.filter_by_viewport_and_size(shapes, scene);
    EXPECT_EQ(pipeline.viewport_filter_calls(), 2u);
}

TEST_F(PipelineFixture, FilterByLayerVisibilityDropsHiddenViewLayerKeepsVisible)
{
    ViewLayerId m1_obstruction = view_layers.find(m1, ViewLayerPurpose::OBSTRUCTION);
    ViewLayerId m2_obstruction = view_layers.find(m2, ViewLayerPurpose::OBSTRUCTION);

    Scene scene;
    scene.set_layer_name_visible("M2", false);

    std::vector<RenderedShape> shapes = {
        RenderedShape{.shape = Shape{.layer_name = "M1", .rects = {Rect{.ll = {0, 0}, .ur = {1, 1}}}}, .view_layer = m1_obstruction},
        RenderedShape{.shape = Shape{.layer_name = "M2", .rects = {Rect{.ll = {0, 0}, .ur = {1, 1}}}}, .view_layer = m2_obstruction},
    };

    const auto &result = pipeline.filter_by_layer_visibility(shapes, scene, view_layers);
    ASSERT_EQ(result.size(), 1u); // only the M1 group survives - the whole M2 group is dropped
    ASSERT_TRUE(result.contains(m1_obstruction));
    EXPECT_EQ(result.at(m1_obstruction).front().shape.layer_name, "M1");
}

TEST_F(PipelineFixture, FilterByLayerVisibilityKeepsShapesWithInvalidViewLayer)
{
    Scene scene; // no layers explicitly hidden
    std::vector<RenderedShape> shapes = {
        RenderedShape{.shape = Shape{.layer_name = "DOES_NOT_EXIST", .rects = {Rect{.ll = {0, 0}, .ur = {1, 1}}}}, .view_layer = {}},
    };

    const auto &result = pipeline.filter_by_layer_visibility(shapes, scene, view_layers);
    ASSERT_EQ(result.size(), 1u);
    ASSERT_TRUE(result.contains(ViewLayerId{}));
    EXPECT_EQ(result.at(ViewLayerId{}).size(), 1u);
}

TEST_F(PipelineFixture, FilterByLayerVisibilityGroupsInBottomUpLayerOrder)
{
    // M1 was declared before M2 in SetUp(), so its ViewLayers got lower
    // pool indices - map iteration order should put M1 first, M2 second,
    // BOUNDARY last (see the class comment on why that's bottom-up order).
    ViewLayerId m1_terminal = view_layers.find(m1, ViewLayerPurpose::TERMINAL);
    ViewLayerId m2_terminal = view_layers.find(m2, ViewLayerPurpose::TERMINAL);
    ViewLayerId boundary = view_layers.boundary_view_layer();

    Scene scene;
    std::vector<RenderedShape> shapes = {
        RenderedShape{.shape = Shape{.layer_name = "BOUNDARY"}, .view_layer = boundary},
        RenderedShape{.shape = Shape{.layer_name = "M2"}, .view_layer = m2_terminal},
        RenderedShape{.shape = Shape{.layer_name = "M1"}, .view_layer = m1_terminal},
    };

    const auto &result = pipeline.filter_by_layer_visibility(shapes, scene, view_layers);
    ASSERT_EQ(result.size(), 3u);

    std::vector<ViewLayerId> order;
    for (const auto &[view_layer, group] : result)
        order.push_back(view_layer);

    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], m1_terminal);
    EXPECT_EQ(order[1], m2_terminal);
    EXPECT_EQ(order[2], boundary);
}

TEST_F(PipelineFixture, FilterByLayerVisibilityReusesCacheUntilVisibilityVersionChanges)
{
    Scene scene;
    std::vector<RenderedShape> shapes = {
        RenderedShape{.shape = Shape{.layer_name = "M1", .rects = {Rect{.ll = {0, 0}, .ur = {1, 1}}}}, .view_layer = {}},
    };

    pipeline.filter_by_layer_visibility(shapes, scene, view_layers);
    pipeline.filter_by_layer_visibility(shapes, scene, view_layers);
    EXPECT_EQ(pipeline.layer_filter_calls(), 1u);

    scene.set_layer_name_visible("M1", false);
    pipeline.filter_by_layer_visibility(shapes, scene, view_layers);
    EXPECT_EQ(pipeline.layer_filter_calls(), 2u);
}

TEST_F(PipelineFixture, RunChainsAllThreeStagesForCurrentAbstract)
{
    // Kept: on M1 (visible), inside the viewport, well above the sub-pixel threshold.
    add_terminal_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}});
    // Dropped by the viewport filter: on M1, but far outside it.
    add_terminal_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {5000, 5000}, .ur = {5010, 5010}}}});
    // Dropped by the layer filter: M2 obstructions are hidden below.
    add_obstruction_shape(Shape{.layer_name = "M2", .rects = {Rect{.ll = {1, 1}, .ur = {5, 5}}}});

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);
    scene.set_layer_name_visible("M2", false);

    const auto &result = pipeline.run(root, scene, view_layers);
    ASSERT_EQ(result.size(), 1u); // only the M1/TERMINAL group survives
    const auto m1_terminal = view_layers.find(m1, ViewLayerPurpose::TERMINAL);
    ASSERT_TRUE(result.contains(m1_terminal));
    const auto &group = result.at(m1_terminal);
    ASSERT_EQ(group.size(), 1u);
    EXPECT_EQ(group.front().shape.layer_name, "M1");
    EXPECT_EQ(group.front().shape.rects.front().ur.x, 10);
}

TEST_F(PipelineFixture, RunOnUnchangedSceneHitsCacheForEveryStage)
{
    add_terminal_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}});
    add_obstruction_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {20, 20}, .ur = {30, 30}}}});

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);

    pipeline.run(root, scene, view_layers);
    pipeline.run(root, scene, view_layers);

    EXPECT_EQ(pipeline.generate_calls(), 1u);
    EXPECT_EQ(pipeline.viewport_filter_calls(), 1u);
    EXPECT_EQ(pipeline.layer_filter_calls(), 1u);
}

TEST_F(PipelineFixture, RunOnViewportOnlyChangeRecomputesViewportAndLayerFilterButNotGenerate)
{
    add_terminal_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}});

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);

    pipeline.run(root, scene, view_layers);
    scene.set_pan(Point{1, 1});
    pipeline.run(root, scene, view_layers);

    EXPECT_EQ(pipeline.generate_calls(), 1u);
    EXPECT_EQ(pipeline.viewport_filter_calls(), 2u);
    EXPECT_EQ(pipeline.layer_filter_calls(), 2u);
}

TEST_F(PipelineFixture, RunOnVisibilityOnlyChangeRecomputesOnlyTheLayerFilterStage)
{
    add_terminal_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}});

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);

    pipeline.run(root, scene, view_layers);
    scene.set_layer_name_visible("M2", false);
    pipeline.run(root, scene, view_layers);

    EXPECT_EQ(pipeline.generate_calls(), 1u);
    EXPECT_EQ(pipeline.viewport_filter_calls(), 1u);
    EXPECT_EQ(pipeline.layer_filter_calls(), 2u);
}

TEST_F(PipelineFixture, RunOnAbstractChangeRecomputesAllThreeStages)
{
    add_terminal_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}});
    AbstractId other_abstract_id = root.create_abstract(AbstractData{});

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);

    pipeline.run(root, scene, view_layers);
    scene.set_current_abstract(other_abstract_id);
    pipeline.run(root, scene, view_layers);

    EXPECT_EQ(pipeline.generate_calls(), 2u);
    EXPECT_EQ(pipeline.viewport_filter_calls(), 2u);
    EXPECT_EQ(pipeline.layer_filter_calls(), 2u);
}
