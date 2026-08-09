#include "../lef_reader.hpp"
#include "../lef_writer.hpp"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <gtest/gtest.h>

using namespace le;

// Deliberately does NOT use the generated to_string()/to_properties() free
// functions for comparison here - they call std::optional::value()
// unconditionally on every is_optional field (confirmed by reading
// generated/layer.hpp), so they'd throw on any layer that leaves one of
// Phase 1's new optional LAYER properties unset (e.g. V1 below, which has
// no PITCH/OFFSET/AREA/RESISTANCE/...). Direct field comparisons avoid
// that and give a clearer failure message anyway.
namespace
{
    std::string fixture_path(const std::string &name)
    {
        return std::string(IO_TEST_FIXTURES_DIR) + "/" + name;
    }

    std::string scratch_output_path()
    {
        return (std::filesystem::temp_directory_path() / "le_lef_writer_roundtrip.lef").string();
    }

    void expect_shapes_equal(const Shape &original, const Shape &written)
    {
        EXPECT_EQ(original.layer_name, written.layer_name);

        ASSERT_EQ(original.rects.size(), written.rects.size());
        for (size_t i = 0; i < original.rects.size(); i++)
        {
            EXPECT_EQ(original.rects[i].ll.x, written.rects[i].ll.x);
            EXPECT_EQ(original.rects[i].ll.y, written.rects[i].ll.y);
            EXPECT_EQ(original.rects[i].ur.x, written.rects[i].ur.x);
            EXPECT_EQ(original.rects[i].ur.y, written.rects[i].ur.y);
        }
        EXPECT_EQ(original.rect_masks, written.rect_masks);

        ASSERT_EQ(original.polygons.size(), written.polygons.size());
        for (size_t i = 0; i < original.polygons.size(); i++)
        {
            ASSERT_EQ(original.polygons[i].points.size(), written.polygons[i].points.size());
            for (size_t j = 0; j < original.polygons[i].points.size(); j++)
            {
                EXPECT_EQ(original.polygons[i].points[j].x, written.polygons[i].points[j].x);
                EXPECT_EQ(original.polygons[i].points[j].y, written.polygons[i].points[j].y);
            }
        }
        EXPECT_EQ(original.polygon_masks, written.polygon_masks);

        ASSERT_EQ(original.paths.size(), written.paths.size());
        for (size_t i = 0; i < original.paths.size(); i++)
        {
            EXPECT_EQ(original.paths[i].width, written.paths[i].width);
            ASSERT_EQ(original.paths[i].polygon.points.size(), written.paths[i].polygon.points.size());
            for (size_t j = 0; j < original.paths[i].polygon.points.size(); j++)
            {
                EXPECT_EQ(original.paths[i].polygon.points[j].x, written.paths[i].polygon.points[j].x);
                EXPECT_EQ(original.paths[i].polygon.points[j].y, written.paths[i].polygon.points[j].y);
            }
        }
        EXPECT_EQ(original.path_masks, written.path_masks);

        ASSERT_EQ(original.rect_iterates.size(), written.rect_iterates.size());
        for (size_t i = 0; i < original.rect_iterates.size(); i++)
        {
            const RectIterate &o = original.rect_iterates[i];
            const RectIterate &w = written.rect_iterates[i];
            EXPECT_EQ(o.rect.ll.x, w.rect.ll.x);
            EXPECT_EQ(o.rect.ll.y, w.rect.ll.y);
            EXPECT_EQ(o.rect.ur.x, w.rect.ur.x);
            EXPECT_EQ(o.rect.ur.y, w.rect.ur.y);
            EXPECT_EQ(o.num_x, w.num_x);
            EXPECT_EQ(o.num_y, w.num_y);
            EXPECT_EQ(o.space_x, w.space_x);
            EXPECT_EQ(o.space_y, w.space_y);
        }

        ASSERT_EQ(original.path_iterates.size(), written.path_iterates.size());
        for (size_t i = 0; i < original.path_iterates.size(); i++)
        {
            const PathIterate &o = original.path_iterates[i];
            const PathIterate &w = written.path_iterates[i];
            EXPECT_EQ(o.path.width, w.path.width);
            ASSERT_EQ(o.path.polygon.points.size(), w.path.polygon.points.size());
            for (size_t j = 0; j < o.path.polygon.points.size(); j++)
            {
                EXPECT_EQ(o.path.polygon.points[j].x, w.path.polygon.points[j].x);
                EXPECT_EQ(o.path.polygon.points[j].y, w.path.polygon.points[j].y);
            }
            EXPECT_EQ(o.num_x, w.num_x);
            EXPECT_EQ(o.num_y, w.num_y);
            EXPECT_EQ(o.space_x, w.space_x);
            EXPECT_EQ(o.space_y, w.space_y);
        }
    }
}

