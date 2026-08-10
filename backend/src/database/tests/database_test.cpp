#include "../database.hpp"
#include <algorithm>
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

TEST(Database, SetLayerNameKeepsByNameIndexInSync)
{
    Root root;
    TechnologyId technology_id = root.create_technology(TechnologyData{.database_units_microns = 2000.0});
    LayerId layer_id = root.create_layer(LayerData{.technology = technology_id, .name = "M1", .type = "ROUTING"});

    ASSERT_TRUE(root.set_layer_name(layer_id, "M2"));

    EXPECT_EQ(root.get_layer_by_name("M1"), LayerId{});
    EXPECT_EQ(root.get_layer_by_name("M2"), layer_id);
    EXPECT_EQ(root.get_layer(layer_id)->name, "M2");
    // Parent list is untouched by a field set that isn't the parent link.
    EXPECT_EQ(root.get_technology_layers(technology_id), std::vector<LayerId>{layer_id});
}

TEST(Database, SetLayerNameOnNonExistentIdReturnsFalse)
{
    Root root;
    TechnologyId technology_id = root.create_technology(TechnologyData{.database_units_microns = 2000.0});
    LayerId layer_id = root.create_layer(LayerData{.technology = technology_id, .name = "M1", .type = "ROUTING"});
    ASSERT_TRUE(root.delete_layer(layer_id));

    EXPECT_FALSE(root.set_layer_name(layer_id, "M2"));
}

TEST(Database, SetLayerTechnologyMovesItBetweenParentIndexEntries)
{
    Root root;
    TechnologyId tech_a = root.create_technology(TechnologyData{.database_units_microns = 1000.0});
    TechnologyId tech_b = root.create_technology(TechnologyData{.database_units_microns = 2000.0});
    LayerId layer_id = root.create_layer(LayerData{.technology = tech_a, .name = "M1", .type = "ROUTING"});

    ASSERT_TRUE(root.set_layer_technology(layer_id, tech_b));

    EXPECT_TRUE(root.get_technology_layers(tech_a).empty());
    EXPECT_EQ(root.get_technology_layers(tech_b), std::vector<LayerId>{layer_id});
    EXPECT_EQ(root.get_layer(layer_id)->technology, tech_b);
    // Unrelated by-name index untouched by a field set that isn't it.
    EXPECT_EQ(root.get_layer_by_name("M1"), layer_id);
}

TEST(Database, SetLayerFieldToItsCurrentValueIsANoOpThatStillReturnsTrue)
{
    Root root;
    TechnologyId technology_id = root.create_technology(TechnologyData{.database_units_microns = 2000.0});
    LayerId layer_id = root.create_layer(LayerData{.technology = technology_id, .name = "M1", .type = "ROUTING"});

    EXPECT_TRUE(root.set_layer_name(layer_id, "M1"));
    EXPECT_EQ(root.get_layer_by_name("M1"), layer_id);
}

TEST(Database, DeleteLayerRemovesItFromEveryIndex)
{
    Root root;
    TechnologyId technology_id = root.create_technology(TechnologyData{.database_units_microns = 2000.0});
    LayerId layer_id = root.create_layer(LayerData{.technology = technology_id, .name = "M1", .type = "ROUTING"});

    ASSERT_TRUE(root.delete_layer(layer_id));

    EXPECT_EQ(root.get_layer(layer_id), nullptr);
    EXPECT_EQ(root.get_layer_by_name("M1"), LayerId{});
    EXPECT_TRUE(root.get_technology_layers(technology_id).empty());
    // Already deleted - a second delete (and deleting a never-created id) is a no-op, not a crash.
    EXPECT_FALSE(root.delete_layer(layer_id));
    EXPECT_FALSE(root.delete_layer(LayerId{}));
}

// get_field()/match_hop() (cmg's generic filter-expression metadata,
// TCL_EXPLORATION.md Phase 1) are Jinja-templated C++ function templates,
// overloaded once per generated class so a generic caller can invoke
// get_field(data, name)/match_hop(root, id, data, hop, matcher) without
// knowing which concrete class `data` is - they parse cleanly whether or
// not their branches are actually correct, since an uninstantiated
// template body isn't fully type-checked. These tests force real
// instantiation of every branch shape the codegen emits, not just a
// successful build.