class LEFWriterRoundtripFixture : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_EQ(reader.read_lef(fixture_path("writer_roundtrip.lef"), original_root, "test_lib"), 0);

        const std::string out_path = scratch_output_path();
        ASSERT_EQ(writer.write_lef(out_path, original_root, original_root.get_design_abstract(original_root.get_design_by_name("WRITERTEST")), LEFWriter::LayerWriteMode::IncludeWithAbstract), 0)
            << (writer.messages().empty() ? "" : writer.messages().front());

        LEFReader reread;
        ASSERT_EQ(reread.read_lef(out_path, written_root, "test_lib"), 0)
            << (reread.messages().empty() ? "" : reread.messages().front());
    }

    Root original_root;
    Root written_root;
    LEFReader reader;
    LEFWriter writer;
};

TEST_F(LEFWriterRoundtripFixture, RoundTripsUnits)
{
    const TechnologyId original_id = original_root.get_technology_ids().front();
    const TechnologyId written_id = written_root.get_technology_ids().front();
    EXPECT_DOUBLE_EQ(original_root.get_technology(original_id)->database_units_microns, written_root.get_technology(written_id)->database_units_microns);
}

TEST_F(LEFWriterRoundtripFixture, RoundTripsRoutingLayerProperties)
{
    const LayerData *original = original_root.get_layer(original_root.get_layer_by_name("M1"));
    const LayerData *written = written_root.get_layer(written_root.get_layer_by_name("M1"));
    ASSERT_TRUE(original != nullptr);
    ASSERT_TRUE(written != nullptr);

    EXPECT_EQ(original->type, written->type);
    EXPECT_EQ(original->direction, written->direction);

    ASSERT_TRUE(original->width.has_value());
    ASSERT_TRUE(written->width.has_value());
    EXPECT_EQ(*original->width, *written->width);

    ASSERT_TRUE(original->pitch.has_value());
    ASSERT_TRUE(written->pitch.has_value());
    EXPECT_EQ(*original->pitch, *written->pitch);

    ASSERT_TRUE(original->offset.has_value());
    ASSERT_TRUE(written->offset.has_value());
    EXPECT_EQ(*original->offset, *written->offset);

    ASSERT_EQ(original->spacing_rules.size(), 5u);
    ASSERT_EQ(written->spacing_rules.size(), 5u);
    for (size_t i = 0; i < original->spacing_rules.size(); i++)
        EXPECT_EQ(original->spacing_rules[i].distance, written->spacing_rules[i].distance) << "rule " << i;

    // SPACING 0.16 SAMENET PGONLY
    EXPECT_TRUE(original->spacing_rules[1].same_net);
    EXPECT_TRUE(written->spacing_rules[1].same_net);
    EXPECT_TRUE(original->spacing_rules[1].same_net_pg_only);
    EXPECT_TRUE(written->spacing_rules[1].same_net_pg_only);

    // SPACING 0.17 LENGTHTHRESHOLD 0.4
    ASSERT_TRUE(original->spacing_rules[2].length_threshold.has_value());
    ASSERT_TRUE(written->spacing_rules[2].length_threshold.has_value());
    EXPECT_EQ(*original->spacing_rules[2].length_threshold, *written->spacing_rules[2].length_threshold);

    // SPACING 0.18 RANGE 0.1 0.3
    ASSERT_TRUE(original->spacing_rules[3].range_min.has_value());
    ASSERT_TRUE(written->spacing_rules[3].range_min.has_value());
    EXPECT_EQ(*original->spacing_rules[3].range_min, *written->spacing_rules[3].range_min);
    ASSERT_TRUE(original->spacing_rules[3].range_max.has_value());
    ASSERT_TRUE(written->spacing_rules[3].range_max.has_value());
    EXPECT_EQ(*original->spacing_rules[3].range_max, *written->spacing_rules[3].range_max);

    // SPACING 0.19 RANGE 0.1 0.3 INFLUENCE 0.05 RANGE 0.2 0.4
    ASSERT_TRUE(original->spacing_rules[4].range_influence.has_value());
    ASSERT_TRUE(written->spacing_rules[4].range_influence.has_value());
    EXPECT_EQ(*original->spacing_rules[4].range_influence, *written->spacing_rules[4].range_influence);
    ASSERT_TRUE(original->spacing_rules[4].range_influence_range_min.has_value());
    ASSERT_TRUE(written->spacing_rules[4].range_influence_range_min.has_value());
    EXPECT_EQ(*original->spacing_rules[4].range_influence_range_min, *written->spacing_rules[4].range_influence_range_min);

    ASSERT_EQ(original->minimum_cuts.size(), 2u);
    ASSERT_EQ(written->minimum_cuts.size(), 2u);
    EXPECT_EQ(original->minimum_cuts[0].cuts, written->minimum_cuts[0].cuts);
    EXPECT_EQ(original->minimum_cuts[0].width, written->minimum_cuts[0].width);
    ASSERT_TRUE(original->minimum_cuts[1].within.has_value());
    ASSERT_TRUE(written->minimum_cuts[1].within.has_value());
    EXPECT_EQ(*original->minimum_cuts[1].within, *written->minimum_cuts[1].within);
    EXPECT_EQ(original->minimum_cuts[1].connection, written->minimum_cuts[1].connection);
    ASSERT_TRUE(original->minimum_cuts[1].length.has_value());
    ASSERT_TRUE(written->minimum_cuts[1].length.has_value());
    EXPECT_EQ(*original->minimum_cuts[1].length, *written->minimum_cuts[1].length);

    ASSERT_EQ(original->min_steps.size(), 3u);
    ASSERT_EQ(written->min_steps.size(), 3u);
    EXPECT_TRUE(original->min_steps[0].min_step_type.empty());
    EXPECT_TRUE(written->min_steps[0].min_step_type.empty());
    EXPECT_EQ(original->min_steps[1].min_step_type, "INSIDECORNER");
    EXPECT_EQ(written->min_steps[1].min_step_type, "INSIDECORNER");
    ASSERT_TRUE(original->min_steps[1].lengthsum.has_value());
    ASSERT_TRUE(written->min_steps[1].lengthsum.has_value());
    EXPECT_EQ(*original->min_steps[1].lengthsum, *written->min_steps[1].lengthsum);
    ASSERT_TRUE(original->min_steps[2].max_edges.has_value());
    ASSERT_TRUE(written->min_steps[2].max_edges.has_value());
    EXPECT_EQ(*original->min_steps[2].max_edges, *written->min_steps[2].max_edges);

    ASSERT_TRUE(original->spacing_table_parallel_run_length.has_value());
    ASSERT_TRUE(written->spacing_table_parallel_run_length.has_value());
    EXPECT_EQ(original->spacing_table_parallel_run_length->lengths, written->spacing_table_parallel_run_length->lengths);
    EXPECT_EQ(original->spacing_table_parallel_run_length->widths, written->spacing_table_parallel_run_length->widths);
    EXPECT_EQ(original->spacing_table_parallel_run_length->spacings, written->spacing_table_parallel_run_length->spacings);

    ASSERT_EQ(original->spacing_table_influence.size(), 2u);
    ASSERT_EQ(written->spacing_table_influence.size(), 2u);
    for (size_t i = 0; i < original->spacing_table_influence.size(); i++)
    {
        EXPECT_EQ(original->spacing_table_influence[i].width, written->spacing_table_influence[i].width);
        EXPECT_EQ(original->spacing_table_influence[i].distance, written->spacing_table_influence[i].distance);
        EXPECT_EQ(original->spacing_table_influence[i].spacing, written->spacing_table_influence[i].spacing);
    }

    ASSERT_TRUE(original->resistance.has_value());
    ASSERT_TRUE(written->resistance.has_value());
    EXPECT_DOUBLE_EQ(*original->resistance, *written->resistance);

    ASSERT_TRUE(original->capacitance.has_value());
    ASSERT_TRUE(written->capacitance.has_value());
    EXPECT_DOUBLE_EQ(*original->capacitance, *written->capacitance);

    ASSERT_TRUE(original->height.has_value());
    ASSERT_TRUE(written->height.has_value());
    EXPECT_EQ(*original->height, *written->height);

    ASSERT_TRUE(original->thickness.has_value());
    ASSERT_TRUE(written->thickness.has_value());
    EXPECT_EQ(*original->thickness, *written->thickness);

    ASSERT_TRUE(original->wire_extension.has_value());
    ASSERT_TRUE(written->wire_extension.has_value());
    EXPECT_EQ(*original->wire_extension, *written->wire_extension);

    // area/antenna_length are stored in dbu/dbu^2, not the LEF file's own
    // raw microns value - checked against the actual expected number
    // (not just original == written), since a reader/writer pair that
    // both forgot the unit conversion would still "round-trip" to the
    // same wrong value. writer_roundtrip.lef: DATABASE MICRONS 1000,
    // AREA 0.5 (microns^2) -> 0.5 * 1000^2 = 500000 dbu^2;
    // ANTENNALENGTHFACTOR 2.0 (microns) -> 2.0 * 1000 = 2000 dbu.
    ASSERT_TRUE(original->area.has_value());
    ASSERT_TRUE(written->area.has_value());
    EXPECT_EQ(*original->area, 500000);
    EXPECT_EQ(*written->area, 500000);

    ASSERT_TRUE(original->antenna_length.has_value());
    ASSERT_TRUE(written->antenna_length.has_value());
    EXPECT_EQ(*original->antenna_length, 2000);
    EXPECT_EQ(*written->antenna_length, 2000);
}

TEST_F(LEFWriterRoundtripFixture, RoundTripsCutLayerWidthAndSpacing)
{
    const LayerData *original = original_root.get_layer(original_root.get_layer_by_name("V1"));
    const LayerData *written = written_root.get_layer(written_root.get_layer_by_name("V1"));
    ASSERT_TRUE(original != nullptr);
    ASSERT_TRUE(written != nullptr);

    EXPECT_EQ(original->type, written->type);

    ASSERT_TRUE(original->width.has_value());
    ASSERT_TRUE(written->width.has_value());
    EXPECT_EQ(*original->width, *written->width);

    ASSERT_EQ(original->spacing_rules.size(), 7u);
    ASSERT_EQ(written->spacing_rules.size(), 7u);
    for (size_t i = 0; i < original->spacing_rules.size(); i++)
        EXPECT_EQ(original->spacing_rules[i].distance, written->spacing_rules[i].distance) << "rule " << i;

    // SPACING 0.22 ADJACENTCUTS 3 WITHIN 0.25
    ASSERT_TRUE(original->spacing_rules[1].adjacent_cuts.has_value());
    ASSERT_TRUE(written->spacing_rules[1].adjacent_cuts.has_value());
    EXPECT_EQ(*original->spacing_rules[1].adjacent_cuts, *written->spacing_rules[1].adjacent_cuts);
    ASSERT_TRUE(original->spacing_rules[1].adjacent_within.has_value());
    ASSERT_TRUE(written->spacing_rules[1].adjacent_within.has_value());
    EXPECT_EQ(*original->spacing_rules[1].adjacent_within, *written->spacing_rules[1].adjacent_within);
    EXPECT_FALSE(original->spacing_rules[1].adjacent_except_same_pg_net);
    EXPECT_FALSE(written->spacing_rules[1].adjacent_except_same_pg_net);

    // SPACING 0.23 ADJACENTCUTS 2 WITHIN 0.3 EXCEPTSAMEPGNET
    EXPECT_TRUE(original->spacing_rules[2].adjacent_except_same_pg_net);
    EXPECT_TRUE(written->spacing_rules[2].adjacent_except_same_pg_net);

    // SPACING 0.6 LAYER M1
    EXPECT_EQ(original->spacing_rules[3].second_layer_name, "M1");
    EXPECT_EQ(written->spacing_rules[3].second_layer_name, "M1");

    // SPACING 1.5 PARALLELOVERLAP
    EXPECT_TRUE(original->spacing_rules[4].parallel_overlap);
    EXPECT_TRUE(written->spacing_rules[4].parallel_overlap);

    // SPACING 0.24 CENTERTOCENTER
    EXPECT_TRUE(original->spacing_rules[5].center_to_center);
    EXPECT_TRUE(written->spacing_rules[5].center_to_center);

    // SPACING 0.25 SAMENET
    EXPECT_TRUE(original->spacing_rules[6].same_net);
    EXPECT_TRUE(written->spacing_rules[6].same_net);
    EXPECT_FALSE(original->spacing_rules[6].same_net_pg_only);
    EXPECT_FALSE(written->spacing_rules[6].same_net_pg_only);

    ASSERT_EQ(original->spacing_table_orthogonal.size(), 2u);
    ASSERT_EQ(written->spacing_table_orthogonal.size(), 2u);
    for (size_t i = 0; i < original->spacing_table_orthogonal.size(); i++)
    {
        EXPECT_EQ(original->spacing_table_orthogonal[i].cut_within, written->spacing_table_orthogonal[i].cut_within);
        EXPECT_EQ(original->spacing_table_orthogonal[i].ortho_spacing, written->spacing_table_orthogonal[i].ortho_spacing);
    }

    // V1 has no PITCH/OFFSET/AREA/RESISTANCE/... - proving they stay unset
    // through a round trip too, not accidentally defaulted to 0/written as
    // a spurious statement.
    EXPECT_FALSE(original->pitch.has_value());
    EXPECT_FALSE(written->pitch.has_value());
    EXPECT_FALSE(original->resistance.has_value());
    EXPECT_FALSE(written->resistance.has_value());
}