TEST(FilterMetadata, GetLayerFieldReturnsNamedScalarLeafOrNullopt)
{
    Root root;
    TechnologyId tech_id = root.create_technology(TechnologyData{.database_units_microns = 2000.0});
    LayerId layer_id = root.create_layer(LayerData{.technology = tech_id, .name = "M1", .type = "ROUTING"});
    const LayerData *data = root.get_layer(layer_id);
    ASSERT_NE(data, nullptr);

    auto name_value = get_field(*data, "name");
    ASSERT_TRUE(name_value.has_value());
    EXPECT_EQ(name_value->type, PropertyValue::Type::STRING);
    EXPECT_EQ(name_value->string_value, "M1");

    // A parent back-reference is structural (a hop target), not a leaf.
    EXPECT_FALSE(get_field(*data, "technology").has_value());
    EXPECT_FALSE(get_field(*data, "does_not_exist").has_value());
}

// match_hop() runtime-dispatches on `hop` with a single string comparison
// per branch, not `if constexpr` - so every branch's body is instantiated
// together for a given Matcher type, not just the one actually taken at
// runtime (unlike a compile-time-selected branch). A class with several
// hop fields (Layer has over a dozen) therefore requires its Matcher to
// compile against *every* one of those target types simultaneously, not
// just the one under test - exactly what the real filter-expression
// evaluator's matcher will look like (a fully generic, name-dispatching
// callable). These helpers use `if constexpr (requires {...})` so a given
// matcher instance compiles against, and safely no-ops on, any type that
// doesn't have the member being checked.
//
// match_hop() also calls a pooled hop target's matcher with (id, data) -
// two args - but a non-pooled one with just (data) - one arg (see
// match_hop()'s own doc comment) - so a matcher usable across every hop a
// class might have needs both call shapes. DualArity below adapts a
// single (data)-only predicate into both.
template <typename Predicate>
struct DualArity
{
    Predicate predicate;

    template <typename T>
    bool operator()(const T &target) const { return predicate(target); }

    template <typename IdT, typename T>
    bool operator()(IdT, const T &target) const { return predicate(target); }
};

template <typename Predicate>
DualArity<Predicate> dual_arity(Predicate predicate) { return DualArity<Predicate>{std::move(predicate)}; }

// The reverse adapter: a predicate that only cares about pooled targets
// (needs the id, e.g. to chain a further match_hop call) - never matches
// a non-pooled (id-less) target.
template <typename Predicate>
struct PooledOnly
{
    Predicate predicate;

    template <typename T>
    bool operator()(const T &) const { return false; }

    template <typename IdT, typename T>
    bool operator()(IdT id, const T &target) const { return predicate(id, target); }
};

template <typename Predicate>
PooledOnly<Predicate> pooled_only(Predicate predicate) { return PooledOnly<Predicate>{std::move(predicate)}; }

auto double_field_equals(double expected)
{
    return dual_arity([expected](const auto &target) -> bool
                       {
        if constexpr (requires { target.database_units_microns; })
        {
            return target.database_units_microns == expected;
        }
        else
        {
            return false;
        } });
}

auto name_equals(std::string_view expected)
{
    return dual_arity([expected](const auto &target) -> bool
                       {
        if constexpr (requires { target.name; })
        {
            return target.name == expected;
        }
        else
        {
            return false;
        } });
}

TEST(FilterMetadata, MatchLayerHopWalksParentScalar)
{
    Root root;
    TechnologyId tech_id = root.create_technology(TechnologyData{.database_units_microns = 2000.0});
    LayerId layer_id = root.create_layer(LayerData{.technology = tech_id, .name = "M1", .type = "ROUTING"});
    const LayerData *data = root.get_layer(layer_id);
    ASSERT_NE(data, nullptr);

    EXPECT_TRUE(match_hop(root, layer_id, *data, "technology", double_field_equals(2000.0)));
    EXPECT_FALSE(match_hop(root, layer_id, *data, "technology", double_field_equals(9999.0)));
    EXPECT_FALSE(match_hop(root, layer_id, *data, "does_not_exist", double_field_equals(2000.0)));
}

TEST(FilterMetadata, MatchTechnologyHopIteratesChildLayersExistentially)
{
    Root root;
    TechnologyId tech_id = root.create_technology(TechnologyData{.database_units_microns = 2000.0});
    root.create_layer(LayerData{.technology = tech_id, .name = "M1", .type = "ROUTING"});
    root.create_layer(LayerData{.technology = tech_id, .name = "M2", .type = "ROUTING"});
    const TechnologyData *data = root.get_technology(tech_id);
    ASSERT_NE(data, nullptr);

    EXPECT_TRUE(match_hop(root, tech_id, *data, "layers", name_equals("M2")));
    EXPECT_FALSE(match_hop(root, tech_id, *data, "layers", name_equals("M3")));
}