TEST_F(LEFWriterRoundtripFixture, RoundTripsAPlainViaWithLayerGeometryAndResistance)
{
    const ViaData *original = original_root.get_via(original_root.get_via_by_name("VIA1"));
    const ViaData *written = written_root.get_via(written_root.get_via_by_name("VIA1"));
    ASSERT_TRUE(original != nullptr);
    ASSERT_TRUE(written != nullptr);

    EXPECT_EQ(original->is_default, written->is_default);
    ASSERT_TRUE(original->resistance.has_value());
    ASSERT_TRUE(written->resistance.has_value());
    EXPECT_DOUBLE_EQ(*original->resistance, *written->resistance);
    EXPECT_FALSE(original->via_rule.has_value());
    EXPECT_FALSE(written->via_rule.has_value());

    ASSERT_EQ(original->layers.size(), written->layers.size());
    for (size_t i = 0; i < original->layers.size(); i++)
    {
        const ViaLayer &o = original->layers[i];
        const ViaLayer &w = written->layers[i];
        EXPECT_EQ(o.layer_name, w.layer_name);
        ASSERT_EQ(o.rects.size(), w.rects.size());
        for (size_t j = 0; j < o.rects.size(); j++)
        {
            EXPECT_EQ(o.rects[j].ll.x, w.rects[j].ll.x);
            EXPECT_EQ(o.rects[j].ll.y, w.rects[j].ll.y);
            EXPECT_EQ(o.rects[j].ur.x, w.rects[j].ur.x);
            EXPECT_EQ(o.rects[j].ur.y, w.rects[j].ur.y);
        }
    }
}