TEST(FilterMetadata, GetShapeFieldReturnsLayerNameLeafNotListFields)
{
    Shape shape;
    shape.layer_name = "M4";

    auto value = get_field(shape, "layer_name");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(value->string_value, "M4");
    EXPECT_FALSE(get_field(shape, "rects").has_value());
}

auto rect_ur_y_equals(int64_t expected)
{
    return dual_arity([expected](const auto &target) -> bool
                       {
        if constexpr (requires { target.ur.y; })
        {
            return target.ur.y == expected;
        }
        else
        {
            return false;
        } });
}

auto layer_name_equals(std::string_view expected)
{
    return dual_arity([expected](const auto &target) -> bool
                       {
        if constexpr (requires { target.layer_name; })
        {
            return target.layer_name == expected;
        }
        else
        {
            return false;
        } });
}

auto design_id_equals(DesignId expected)
{
    return dual_arity([expected](const auto &target) -> bool
                       {
        if constexpr (requires { target.design; })
        {
            return target.design == expected;
        }
        else
        {
            return false;
        } });
}

TEST(FilterMetadata, MatchShapeHopIteratesEmbeddedRects)
{
    // Shape has several other hop fields too (paths, polygons, texts, ...) -
    // rect_ur_y_equals must (and does) compile against all of them. Shape
    // is pooled (TCL_EXPLORATION.md Phase 3), so this is the (root, id,
    // data, ...) form - see MatchRectHopWalksScalarPointsWithoutRootOrId
    // below for a genuinely non-pooled example of the other form.
    Root root;
    Shape shape;
    shape.layer_name = "M4";
    shape.rects.push_back(Rect{.ll = Point{.x = 0, .y = 0}, .ur = Point{.x = 100, .y = 200}});
    ShapeId shape_id = root.create_shape(shape);
    const ShapeData *data = root.get_shape(shape_id);
    ASSERT_NE(data, nullptr);

    EXPECT_TRUE(match_hop(root, shape_id, *data, "rects", rect_ur_y_equals(200)));
    EXPECT_FALSE(match_hop(root, shape_id, *data, "rects", rect_ur_y_equals(999)));
    EXPECT_FALSE(match_hop(root, shape_id, *data, "polygons", rect_ur_y_equals(200)));
}

TEST(FilterMetadata, MatchRectHopWalksScalarPointsWithoutRootOrId)
{
    // Rect (has_pool=False) is the "no root/id needed" match_hop() form -
    // Shape itself no longer demonstrates this now that it's pooled (see
    // MatchShapeHopIteratesEmbeddedRects above).
    Rect rect{.ll = Point{.x = 0, .y = 0}, .ur = Point{.x = 100, .y = 200}};

    EXPECT_TRUE(match_hop(rect, "ur", [](const Point &point) -> bool
                           { return point.y == 200; }));
    EXPECT_FALSE(match_hop(rect, "does_not_exist", [](const Point &) -> bool
                            { return true; }));
}

TEST(FilterMetadata, MatchObstructionHopCoversParentWalkAndEmbeddedListExistentially)
{
    Root root;
    LibraryId library_id = root.create_library(LibraryData{.name = "LIB"});
    DesignId design_id = root.create_design(DesignData{.library = library_id, .name = "CELL"});
    AbstractId abstract_id = root.create_abstract(AbstractData{.design = design_id});

    ObstructionId obstruction_id = root.create_obstruction(ObstructionData{.abstract = abstract_id});
    root.create_shape(ShapeData{.obstruction = obstruction_id, .layer_name = "M4"});
    const ObstructionData *data = root.get_obstruction(obstruction_id);
    ASSERT_NE(data, nullptr);

    // Parent-scalar hop (Obstruction.abstract -> Abstract).
    EXPECT_TRUE(match_hop(root, obstruction_id, *data, "abstract", design_id_equals(design_id)));

    // Child-list hop (Obstruction.shapes -> Shape, now pooled - TCL_EXPLORATION.md Phase 3).
    EXPECT_TRUE(match_hop(root, obstruction_id, *data, "shapes", layer_name_equals("M4")));
    EXPECT_FALSE(match_hop(root, obstruction_id, *data, "shapes", layer_name_equals("M9")));
    EXPECT_FALSE(match_hop(root, obstruction_id, *data, "does_not_exist", layer_name_equals("M4")));
}