TEST_F(LEFWriterRoundtripFixture, RoundTripsAGenerateViaRuleWithEnclosureAndACutLayer)
{
    const ViaRuleData *original = original_root.get_via_rule(original_root.get_via_rule_by_name("VIARULE1"));
    const ViaRuleData *written = written_root.get_via_rule(written_root.get_via_rule_by_name("VIARULE1"));
    ASSERT_TRUE(original != nullptr);
    ASSERT_TRUE(written != nullptr);

    EXPECT_TRUE(original->is_generate);
    EXPECT_TRUE(written->is_generate);
    ASSERT_EQ(original->layers.size(), written->layers.size());
    ASSERT_EQ(original->layers.size(), 3u);

    for (size_t i = 0; i < 2; i++)
    {
        const ViaRuleLayer &o = original->layers[i];
        const ViaRuleLayer &w = written->layers[i];
        EXPECT_EQ(o.layer_name, w.layer_name);
        ASSERT_TRUE(o.enclosure_overhang1.has_value());
        ASSERT_TRUE(w.enclosure_overhang1.has_value());
        EXPECT_EQ(*o.enclosure_overhang1, *w.enclosure_overhang1);
        ASSERT_TRUE(o.enclosure_overhang2.has_value());
        ASSERT_TRUE(w.enclosure_overhang2.has_value());
        EXPECT_EQ(*o.enclosure_overhang2, *w.enclosure_overhang2);
        ASSERT_TRUE(o.width_min.has_value());
        ASSERT_TRUE(w.width_min.has_value());
        EXPECT_EQ(*o.width_min, *w.width_min);
        ASSERT_TRUE(o.width_max.has_value());
        ASSERT_TRUE(w.width_max.has_value());
        EXPECT_EQ(*o.width_max, *w.width_max);
    }

    const ViaRuleLayer &original_cut = original->layers[2];
    const ViaRuleLayer &written_cut = written->layers[2];
    EXPECT_EQ(original_cut.layer_name, written_cut.layer_name);
    ASSERT_TRUE(original_cut.rect.has_value());
    ASSERT_TRUE(written_cut.rect.has_value());
    EXPECT_EQ(original_cut.rect->ll.x, written_cut.rect->ll.x);
    EXPECT_EQ(original_cut.rect->ur.x, written_cut.rect->ur.x);
    ASSERT_TRUE(original_cut.resistance.has_value());
    ASSERT_TRUE(written_cut.resistance.has_value());
    EXPECT_DOUBLE_EQ(*original_cut.resistance, *written_cut.resistance);
}

TEST_F(LEFWriterRoundtripFixture, RoundTripsSiteDefinitions)
{
    const SiteData *original_core = original_root.get_site(original_root.get_site_by_name("CORE"));
    const SiteData *written_core = written_root.get_site(written_root.get_site_by_name("CORE"));
    ASSERT_TRUE(original_core != nullptr);
    ASSERT_TRUE(written_core != nullptr);

    EXPECT_EQ(original_core->site_class, written_core->site_class);
    ASSERT_TRUE(original_core->size.has_value());
    ASSERT_TRUE(written_core->size.has_value());
    EXPECT_EQ(original_core->size->x, written_core->size->x);
    EXPECT_EQ(original_core->size->y, written_core->size->y);
    EXPECT_EQ(original_core->symmetry.x, written_core->symmetry.x);
    EXPECT_EQ(original_core->symmetry.y, written_core->symmetry.y);

    const SiteData *original_dblcore = original_root.get_site(original_root.get_site_by_name("DBLCORE"));
    const SiteData *written_dblcore = written_root.get_site(written_root.get_site_by_name("DBLCORE"));
    ASSERT_TRUE(original_dblcore != nullptr);
    ASSERT_TRUE(written_dblcore != nullptr);

    ASSERT_EQ(original_dblcore->row_pattern.size(), written_dblcore->row_pattern.size());
    for (size_t i = 0; i < original_dblcore->row_pattern.size(); i++)
    {
        EXPECT_EQ(original_dblcore->row_pattern[i].site_name, written_dblcore->row_pattern[i].site_name);
        EXPECT_EQ(original_dblcore->row_pattern[i].orient, written_dblcore->row_pattern[i].orient);
    }
}

TEST_F(LEFWriterRoundtripFixture, RoundTripsANonDefaultRuleWithAnEmbeddedViaAndUseStatements)
{
    const NonDefaultRuleData *original = original_root.get_non_default_rule(original_root.get_non_default_rule_by_name("WIDE_M1"));
    const NonDefaultRuleData *written = written_root.get_non_default_rule(written_root.get_non_default_rule_by_name("WIDE_M1"));
    ASSERT_TRUE(original != nullptr);
    ASSERT_TRUE(written != nullptr);

    EXPECT_EQ(original->hard_spacing, written->hard_spacing);

    ASSERT_EQ(original->layers.size(), written->layers.size());
    ASSERT_EQ(original->layers.size(), 1u);
    EXPECT_EQ(original->layers[0].layer_name, written->layers[0].layer_name);
    ASSERT_TRUE(original->layers[0].width.has_value());
    ASSERT_TRUE(written->layers[0].width.has_value());
    EXPECT_EQ(*original->layers[0].width, *written->layers[0].width);
    ASSERT_TRUE(original->layers[0].spacing.has_value());
    ASSERT_TRUE(written->layers[0].spacing.has_value());
    EXPECT_EQ(*original->layers[0].spacing, *written->layers[0].spacing);

    ASSERT_EQ(original->vias.size(), written->vias.size());
    ASSERT_EQ(original->vias.size(), 1u);
    EXPECT_EQ(original->vias[0].name, written->vias[0].name);
    ASSERT_TRUE(original->vias[0].resistance.has_value());
    ASSERT_TRUE(written->vias[0].resistance.has_value());
    EXPECT_DOUBLE_EQ(*original->vias[0].resistance, *written->vias[0].resistance);
    ASSERT_EQ(original->vias[0].layers.size(), written->vias[0].layers.size());

    EXPECT_EQ(original->use_via_names, written->use_via_names);

    ASSERT_EQ(original->min_cuts.size(), written->min_cuts.size());
    ASSERT_EQ(original->min_cuts.size(), 1u);
    EXPECT_EQ(original->min_cuts[0].cut_layer_name, written->min_cuts[0].cut_layer_name);
    EXPECT_EQ(original->min_cuts[0].num_cuts, written->min_cuts[0].num_cuts);
}

TEST_F(LEFWriterRoundtripFixture, RoundTripsMacroClassOriginSizeSymmetryAndSite)
{
    const DesignId original_design_id = original_root.get_design_by_name("WRITERTEST");
    const DesignId written_design_id = written_root.get_design_by_name("WRITERTEST");
    ASSERT_TRUE(original_design_id.valid());
    ASSERT_TRUE(written_design_id.valid());

    const AbstractData *original = original_root.get_abstract(original_root.get_design_abstract(original_design_id));
    const AbstractData *written = written_root.get_abstract(written_root.get_design_abstract(written_design_id));
    ASSERT_TRUE(original != nullptr);
    ASSERT_TRUE(written != nullptr);

    EXPECT_EQ(original->type, written->type);
    EXPECT_EQ(original->origin.x, written->origin.x);
    EXPECT_EQ(original->origin.y, written->origin.y);
    EXPECT_EQ(original->size.x, written->size.x);
    EXPECT_EQ(original->size.y, written->size.y);
    EXPECT_EQ(original->symmetry.x, written->symmetry.x);
    EXPECT_EQ(original->symmetry.y, written->symmetry.y);
    EXPECT_EQ(original->symmetry.r90, written->symmetry.r90);
    EXPECT_EQ(original->site, written->site);
    EXPECT_EQ(original->eeq, written->eeq);
    EXPECT_EQ(original->is_fixed_mask, written->is_fixed_mask);
    // leq/power/source are never populated in the first place for a
    // VERSION 5.8 file (lef.y's own grammar drops all three silently on
    // read at that version) - see write_macro's own comment.
    EXPECT_TRUE(original->leq.empty());
    EXPECT_TRUE(written->leq.empty());
}

TEST_F(LEFWriterRoundtripFixture, RoundTripsPinDirectionAndPortGeometryIncludingAMixedRectPolygonPath)
{
    const DesignId original_design_id = original_root.get_design_by_name("WRITERTEST");
    const DesignId written_design_id = written_root.get_design_by_name("WRITERTEST");
    const AbstractId original_abstract_id = original_root.get_design_abstract(original_design_id);
    const AbstractId written_abstract_id = written_root.get_design_abstract(written_design_id);

    ASSERT_EQ(original_root.get_abstract_terminals(original_abstract_id).size(), 1u);
    ASSERT_EQ(written_root.get_abstract_terminals(written_abstract_id).size(), 1u);

    const TerminalId original_terminal_id = original_root.get_abstract_terminals(original_abstract_id).front();
    const TerminalId written_terminal_id = written_root.get_abstract_terminals(written_abstract_id).front();

    const TerminalData *original_terminal = original_root.get_terminal(original_terminal_id);
    const TerminalData *written_terminal = written_root.get_terminal(written_terminal_id);
    EXPECT_EQ(original_terminal->direction, written_terminal->direction);
    EXPECT_EQ(original_terminal->use, written_terminal->use);
    EXPECT_EQ(original_terminal->shape, written_terminal->shape);
    EXPECT_EQ(original_terminal->must_join, written_terminal->must_join);
    EXPECT_EQ(original_terminal->net_expr, written_terminal->net_expr);
    // leq is never populated at VERSION 5.8 (obsolete on both read and
    // write - see write_terminal's own comment).
    EXPECT_TRUE(original_terminal->leq.empty());
    EXPECT_TRUE(written_terminal->leq.empty());

    ASSERT_EQ(original_root.get_terminal_ports(original_terminal_id).size(), 1u);
    ASSERT_EQ(written_root.get_terminal_ports(written_terminal_id).size(), 1u);

    const TerminalPortData *original_port = original_root.get_terminal_port(original_root.get_terminal_ports(original_terminal_id).front());
    const TerminalPortData *written_port = written_root.get_terminal_port(written_root.get_terminal_ports(written_terminal_id).front());

    ASSERT_EQ(original_port->shapes.size(), written_port->shapes.size());
    for (size_t i = 0; i < original_port->shapes.size(); i++)
        expect_shapes_equal(original_port->shapes[i], written_port->shapes[i]);
}