TEST(FilterMetadata, MatchHopChainsTwoLevelsDeepUsingTheIdPassedToTheFirstHopsMatcher)
{
    // This is the actual point of match_hop() passing (id, data) rather
    // than just data for pooled targets: a matcher resolving the first
    // hop (Obstruction.abstract -> Abstract) can use the AbstractId it's
    // handed to resolve a *second* hop (Abstract.design -> Design) itself
    // - impossible if match_hop only ever exposed data, since nothing
    // downstream would have an id to call match_hop with again. Mirrors
    // what a filter path like `.abstract.design.name` needs to do.
    Root root;
    LibraryId library_id = root.create_library(LibraryData{.name = "LIB"});
    DesignId design_id = root.create_design(DesignData{.library = library_id, .name = "CELL"});
    AbstractId abstract_id = root.create_abstract(AbstractData{.design = design_id});
    ObstructionId obstruction_id = root.create_obstruction(ObstructionData{.abstract = abstract_id});
    const ObstructionData *data = root.get_obstruction(obstruction_id);
    ASSERT_NE(data, nullptr);

    // Generic (not concretely AbstractId/AbstractData) because Obstruction
    // now has two pooled hop fields ("abstract" and, since Shape is
    // pooled too, "shapes") - both get matcher(id, data) calls, so this
    // predicate must compile against ShapeId/ShapeData as well. match_hop
    // itself stays valid to call either way (Shape's own match_hop simply
    // has no "design" branch, so it returns false at runtime).
    auto second_hop_matches_cell = pooled_only([&](auto hop1_id, const auto &hop1_data) -> bool
                                                {
        return match_hop(root, hop1_id, hop1_data, "design", dual_arity([](const auto &hop2_data) -> bool
                                                                          {
            if constexpr (requires { hop2_data.name; })
            {
                return hop2_data.name == "CELL";
            }
            else
            {
                return false;
            } })); });
    EXPECT_TRUE(match_hop(root, obstruction_id, *data, "abstract", second_hop_matches_cell));

    auto second_hop_matches_wrong_name = pooled_only([&](auto hop1_id, const auto &hop1_data) -> bool
                                                       {
        return match_hop(root, hop1_id, hop1_data, "design", dual_arity([](const auto &hop2_data) -> bool
                                                                          {
            if constexpr (requires { hop2_data.name; })
            {
                return hop2_data.name == "NOT_CELL";
            }
            else
            {
                return false;
            } })); });
    EXPECT_FALSE(match_hop(root, obstruction_id, *data, "abstract", second_hop_matches_wrong_name));
}

TEST(FilterMetadata, MatchDesignHopWalksSingleNonListChild)
{
    // Design.abstract/Design.schematic are is_child but not is_list - the
    // one hop shape (single, not existential-over-a-list child) none of
    // the other FilterMetadata tests above exercise.
    Root root;
    LibraryId library_id = root.create_library(LibraryData{.name = "LIB"});
    DesignId design_id = root.create_design(DesignData{.library = library_id, .name = "CELL"});
    root.create_abstract(AbstractData{.design = design_id});
    const DesignData *data = root.get_design(design_id);
    ASSERT_NE(data, nullptr);

    EXPECT_TRUE(match_hop(root, design_id, *data, "abstract", design_id_equals(design_id)));
    EXPECT_FALSE(match_hop(root, design_id, *data, "does_not_exist", design_id_equals(design_id)));
}

TEST(FilterMetadata, SearchLayerCollectsIdsMatchingAGenericPredicate)
{
    Root root;
    TechnologyId tech_id = root.create_technology(TechnologyData{.database_units_microns = 2000.0});
    LayerId m1 = root.create_layer(LayerData{.technology = tech_id, .name = "M1", .type = "ROUTING"});
    LayerId m2 = root.create_layer(LayerData{.technology = tech_id, .name = "M2", .type = "ROUTING"});
    root.create_layer(LayerData{.technology = tech_id, .name = "V1", .type = "CUT"});

    // A stand-in for what the hand-written filter-expression evaluator
    // (Phase 2) will eventually plug in: a predicate over (root, id, data).
    auto is_routing = [](const Root &, LayerId, const LayerData &d)
    { return d.type == "ROUTING"; };

    std::vector<LayerId> routing_layers = root.search_layer(is_routing);
    EXPECT_EQ(routing_layers.size(), 2u);
    EXPECT_NE(std::find(routing_layers.begin(), routing_layers.end(), m1), routing_layers.end());
    EXPECT_NE(std::find(routing_layers.begin(), routing_layers.end(), m2), routing_layers.end());

    auto matches_nothing = [](const Root &, LayerId, const LayerData &)
    { return false; };
    EXPECT_TRUE(root.search_layer(matches_nothing).empty());
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