TEST_F(LEFWriterRoundtripFixture, RoundTripsObstructionRectIterateAndPathIterateAsRawStatementsNotExpanded)
{
    const DesignId original_design_id = original_root.get_design_by_name("WRITERTEST");
    const DesignId written_design_id = written_root.get_design_by_name("WRITERTEST");
    const AbstractId original_abstract_id = original_root.get_design_abstract(original_design_id);
    const AbstractId written_abstract_id = written_root.get_design_abstract(written_design_id);

    ASSERT_EQ(original_root.get_abstract_obstructions(original_abstract_id).size(), 1u);
    ASSERT_EQ(written_root.get_abstract_obstructions(written_abstract_id).size(), 1u);

    const ObstructionData *original = original_root.get_obstruction(original_root.get_abstract_obstructions(original_abstract_id).front());
    const ObstructionData *written = written_root.get_obstruction(written_root.get_abstract_obstructions(written_abstract_id).front());

    ASSERT_EQ(original->shapes.size(), written->shapes.size());
    for (size_t i = 0; i < original->shapes.size(); i++)
        expect_shapes_equal(original->shapes[i], written->shapes[i]);

    // Belt-and-suspenders on the specific claim in this test's name: both
    // the original read and the round-tripped re-read see the ITERATE as
    // one raw statement each (num_x*num_y == 4/2), not 4/2 separately
    // materialized rects/paths - proving LEFWriter re-emitted compact
    // ITERATE syntax rather than expanded individual RECT/PATH statements.
    ASSERT_EQ(original->shapes.front().rect_iterates.size(), 1u);
    ASSERT_EQ(written->shapes.front().rect_iterates.size(), 1u);
    ASSERT_EQ(original->shapes.front().path_iterates.size(), 1u);
    ASSERT_EQ(written->shapes.front().path_iterates.size(), 1u);
}

TEST_F(LEFWriterRoundtripFixture, LayerWriteModeNoneWritesNoLayers)
{
    const std::string out_path = scratch_output_path();
    LEFWriter local_writer;
    ASSERT_EQ(local_writer.write_lef(out_path, original_root, original_root.get_design_abstract(original_root.get_design_by_name("WRITERTEST")), LEFWriter::LayerWriteMode::None), 0);

    // LayerWriteMode::None deliberately omits UNITS too (see write_lef's
    // own comment - it's gated on the same `mode != None` check as the
    // layers), so this MACRO-only file has no DATABASE MICRONS of its
    // own - pre-seed reread_root's Technology with one, mirroring the
    // realistic "tech file read first, macro file second, same Root"
    // pattern this mode exists for (matches load-order convention in
    // lef_reader.cpp's own is_technology_empty()-gated create-vs-reuse).
    Root reread_root;
    reread_root.create_technology(TechnologyData{.database_units_microns = 1000.0});
    LEFReader reread;
    ASSERT_EQ(reread.read_lef(out_path, reread_root, "test_lib"), 0);
    EXPECT_TRUE(reread_root.is_layer_empty());
    EXPECT_TRUE(reread_root.get_design_by_name("WRITERTEST").valid());
}

TEST_F(LEFWriterRoundtripFixture, LayerWriteModeTechnologyOnlyWritesNoMacro)
{
    const std::string out_path = scratch_output_path();
    LEFWriter local_writer;
    ASSERT_EQ(local_writer.write_lef(out_path, original_root, original_root.get_design_abstract(original_root.get_design_by_name("WRITERTEST")), LEFWriter::LayerWriteMode::TechnologyOnly), 0);

    Root reread_root;
    LEFReader reread;
    ASSERT_EQ(reread.read_lef(out_path, reread_root, "test_lib"), 0);
    EXPECT_TRUE(reread_root.get_layer_by_name("M1").valid());
    EXPECT_FALSE(reread_root.get_design_by_name("WRITERTEST").valid());
}

TEST(LEFWriterTwoWordMacroClass, SplitsATwoWordAbstractTypeIntoBaseAndSubtypeInsteadOfFailingTheWholeWrite)
{
    // LEF's CLASS statement is `CLASS <base> [<subtype>] ;` - lef.y's own
    // class_type grammar rule combines both into ONE space-joined string
    // (e.g. "CORE WELLTAP") in lefiMacro::macroClass(), which is exactly
    // what lef_reader.cpp stores verbatim into AbstractData::type. Found
    // by running the lef_roundtrip_diff dev tool against complete.5.8.lef -
    // passing that combined string straight to lefwMacroClass's first
    // argument (which validates it against a fixed known-base-class list)
    // silently failed the ENTIRE MACRO write with LEFW_BAD_DATA for every
    // macro using a two-word CLASS (mycorewelltap/myblackbox/mysoft/
    // mydriver in that fixture all hit this before the fix).
    Root root;
    const LibraryId library_id = root.create_library(LibraryData{.name = "test_lib"});
    const DesignId design_id = root.create_design(DesignData{.library = library_id, .name = "WELLTAPTEST"});
    root.create_abstract(AbstractData{.design = design_id, .type = "CORE WELLTAP"});

    const std::string out_path = (std::filesystem::temp_directory_path() / "le_lef_writer_two_word_class.lef").string();
    LEFWriter writer;
    ASSERT_EQ(writer.write_lef(out_path, root, root.get_design_abstract(design_id), LEFWriter::LayerWriteMode::None), 0)
        << (writer.messages().empty() ? "" : writer.messages().front());

    // LayerWriteMode::None omits UNITS along with layers - this MACRO's
    // own ORIGIN/SIZE statements still need a database_units_microns to
    // convert against on re-read (see LEFWriterRoundtripFixture's
    // LayerWriteModeNoneWritesNoLayers test for the same pre-seeding).
    Root reread_root;
    reread_root.create_technology(TechnologyData{.database_units_microns = 1000.0});
    LEFReader reread;
    ASSERT_EQ(reread.read_lef(out_path, reread_root, "test_lib"), 0);

    const DesignId reread_design_id = reread_root.get_design_by_name("WELLTAPTEST");
    ASSERT_TRUE(reread_design_id.valid());
    const AbstractData *reread_abstract = reread_root.get_abstract(reread_root.get_design_abstract(reread_design_id));
    ASSERT_TRUE(reread_abstract != nullptr);
    EXPECT_EQ(reread_abstract->type, "CORE WELLTAP");
}

TEST(LEFWriterErrorHandling, FailsGracefullyWhenTheOutputPathCannotBeOpened)
{
    Root root;
    LEFWriter writer;
    // A directory that doesn't exist - fopen(..., "w") fails.
    const int result = writer.write_lef("/nonexistent_directory_le_test/out.lef", root, AbstractId{}, LEFWriter::LayerWriteMode::None);
    EXPECT_NE(result, 0);
    ASSERT_FALSE(writer.messages().empty());
}

TEST(LEFWriterErrorHandling, WritesJustTheEndLibraryLineForAnEmptyRoot)
{
    Root root;
    LEFWriter writer;
    const std::string out_path = (std::filesystem::temp_directory_path() / "le_lef_writer_empty.lef").string();
    EXPECT_EQ(writer.write_lef(out_path, root, AbstractId{}, LEFWriter::LayerWriteMode::IncludeWithAbstract), 0);
}

namespace
{
    // lefdiff itself doesn't diff anything (despite the name) - its real
    // main() (differLef.cpp, confirmed by reading it) takes
    // `fileName1 fileName2 lefOut1 lefOut2` and dumps a normalized text
    // representation of each input to lefOut1/lefOut2; the caller is
    // expected to sort and compare those two dumps itself (see that
    // file's own "later these files will be sorted for comparison"
    // comment) - confirmed directly by the user, who'd hit this exact
    // "lefdiff doesn't work, it just writes sorted-looking output" gotcha
    // before.
    std::vector<std::string> sorted_lines(const std::string &path)
    {
        std::ifstream in(path);
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(in, line))
            lines.push_back(line);
        std::sort(lines.begin(), lines.end());
        return lines;
    }
}

// This is the actual UPDATES.md item 12 verification loop, automated: read
// a LEF file, write it back out via LEFWriter, and confirm the vendored
// lefdiff tool's own normalized dumps of the two files match - i.e. the
// round trip lost nothing lefdiff itself can see. writer_roundtrip.lef is
// deliberately scoped to exactly what Phase 1 supports (see its own
// comment/the fixture file), so this is expected to pass cleanly today;
// it's the harness later phases reuse to verify their own added coverage
// (each new construct added to writer_roundtrip.lef either round-trips
// clean or shows up here as a real, specific diff).
TEST(LEFWriterLefdiffFidelity, WriterRoundtripFixtureMatchesTheOriginalPerLefdiff)
{
    Root root;
    LEFReader reader;
    ASSERT_EQ(reader.read_lef(fixture_path("writer_roundtrip.lef"), root, "test_lib"), 0);

    const std::string written_path = (std::filesystem::temp_directory_path() / "le_lefdiff_written.lef").string();
    LEFWriter writer;
    ASSERT_EQ(writer.write_lef(written_path, root, root.get_design_abstract(root.get_design_by_name("WRITERTEST")), LEFWriter::LayerWriteMode::IncludeWithAbstract), 0);

    const std::string dump1_path = (std::filesystem::temp_directory_path() / "le_lefdiff_dump1.txt").string();
    const std::string dump2_path = (std::filesystem::temp_directory_path() / "le_lefdiff_dump2.txt").string();

    const std::string command = fmt::format(
        "\"{}\" \"{}\" \"{}\" \"{}\" \"{}\" > /dev/null 2>&1",
        LEFDIFF_BIN, fixture_path("writer_roundtrip.lef"), written_path, dump1_path, dump2_path);
    ASSERT_EQ(std::system(command.c_str()), 0) << "lefdiff itself failed to run: " << command;

    EXPECT_EQ(sorted_lines(dump1_path), sorted_lines(dump2_path));
}
