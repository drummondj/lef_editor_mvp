#include "../api.hpp"
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <map>
#include <string>
#include <thread>

namespace
{
    std::string fixture_path(const std::string &name)
    {
        return std::string(API_TEST_FIXTURES_DIR) + "/" + name;
    }

    // A moderately-sized (not stress_data.hpp's full 1M-shape) generated
    // LEF - one PIN per shape, matching stress_data.hpp's "fresh LAYER
    // before every geometry item" trick so shapes_from_parser finalizes
    // many separate Shape entries - giving Pipeline::filter_by_layer_
    // visibility's grouping loop (the exact std::map mutation that raced
    // in the reported crash) enough iterations per call to take
    // measurable time, without making this test itself slow. Written to
    // a scratch temp file rather than a checked-in fixture, since it's
    // generated, not hand-authored.
    std::string generate_concurrency_stress_lef(int shape_count)
    {
        const std::filesystem::path path = std::filesystem::temp_directory_path() / "le_concurrency_stress.lef";

        std::ofstream out(path, std::ios::trunc);
        out << "VERSION 5.8 ;\nBUSBITCHARS \"<>\" ;\nDIVIDERCHAR \"/\" ;\n\n";
        out << "UNITS\n   DATABASE MICRONS 1000 ;\nEND UNITS\n\n";
        out << "LAYER M1\n   TYPE ROUTING ;\n   WIDTH 1 ;\n   PITCH 2 ;\n   DIRECTION HORIZONTAL ;\nEND M1\n\n";
        out << "MACRO CONCURRENCYSTRESS\n   CLASS CORE ;\n   SIZE 100000 BY 100000 ;\n\n";

        for (int i = 0; i < shape_count; ++i)
        {
            const double x = (i % 1000) * 100.0;
            const double y = (i / 1000) * 100.0;
            out << "   PIN P" << i << "\n      DIRECTION INPUT ;\n      PORT\n         LAYER M1 ;\n"
                << "         RECT " << x << " " << y << " " << (x + 1.0) << " " << (y + 1.0) << " ;\n      END\n   END P" << i << "\n";
        }

        out << "END CONCURRENCYSTRESS\n";
        return path.string();
    }

    // ROUTING layers (e.g. the pin's M1) render with a tiled FillPattern
    // (diagonal stripes) rather than a flat fill - unlike a flat fill, a
    // single fixed pixel can land in a transparent gap by coincidence of
    // the pattern's phase. Scans a region instead of trusting one point.
    bool region_has_opaque_pixel(const LePixelBuffer &buffer, int x0, int y0, int x1, int y1)
    {
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
            {
                const uint8_t *p = buffer.data + static_cast<size_t>(y) * static_cast<size_t>(buffer.row_bytes) + static_cast<size_t>(x) * 4;
                if (p[3] > 0)
                    return true;
            }
        return false;
    }

    // The mouse-snap cursor box is drawn pure opaque red (see
    // Renderer::kCursorBoxColor) - distinct from the grid's gray/white
    // dots and any layer fill color, so "clearly red-dominant and opaque"
    // is a reliable way to detect it without depending on exact stroke
    // antialiasing.
    bool region_has_red_cursor_pixel(const LePixelBuffer &buffer, int x0, int y0, int x1, int y1)
    {
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
            {
                const uint8_t *p = buffer.data + static_cast<size_t>(y) * static_cast<size_t>(buffer.row_bytes) + static_cast<size_t>(x) * 4;
                if (p[0] > 200 && p[1] < 50 && p[2] < 50 && p[3] > 200)
                    return true;
            }
        return false;
    }

    // The hover outline is drawn pure opaque yellow (see
    // Renderer::kHoverOutlineColor) - distinct from the cursor box (red),
    // origin marker (amber), and grid (gray/white), so "clearly
    // yellow-dominant and opaque" reliably detects it without depending
    // on exact stroke antialiasing.
    bool region_has_yellow_hover_pixel(const LePixelBuffer &buffer, int x0, int y0, int x1, int y1)
    {
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
            {
                const uint8_t *p = buffer.data + static_cast<size_t>(y) * static_cast<size_t>(buffer.row_bytes) + static_cast<size_t>(x) * 4;
                if (p[0] > 200 && p[1] > 200 && p[2] < 50 && p[3] > 200)
                    return true;
            }
        return false;
    }

    // The selection outline is drawn pure opaque white (see
    // Renderer::kSelectionOutlineColor) - distinct from every other
    // overlay/grid color and from every default layer palette color (none
    // of which are pure white), so "R/G/B all near max and opaque"
    // reliably detects it without depending on exact stroke antialiasing.
    bool region_has_white_selection_pixel(const LePixelBuffer &buffer, int x0, int y0, int x1, int y1)
    {
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
            {
                const uint8_t *p = buffer.data + static_cast<size_t>(y) * static_cast<size_t>(buffer.row_bytes) + static_cast<size_t>(x) * 4;
                if (p[0] > 250 && p[1] > 250 && p[2] > 250 && p[3] > 200)
                    return true;
            }
        return false;
    }

    struct ApiFixture : public ::testing::Test
    {
        void SetUp() override { handle = le_create(); }
        void TearDown() override { le_destroy(handle); }

        LeHandle *handle = nullptr;
    };
}

TEST(Api, CreateNeverReturnsNull)
{
    LeHandle *handle = le_create();
    ASSERT_NE(handle, nullptr);
    le_destroy(handle);
}

TEST(Api, DestroyNullHandleDoesNotCrash)
{
    le_destroy(nullptr);
}

TEST_F(ApiFixture, ReadLefWithMissingFileReturnsNonzeroAndLoadsNoDesigns)
{
    EXPECT_NE(le_read_lef(handle, "/does/not/exist.lef"), 0);
    EXPECT_EQ(le_design_count(handle), 0);
}

TEST_F(ApiFixture, ReadLefWithNullHandleOrPathReturnsNonzero)
{
    EXPECT_NE(le_read_lef(nullptr, fixture_path("testcell.lef").c_str()), 0);
    EXPECT_NE(le_read_lef(handle, nullptr), 0);
}

TEST_F(ApiFixture, ReadLefWithValidFileSucceedsAndPopulatesOneDesign)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_design_count(handle), 1);

    const char *name = le_design_name(handle, 0);
    ASSERT_NE(name, nullptr);
    EXPECT_STREQ(name, "TESTCELL");
}

TEST_F(ApiFixture, DesignCountAndNameAreZeroOrNullForNullHandle)
{
    EXPECT_EQ(le_design_count(nullptr), 0);
    EXPECT_EQ(le_design_name(nullptr, 0), nullptr);
}

TEST_F(ApiFixture, DesignNameOutOfRangeReturnsNull)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    EXPECT_EQ(le_design_name(handle, 1), nullptr);
    EXPECT_EQ(le_design_name(handle, -1), nullptr);
}

TEST_F(ApiFixture, SetCurrentDesignOutOfRangeReturnsNonzero)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    EXPECT_NE(le_set_current_design(handle, 1), 0);
    EXPECT_NE(le_set_current_design(handle, -1), 0);
    EXPECT_NE(le_set_current_design(nullptr, 0), 0);
}

TEST_F(ApiFixture, SetCurrentDesignValidIndexSucceeds)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    EXPECT_EQ(le_set_current_design(handle, 0), 0);
}

TEST_F(ApiFixture, RenderPixelBufferWithNullHandleReturnsAllZero)
{
    LePixelBuffer buffer = le_render_pixel_buffer(nullptr);
    EXPECT_EQ(buffer.data, nullptr);
    EXPECT_EQ(buffer.width, 0);
    EXPECT_EQ(buffer.height, 0);
    EXPECT_EQ(buffer.row_bytes, 0);
}

TEST_F(ApiFixture, RenderPixelBufferBeforeAnySetupDoesNotCrashAndStaysWithinRequestedViewport)
{
    // No LEF loaded, no Design selected, but a real viewport size - the
    // degenerate "nothing to draw yet" case should still produce a valid,
    // correctly-sized buffer rather than crash.
    le_set_viewport_size(handle, 50, 50);
    LePixelBuffer buffer = le_render_pixel_buffer(handle);
    EXPECT_NE(buffer.data, nullptr);
    EXPECT_EQ(buffer.width, 50);
    EXPECT_EQ(buffer.height, 50);
    EXPECT_GE(buffer.row_bytes, 50 * 4);
}

TEST_F(ApiFixture, RenderPixelBufferWithZeroSizedViewportDoesNotCrash)
{
    LePixelBuffer buffer = le_render_pixel_buffer(handle);
    EXPECT_EQ(buffer.width, 0);
    EXPECT_EQ(buffer.height, 0);
}

TEST_F(ApiFixture, RenderPixelBufferProducesTheRequestedDimensions)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design(handle, 0), 0);

    le_set_viewport_size(handle, 200, 200);

    // Scene starts at scale 1.0 / pan (0, 0) - le_zoom to scale 10.0 (10
    // px/dbu-micron-ish, matches DATABASE MICRONS 1000 -> 1000 dbu/micron)
    // anchored at image pixel (0, 200) (bottom-left corner, i.e. dbu (0,
    // 0) at the starting pan/scale) keeps pan pinned at (0, 0) exactly.
    le_zoom(handle, 9.0, 0, 200);

    LePixelBuffer buffer = le_render_pixel_buffer(handle);
    EXPECT_EQ(buffer.width, 200);
    EXPECT_EQ(buffer.height, 200);
}

TEST_F(ApiFixture, FitSceneWithNullHandleDoesNotCrash)
{
    le_fit_scene(nullptr, 10);
}

TEST_F(ApiFixture, FitSceneWithNoDesignSelectedFallsBackToDefaultScaleAndPan)
{
    le_set_viewport_size(handle, 100, 100);
    le_fit_scene(handle, 10);

    LePixelBuffer buffer = le_render_pixel_buffer(handle);
    EXPECT_EQ(buffer.width, 100);
    EXPECT_EQ(buffer.height, 100);
}

TEST_F(ApiFixture, FitSceneFillsTheViewportWithThePinVisible)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design(handle, 0), 0);

    le_set_viewport_size(handle, 200, 200);
    le_fit_scene(handle, 10);

    LePixelBuffer buffer = le_render_pixel_buffer(handle);
    ASSERT_NE(buffer.data, nullptr);

    // The macro's content should now fill most of the 200x200 viewport -
    // scan a block around the center, which sits inside both the 10x10
    // micron macro boundary and PIN A's (2,2)-(8,8) micron RECT. PIN A is
    // on M1 (a ROUTING layer), so its fill is a tiled diagonal-stripe
    // pattern rather than a flat fill - a single fixed pixel could land in
    // a transparent gap by coincidence of the pattern's phase.
    EXPECT_TRUE(region_has_opaque_pixel(buffer, 80, 80, 120, 120));
}

TEST_F(ApiFixture, LibraryCountAndAtAreZeroOrInvalidForNullHandle)
{
    EXPECT_EQ(le_library_count(nullptr), 0);

    const LeLibraryInfo info = le_library_at(nullptr, 0);
    EXPECT_EQ(info.id.index, UINT32_MAX);
    EXPECT_EQ(info.name, nullptr);
}

TEST_F(ApiFixture, LibraryAtOutOfRangeReturnsInvalidRow)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);

    const LeLibraryInfo info = le_library_at(handle, 1);
    EXPECT_EQ(info.id.index, UINT32_MAX);
    EXPECT_EQ(info.name, nullptr);
}

TEST_F(ApiFixture, EachLefReadCreatesItsOwnLibrary)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_read_lef(handle, fixture_path("othercell.lef").c_str()), 0);

    ASSERT_EQ(le_library_count(handle), 2);

    const LeLibraryInfo lib0 = le_library_at(handle, 0);
    ASSERT_NE(lib0.name, nullptr);
    EXPECT_STREQ(lib0.name, "testcell");
    EXPECT_TRUE(lib0.id.index != UINT32_MAX);

    const LeLibraryInfo lib1 = le_library_at(handle, 1);
    ASSERT_NE(lib1.name, nullptr);
    EXPECT_STREQ(lib1.name, "othercell");
    EXPECT_NE(lib1.id.index, lib0.id.index);
}

TEST_F(ApiFixture, LibraryDesignCountAndAtAreZeroOrInvalidForNullHandleOrBadIndex)
{
    EXPECT_EQ(le_library_design_count(nullptr, 0), 0);

    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    EXPECT_EQ(le_library_design_count(handle, 1), 0);  // out-of-range library
    EXPECT_EQ(le_library_design_count(handle, -1), 0); // negative

    const LeDesignInfo info = le_library_design_at(handle, 1, 0);
    EXPECT_EQ(info.id.index, UINT32_MAX);
    EXPECT_EQ(info.library_id.index, UINT32_MAX);
    EXPECT_EQ(info.abstract_id.index, UINT32_MAX);
    EXPECT_EQ(info.name, nullptr);
}

TEST_F(ApiFixture, LibraryDesignAtReturnsTheDesignAndItsAbstractId)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);

    ASSERT_EQ(le_library_design_count(handle, 0), 1);

    const LeLibraryInfo library = le_library_at(handle, 0);
    const LeDesignInfo design = le_library_design_at(handle, 0, 0);

    ASSERT_NE(design.name, nullptr);
    EXPECT_STREQ(design.name, "TESTCELL");
    EXPECT_EQ(design.library_id.index, library.id.index);
    EXPECT_EQ(design.library_id.generation, library.id.generation);
    EXPECT_NE(design.id.index, UINT32_MAX);
    EXPECT_NE(design.abstract_id.index, UINT32_MAX); // every read Design gets an Abstract view
}

TEST_F(ApiFixture, SetCurrentDesignByIdWithNullHandleOrUnknownIdReturnsNonzero)
{
    EXPECT_NE(le_set_current_design_by_id(nullptr, LeDesignId{0, 0}), 0);

    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    EXPECT_NE(le_set_current_design_by_id(handle, LeDesignId{UINT32_MAX, 0}), 0);
    EXPECT_NE(le_set_current_design_by_id(handle, LeDesignId{99, 0}), 0);
}

TEST_F(ApiFixture, SetCurrentDesignByIdSelectsTheSameDesignAsSetCurrentDesign)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);

    const LeDesignInfo design = le_library_design_at(handle, 0, 0);
    ASSERT_EQ(le_set_current_design_by_id(handle, design.id), 0);

    le_set_viewport_size(handle, 100, 100);
    le_zoom(handle, 100.0 / 10000.0 - 1.0, 0, 100); // pan (0,0), scale 0.01 - see other zoom-setup tests

    LePixelBuffer buffer = le_render_pixel_buffer(handle);
    ASSERT_NE(buffer.data, nullptr);
    // PIN A is on M1 (ROUTING), so its fill is a tiled diagonal-stripe
    // pattern - scan the pin's own region rather than trusting one pixel.
    EXPECT_TRUE(region_has_opaque_pixel(buffer, 21, 21, 79, 79)); // pin rect visible, same as le_set_current_design(handle, 0) would give
}

TEST_F(ApiFixture, LayerCountAndAtAreZeroOrInvalidForNullHandleOrNoViewLayerSetYet)
{
    EXPECT_EQ(le_layer_count(nullptr), 0);
    EXPECT_EQ(le_layer_count(handle), 0); // no LEF read yet - no ViewLayerSet built

    const LeLayerRow row = le_layer_at(handle, 0);
    EXPECT_EQ(row.name, nullptr);
}

TEST_F(ApiFixture, LayerAtOutOfRangeReturnsInvalidRow)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);

    const LeLayerRow row = le_layer_at(handle, 2);
    EXPECT_EQ(row.name, nullptr);
}

TEST_F(ApiFixture, LayerAtIncludesBoundaryAsAnOrdinaryRowAfterEveryPhysicalLayer)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);

    // testcell.lef declares one physical Layer (M1) - the API doesn't
    // special-case BOUNDARY, it's just another row, so the count is 2.
    ASSERT_EQ(le_layer_count(handle), 2);

    const LeLayerRow m1_row = le_layer_at(handle, 0);
    ASSERT_NE(m1_row.name, nullptr);
    EXPECT_STREQ(m1_row.name, "M1");

    // M1 is the first ROUTING layer declared - first slot of the bright
    // ROUTING/CUT palette, red (255, 0, 0) - see view_style.hpp.
    EXPECT_EQ(m1_row.color_r, 255);
    EXPECT_EQ(m1_row.color_g, 0);
    EXPECT_EQ(m1_row.color_b, 0);

    const LeLayerRow boundary_row = le_layer_at(handle, 1);
    ASSERT_NE(boundary_row.name, nullptr);
    EXPECT_STREQ(boundary_row.name, "BOUNDARY");
}

TEST_F(ApiFixture, PurposeCountAndAtAreZeroOrInvalidForNullHandleOrNoViewLayerSetYet)
{
    EXPECT_EQ(le_purpose_count(nullptr), 0);
    EXPECT_EQ(le_purpose_count(handle), 0); // no LEF read yet - no ViewLayerSet built

    EXPECT_EQ(le_purpose_at(handle, 0), -1);
    EXPECT_EQ(le_purpose_at(nullptr, 0), -1);
}

TEST_F(ApiFixture, PurposeAtOutOfRangeReturnsInvalid)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);

    EXPECT_EQ(le_purpose_at(handle, 3), -1);
    EXPECT_EQ(le_purpose_at(handle, -1), -1);
}

TEST_F(ApiFixture, PurposeAtListsTerminalObstructionThenBoundary)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);

    // The "columns" axis - row-independent, not scoped to M1 or any other
    // specific layer (see ViewLayerSet::purposes()'s own comment).
    ASSERT_EQ(le_purpose_count(handle), 3);
    EXPECT_EQ(le_purpose_at(handle, 0), 0); // TERMINAL
    EXPECT_EQ(le_purpose_at(handle, 1), 1); // OBSTRUCTION
    EXPECT_EQ(le_purpose_at(handle, 2), 2); // BOUNDARY
}

TEST_F(ApiFixture, LayerNameVisibilityDefaultsTrueAndRoundTrips)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);

    EXPECT_NE(le_is_layer_name_visible(handle, "M1"), 0);
    // Unknown-to-null-handle/name default matches Scene's own default.
    EXPECT_NE(le_is_layer_name_visible(nullptr, "M1"), 0);
    EXPECT_NE(le_is_layer_name_visible(handle, nullptr), 0);

    le_set_layer_name_visible(handle, "M1", 0);
    EXPECT_EQ(le_is_layer_name_visible(handle, "M1"), 0);
    // A different, never-toggled layer name is unaffected.
    EXPECT_NE(le_is_layer_name_visible(handle, "BOUNDARY"), 0);

    le_set_layer_name_visible(handle, "M1", 1);
    EXPECT_NE(le_is_layer_name_visible(handle, "M1"), 0);
}

TEST_F(ApiFixture, SetLayerNameVisibleWithNullHandleOrNullNameDoesNotCrash)
{
    le_set_layer_name_visible(nullptr, "M1", 0);
    le_set_layer_name_visible(handle, nullptr, 0);
}

TEST_F(ApiFixture, PurposeVisibilityDefaultsTrueAndRoundTrips)
{
    EXPECT_NE(le_is_purpose_visible(handle, 1), 0); // OBSTRUCTION
    EXPECT_NE(le_is_purpose_visible(nullptr, 1), 0);

    le_set_purpose_visible(handle, 1, 0);
    EXPECT_EQ(le_is_purpose_visible(handle, 1), 0);
    // A different, never-toggled purpose is unaffected.
    EXPECT_NE(le_is_purpose_visible(handle, 0), 0); // TERMINAL

    le_set_purpose_visible(handle, 1, 1);
    EXPECT_NE(le_is_purpose_visible(handle, 1), 0);
}

TEST_F(ApiFixture, SetPurposeVisibleWithNullHandleDoesNotCrash)
{
    le_set_purpose_visible(nullptr, 1, 0);
}

TEST_F(ApiFixture, LayerNameSelectabilityDefaultsTrueAndRoundTrips)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);

    EXPECT_NE(le_is_layer_name_selectable(handle, "M1"), 0);
    EXPECT_NE(le_is_layer_name_selectable(nullptr, "M1"), 0);
    EXPECT_NE(le_is_layer_name_selectable(handle, nullptr), 0);

    le_set_layer_name_selectable(handle, "M1", 0);
    EXPECT_EQ(le_is_layer_name_selectable(handle, "M1"), 0);

    le_set_layer_name_selectable(handle, "M1", 1);
    EXPECT_NE(le_is_layer_name_selectable(handle, "M1"), 0);
}

TEST_F(ApiFixture, SetLayerNameSelectableWithNullHandleOrNullNameDoesNotCrash)
{
    le_set_layer_name_selectable(nullptr, "M1", 0);
    le_set_layer_name_selectable(handle, nullptr, 0);
}

TEST_F(ApiFixture, PurposeSelectabilityDefaultsTrueAndRoundTrips)
{
    EXPECT_NE(le_is_purpose_selectable(handle, 1), 0); // OBSTRUCTION
    EXPECT_NE(le_is_purpose_selectable(nullptr, 1), 0);

    le_set_purpose_selectable(handle, 1, 0);
    EXPECT_EQ(le_is_purpose_selectable(handle, 1), 0);

    le_set_purpose_selectable(handle, 1, 1);
    EXPECT_NE(le_is_purpose_selectable(handle, 1), 0);
}

TEST_F(ApiFixture, SetPurposeSelectableWithNullHandleDoesNotCrash)
{
    le_set_purpose_selectable(nullptr, 1, 0);
}

TEST_F(ApiFixture, HidingALayerByNameRemovesItFromTheRenderedBuffer)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design(handle, 0), 0);

    le_set_viewport_size(handle, 100, 100);
    le_zoom(handle, 100.0 / 10000.0 - 1.0, 0, 100); // pan (0,0), scale 0.01 - see other zoom-setup tests

    LePixelBuffer before = le_render_pixel_buffer(handle);
    ASSERT_NE(before.data, nullptr);
    ASSERT_TRUE(region_has_opaque_pixel(before, 21, 21, 79, 79)); // baseline: PIN A visible

    le_set_layer_name_visible(handle, "M1", 0); // hides both M1/TERMINAL and M1/OBSTRUCTION

    LePixelBuffer after = le_render_pixel_buffer(handle);
    ASSERT_NE(after.data, nullptr);
    EXPECT_FALSE(region_has_opaque_pixel(after, 21, 21, 79, 79)); // M1 is hidden now
}

TEST_F(ApiFixture, HidingATerminalPurposeRemovesItFromTheRenderedBuffer)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design(handle, 0), 0);

    le_set_viewport_size(handle, 100, 100);
    le_zoom(handle, 100.0 / 10000.0 - 1.0, 0, 100); // pan (0,0), scale 0.01 - see other zoom-setup tests

    LePixelBuffer before = le_render_pixel_buffer(handle);
    ASSERT_NE(before.data, nullptr);
    ASSERT_TRUE(region_has_opaque_pixel(before, 21, 21, 79, 79)); // baseline: PIN A (TERMINAL) visible

    le_set_purpose_visible(handle, 0, 0); // TERMINAL off, across every layer

    LePixelBuffer after = le_render_pixel_buffer(handle);
    ASSERT_NE(after.data, nullptr);
    EXPECT_FALSE(region_has_opaque_pixel(after, 21, 21, 79, 79)); // PIN A's TERMINAL column is hidden now
}

TEST_F(ApiFixture, GridSpacingDefaultsMatchScene)
{
    EXPECT_EQ(le_minor_grid_spacing(handle), 5);
    EXPECT_EQ(le_major_grid_spacing(handle), 50);
}

TEST_F(ApiFixture, GridSpacingRoundTrips)
{
    le_set_minor_grid_spacing(handle, 10);
    le_set_major_grid_spacing(handle, 100);

    EXPECT_EQ(le_minor_grid_spacing(handle), 10);
    EXPECT_EQ(le_major_grid_spacing(handle), 100);
}

TEST_F(ApiFixture, GridSpacingIgnoresNonPositiveValues)
{
    le_set_minor_grid_spacing(handle, 10);
    le_set_minor_grid_spacing(handle, 0);
    le_set_minor_grid_spacing(handle, -5);
    EXPECT_EQ(le_minor_grid_spacing(handle), 10); // unchanged

    le_set_major_grid_spacing(handle, 100);
    le_set_major_grid_spacing(handle, 0);
    le_set_major_grid_spacing(handle, -5);
    EXPECT_EQ(le_major_grid_spacing(handle), 100); // unchanged
}

TEST_F(ApiFixture, GridSpacingWithNullHandleReturnsZeroAndDoesNotCrash)
{
    EXPECT_EQ(le_minor_grid_spacing(nullptr), 0);
    EXPECT_EQ(le_major_grid_spacing(nullptr), 0);
    le_set_minor_grid_spacing(nullptr, 10);
    le_set_major_grid_spacing(nullptr, 100);
}

TEST_F(ApiFixture, MousePositionWithNullHandleDoesNotCrash)
{
    le_set_mouse_position(nullptr, 10, 10);
    le_clear_mouse_position(nullptr);
}

TEST_F(ApiFixture, SnappedMousePositionWithNullHandleReturnsNoPosition)
{
    const LeSnappedMousePosition result = le_snapped_mouse_position(nullptr);
    EXPECT_EQ(result.has_position, 0);
    EXPECT_EQ(result.x_um, 0.0);
    EXPECT_EQ(result.y_um, 0.0);
}

TEST_F(ApiFixture, SnappedMousePositionHasNoPositionUntilMouseIsSet)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    le_set_viewport_size(handle, 100, 100);
    const LeSnappedMousePosition result = le_snapped_mouse_position(handle);
    EXPECT_EQ(result.has_position, 0);
}

TEST_F(ApiFixture, SnappedMousePositionHasNoPositionWithoutATechnologyLoaded)
{
    // No le_read_lef call at all - degrades gracefully instead of dividing
    // by a Technology::database_units_microns that doesn't exist yet.
    le_set_viewport_size(handle, 100, 100);
    le_set_mouse_position(handle, 50, 50);

    const LeSnappedMousePosition result = le_snapped_mouse_position(handle);
    EXPECT_EQ(result.has_position, 0);
}

TEST_F(ApiFixture, SnappedMousePositionReturnsGridSnappedMicronCoordinates)
{
    // testcell.lef declares DATABASE MICRONS 1000 - 1000 dbu per micron.
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);

    le_set_viewport_size(handle, 100, 100);
    le_set_minor_grid_spacing(handle, 10);

    // dbu (23, 100 - 27) = (23, 73); nearest multiples of 10 are 20 and 70
    // -> 0.02um and 0.07um at 1000 dbu/um.
    le_set_mouse_position(handle, 23, 27);

    const LeSnappedMousePosition result = le_snapped_mouse_position(handle);
    ASSERT_EQ(result.has_position, 1);
    EXPECT_DOUBLE_EQ(result.x_um, 0.02);
    EXPECT_DOUBLE_EQ(result.y_um, 0.07);
}

TEST_F(ApiFixture, SnappedMousePositionHasNoPositionAfterClear)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    le_set_viewport_size(handle, 100, 100);
    le_set_mouse_position(handle, 50, 50);
    ASSERT_EQ(le_snapped_mouse_position(handle).has_position, 1);

    le_clear_mouse_position(handle);
    EXPECT_EQ(le_snapped_mouse_position(handle).has_position, 0);
}

TEST_F(ApiFixture, RenderPixelBufferShowsRedCursorBoxAtSnappedMousePosition)
{
    le_set_viewport_size(handle, 100, 100);
    le_zoom(handle, 1.0, 0, 100); // scale 1.0 -> 2.0, anchored at dbu (0,0) so pan stays (0,0) - default minor spacing (5dbu*2=10px) clears the density floor

    // Screen pixel (50,50), top-left origin/y-down (same convention as
    // le_zoom's x/y) -> dbu (25,25), already a multiple of the default
    // 5dbu minor grid, so it snaps to itself and the box lands centered
    // on screen pixel (50,50) after compose_with_overlays's Y-flip.
    le_set_mouse_position(handle, 50, 50);

    LePixelBuffer buffer = le_render_pixel_buffer(handle);
    ASSERT_NE(buffer.data, nullptr);
    EXPECT_TRUE(region_has_red_cursor_pixel(buffer, 40, 40, 60, 60));
}

TEST_F(ApiFixture, RenderPixelBufferHasNoCursorBoxWhenNoMousePositionSet)
{
    le_set_viewport_size(handle, 100, 100);
    le_zoom(handle, 1.0, 0, 100);

    LePixelBuffer buffer = le_render_pixel_buffer(handle);
    ASSERT_NE(buffer.data, nullptr);
    EXPECT_FALSE(region_has_red_cursor_pixel(buffer, 40, 40, 60, 60));
}

TEST_F(ApiFixture, ClearMousePositionRemovesTheCursorBoxFromSubsequentRenders)
{
    le_set_viewport_size(handle, 100, 100);
    le_zoom(handle, 1.0, 0, 100);
    le_set_mouse_position(handle, 50, 50);

    LePixelBuffer before = le_render_pixel_buffer(handle);
    ASSERT_NE(before.data, nullptr);
    ASSERT_TRUE(region_has_red_cursor_pixel(before, 40, 40, 60, 60));

    le_clear_mouse_position(handle);
    LePixelBuffer after = le_render_pixel_buffer(handle);
    ASSERT_NE(after.data, nullptr);
    EXPECT_FALSE(region_has_red_cursor_pixel(after, 40, 40, 60, 60));
}

TEST_F(ApiFixture, ZoomWithNullHandleDoesNotCrash)
{
    le_zoom(nullptr, 0.5, 10, 10);
}

TEST_F(ApiFixture, ZoomWithDegenerateFactorLeavesScaleAndPanUnchanged)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design(handle, 0), 0);

    le_set_viewport_size(handle, 100, 100);
    le_zoom(handle, 100.0 / 10000.0 - 1.0, 0, 100); // -> scale 0.01, pan (0, 0)

    // A factor <= -1.0 would make new_scale non-positive - must be
    // ignored entirely (same guard as Scene::set_scale), not clamp to
    // some fallback value.
    le_zoom(handle, -1.0, 50, 50);

    LePixelBuffer buffer = le_render_pixel_buffer(handle);
    ASSERT_NE(buffer.data, nullptr);
    EXPECT_TRUE(region_has_opaque_pixel(buffer, 21, 21, 79, 79)); // pin rect still visible exactly where it was
}

TEST_F(ApiFixture, ZoomKeepsTheAnchorPixelFixedOnScreen)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design(handle, 0), 0);

    le_set_viewport_size(handle, 100, 100);
    le_zoom(handle, 100.0 / 10000.0 - 1.0, 0, 100); // -> scale 0.01, pan (0, 0)

    LePixelBuffer before = le_render_pixel_buffer(handle);
    ASSERT_NE(before.data, nullptr);
    // A small window around (50, 50) rather than that one exact pixel -
    // PIN A's ROUTING-layer fill is a tiled stripe pattern, so a single
    // point could land in a transparent gap by coincidence of phase.
    ASSERT_TRUE(region_has_opaque_pixel(before, 43, 43, 57, 57)); // baseline: pin rect visible around (50, 50)

    // Zoom in 2x anchored at the same pixel being sampled - the dbu point
    // under (50, 50) must still be under (50, 50) afterward, so it should
    // still show the same pin rect content there.
    le_zoom(handle, 1.0, 50, 50);

    LePixelBuffer after = le_render_pixel_buffer(handle);
    ASSERT_NE(after.data, nullptr);
    EXPECT_TRUE(region_has_opaque_pixel(after, 43, 43, 57, 57));
}

TEST_F(ApiFixture, PanWithNullHandleDoesNotCrash)
{
    le_pan(nullptr, 1.0, 1.0);
}

TEST_F(ApiFixture, PanShiftsContentOutOfView)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design(handle, 0), 0);

    le_set_viewport_size(handle, 100, 100);
    le_zoom(handle, 100.0 / 10000.0 - 1.0, 0, 100); // -> scale 0.01, pan (0, 0)

    LePixelBuffer before = le_render_pixel_buffer(handle);
    ASSERT_NE(before.data, nullptr);
    ASSERT_TRUE(region_has_opaque_pixel(before, 21, 21, 79, 79)); // baseline: pin rect visible

    // Pan by 2 full viewport widths in dbu x - the whole 10000-dbu-wide
    // macro (well under one viewport width at this scale) ends up entirely
    // out of view.
    le_pan(handle, 2.0, 0.0);

    LePixelBuffer after = le_render_pixel_buffer(handle);
    ASSERT_NE(after.data, nullptr);
    EXPECT_FALSE(region_has_opaque_pixel(after, 21, 21, 79, 79));
}

TEST_F(ApiFixture, KeyDownZoomZoomsInAnchoredAtTheCurrentMousePosition)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design(handle, 0), 0);

    le_set_viewport_size(handle, 100, 100);
    le_zoom(handle, 100.0 / 10000.0 - 1.0, 0, 100); // -> scale 0.01, pan (0, 0); PIN A at device (20,20)-(80,80)

    LePixelBuffer before = le_render_pixel_buffer(handle);
    ASSERT_NE(before.data, nullptr);
    ASSERT_FALSE(region_has_opaque_pixel(before, 13, 48, 17, 52)); // just outside the pin's left edge

    le_set_mouse_position(handle, 50, 50); // center of the macro/pin
    le_key_down(handle, LE_KEY_ZOOM);

    // Zooming in (factor +0.3, scale 0.01 -> 0.013) grows the pin's
    // half-width from 30px to 39px around the same (50,50) anchor - the
    // sample window that was just outside it before is now inside.
    LePixelBuffer after = le_render_pixel_buffer(handle);
    ASSERT_NE(after.data, nullptr);
    EXPECT_TRUE(region_has_opaque_pixel(after, 13, 48, 17, 52));
}

TEST_F(ApiFixture, KeyDownZoomWithShiftHeldZoomsOutInstead)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design(handle, 0), 0);

    le_set_viewport_size(handle, 100, 100);
    le_zoom(handle, 100.0 / 10000.0 - 1.0, 0, 100); // -> scale 0.01, pan (0, 0); PIN A at device (20,20)-(80,80)

    LePixelBuffer before = le_render_pixel_buffer(handle);
    ASSERT_NE(before.data, nullptr);
    ASSERT_TRUE(region_has_opaque_pixel(before, 23, 48, 27, 52)); // inside the pin, near its left edge

    le_set_mouse_position(handle, 50, 50);
    le_key_down(handle, LE_KEY_SHIFT);
    le_key_down(handle, LE_KEY_ZOOM);

    // Zooming out (factor -0.3, scale 0.01 -> 0.007) shrinks the pin's
    // half-width from 30px to 21px around the same anchor - the sample
    // window that was inside it before is now outside. Sampled at x:18-22
    // rather than reusing the "before" window (23-27): the mouse position
    // set above also puts the pin under hover, and its outline's stroke
    // antialiasing extends a pixel or two past the pin's own new edge
    // (~x=29) toward the old window, so 23-27 isn't a clean miss anymore.
    LePixelBuffer after = le_render_pixel_buffer(handle);
    ASSERT_NE(after.data, nullptr);
    EXPECT_FALSE(region_has_opaque_pixel(after, 18, 48, 22, 52));
}

TEST_F(ApiFixture, KeyDownZoomWithoutAMousePositionSetDoesNotCrash)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design(handle, 0), 0);
    le_set_viewport_size(handle, 100, 100);

    le_key_down(handle, LE_KEY_ZOOM); // no le_set_mouse_position call first - anchors at the default (0,0)

    LePixelBuffer buffer = le_render_pixel_buffer(handle);
    EXPECT_NE(buffer.data, nullptr);
}

TEST_F(ApiFixture, KeyDownFitFitsTheViewportToContent)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design(handle, 0), 0);

    le_set_viewport_size(handle, 200, 200);
    le_key_down(handle, LE_KEY_FIT);

    // Mirrors FitSceneFillsTheViewportWithThePinVisible, triggered via
    // le_key_down instead of le_fit_scene directly.
    LePixelBuffer buffer = le_render_pixel_buffer(handle);
    ASSERT_NE(buffer.data, nullptr);
    EXPECT_TRUE(region_has_opaque_pixel(buffer, 80, 80, 120, 120));
}

TEST_F(ApiFixture, KeyDownPanLeftShiftsContentOutOfView)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design(handle, 0), 0);

    le_set_viewport_size(handle, 100, 100);
    le_zoom(handle, 100.0 / 10000.0 - 1.0, 0, 100); // -> scale 0.01, pan (0, 0)

    LePixelBuffer before = le_render_pixel_buffer(handle);
    ASSERT_NE(before.data, nullptr);
    ASSERT_TRUE(region_has_opaque_pixel(before, 21, 21, 79, 79));

    // kKeyPanFactor (0.25) per press - 8 presses covers 2 full viewport
    // widths, the same margin PanShiftsContentOutOfView uses directly.
    for (int i = 0; i < 8; ++i)
        le_key_down(handle, LE_KEY_PAN_LEFT);

    LePixelBuffer after = le_render_pixel_buffer(handle);
    ASSERT_NE(after.data, nullptr);
    EXPECT_FALSE(region_has_opaque_pixel(after, 21, 21, 79, 79));
}

TEST_F(ApiFixture, KeyDownPanDirectionsAreEachOthersOpposite)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design(handle, 0), 0);

    le_set_viewport_size(handle, 100, 100);
    le_zoom(handle, 100.0 / 10000.0 - 1.0, 0, 100);

    LePixelBuffer before = le_render_pixel_buffer(handle);
    ASSERT_NE(before.data, nullptr);
    ASSERT_TRUE(region_has_opaque_pixel(before, 21, 21, 79, 79));

    le_key_down(handle, LE_KEY_PAN_LEFT);
    le_key_down(handle, LE_KEY_PAN_RIGHT); // undoes the left pan
    le_key_down(handle, LE_KEY_PAN_UP);
    le_key_down(handle, LE_KEY_PAN_DOWN); // undoes the up pan

    LePixelBuffer after = le_render_pixel_buffer(handle);
    ASSERT_NE(after.data, nullptr);
    EXPECT_TRUE(region_has_opaque_pixel(after, 21, 21, 79, 79));
}

TEST_F(ApiFixture, RenderPixelBufferDrawsThePinRectAtItsExpectedLocation)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design(handle, 0), 0);

    // MACRO SIZE is 10x10 microns, PIN A's RECT is (2,2)-(8,8) microns.
    // DATABASE MICRONS 1000 -> 1 micron = 1000 dbu. Scale chosen so the
    // whole 10x10 micron (10000x10000 dbu) macro fills a 100x100px buffer.
    // Scene starts at scale 1.0 / pan (0, 0) - le_zoom to that scale
    // anchored at image pixel (0, 100) (dbu (0, 0) at the starting
    // pan/scale) keeps pan pinned at (0, 0) exactly.
    le_set_viewport_size(handle, 100, 100);
    le_zoom(handle, 100.0 / 10000.0 - 1.0, 0, 100);

    LePixelBuffer buffer = le_render_pixel_buffer(handle);
    ASSERT_NE(buffer.data, nullptr);

    // Pin rect (2,2)-(8,8) microns -> pixel (20,20)-(80,80) before
    // rasterize()'s Y-flip -> device (20, 100-80)-(80, 100-20) after it.
    // PIN A is on M1 (ROUTING), so its fill is a tiled diagonal-stripe
    // pattern, not a flat fill - scan a region inside it (inset from the
    // outline hairline) rather than trusting one exact pixel.
    EXPECT_TRUE(region_has_opaque_pixel(buffer, 21, 21, 79, 79)); // fill pattern - something was actually drawn here

    // Well outside the pin rect, but still inside the macro boundary
    // outline - the boundary is stroke-only (no fill), so this should be
    // fully transparent.
    const uint8_t *outside = buffer.data + static_cast<size_t>(5) * static_cast<size_t>(buffer.row_bytes) + static_cast<size_t>(5) * 4;
    EXPECT_EQ(outside[3], 0);
}

TEST_F(ApiFixture, MouseMoveOverASelectableShapeShowsAYellowHoverOutline)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design(handle, 0), 0);

    // Same pan/scale as RenderPixelBufferDrawsThePinRectAtItsExpectedLocation -
    // PIN A's rect ends up at device pixel (20,20)-(80,80).
    le_set_viewport_size(handle, 100, 100);
    le_zoom(handle, 100.0 / 10000.0 - 1.0, 0, 100);

    le_set_mouse_position(handle, 50, 50); // well inside PIN A's rect

    LePixelBuffer buffer = le_render_pixel_buffer(handle);
    ASSERT_NE(buffer.data, nullptr);
    EXPECT_TRUE(region_has_yellow_hover_pixel(buffer, 18, 48, 22, 52)); // left edge of the hovered pin's outline
}

TEST_F(ApiFixture, MouseMoveAwayFromAnyShapeClearsTheHoverOutline)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design(handle, 0), 0);

    le_set_viewport_size(handle, 100, 100);
    le_zoom(handle, 100.0 / 10000.0 - 1.0, 0, 100);

    le_set_mouse_position(handle, 50, 50); // over the pin
    le_set_mouse_position(handle, 95, 95); // now off every shape

    LePixelBuffer buffer = le_render_pixel_buffer(handle);
    ASSERT_NE(buffer.data, nullptr);
    EXPECT_FALSE(region_has_yellow_hover_pixel(buffer, 18, 48, 22, 52));
}

TEST_F(ApiFixture, ClearMousePositionAlsoClearsTheHoverOutline)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design(handle, 0), 0);

    le_set_viewport_size(handle, 100, 100);
    le_zoom(handle, 100.0 / 10000.0 - 1.0, 0, 100);

    le_set_mouse_position(handle, 50, 50);
    le_clear_mouse_position(handle);

    LePixelBuffer buffer = le_render_pixel_buffer(handle);
    ASSERT_NE(buffer.data, nullptr);
    EXPECT_FALSE(region_has_yellow_hover_pixel(buffer, 18, 48, 22, 52));
}

TEST_F(ApiFixture, MouseMoveOverAnUnselectableLayerNeverShowsAHoverOutline)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design(handle, 0), 0);

    le_set_viewport_size(handle, 100, 100);
    le_zoom(handle, 100.0 / 10000.0 - 1.0, 0, 100);
    le_set_layer_name_selectable(handle, "M1", 0);

    le_set_mouse_position(handle, 50, 50); // over the pin, but M1 is unselectable

    LePixelBuffer buffer = le_render_pixel_buffer(handle);
    ASSERT_NE(buffer.data, nullptr);
    EXPECT_FALSE(region_has_yellow_hover_pixel(buffer, 18, 48, 22, 52));
}

TEST_F(ApiFixture, KeyDownThenIsKeyHeldReturnsTrue)
{
    EXPECT_EQ(le_is_key_held(handle, LE_KEY_SHIFT), 0);

    le_key_down(handle, LE_KEY_SHIFT);
    EXPECT_NE(le_is_key_held(handle, LE_KEY_SHIFT), 0);
}

TEST_F(ApiFixture, KeyUpClearsAHeldKey)
{
    le_key_down(handle, LE_KEY_SHIFT);
    ASSERT_NE(le_is_key_held(handle, LE_KEY_SHIFT), 0);

    le_key_up(handle, LE_KEY_SHIFT);
    EXPECT_EQ(le_is_key_held(handle, LE_KEY_SHIFT), 0);
}

TEST_F(ApiFixture, KeyUpWithoutAPrecedingKeyDownIsANoOp)
{
    le_key_up(handle, LE_KEY_SHIFT);
    EXPECT_EQ(le_is_key_held(handle, LE_KEY_SHIFT), 0);
}

TEST_F(ApiFixture, KeyStateFunctionsWithNullHandleDoNotCrash)
{
    le_key_down(nullptr, LE_KEY_SHIFT);
    le_key_up(nullptr, LE_KEY_SHIFT);
    EXPECT_EQ(le_is_key_held(nullptr, LE_KEY_SHIFT), 0);
    le_clear_all_keys(nullptr);
}

TEST_F(ApiFixture, ClearAllKeysReleasesAHeldShift)
{
    le_key_down(handle, LE_KEY_SHIFT);
    ASSERT_NE(le_is_key_held(handle, LE_KEY_SHIFT), 0);

    le_clear_all_keys(handle);
    EXPECT_EQ(le_is_key_held(handle, LE_KEY_SHIFT), 0);
}


namespace
{
    // Shared by every click/drag-select test below: reads two_shapes.lef
    // (MACRO TWOSHAPES, PIN A at (1,1)-(4,4) micron, PIN B at
    // (16,16)-(19,19) micron - see the fixture file), and zooms a 200x200
    // viewport to scale 0.01 (10px/micron) with pan pinned at (0,0), same
    // "anchor at the bottom-left image corner" trick as other tests in
    // this file. At that scale/pan (device/image pixel space, top-left
    // origin, y down):
    //   - PIN A occupies device (10,160)-(40,190) - center ~(25,175).
    //   - PIN B occupies device (160,10)-(190,40) - center ~(175,25).
    //   - (100,100) device is empty space (10,10 micron - inside the
    //     macro, outside both pins).
    void load_two_shapes_at_known_scale(LeHandle *handle)
    {
        ASSERT_EQ(le_read_lef(handle, fixture_path("two_shapes.lef").c_str()), 0);
        ASSERT_EQ(le_set_current_design(handle, 0), 0);

        le_set_viewport_size(handle, 200, 200);
        le_zoom(handle, 0.01 - 1.0, 0, 200);
    }
}

TEST_F(ApiFixture, MouseDownThenUpAsAClickSelectsTheHitShape)
{
    load_two_shapes_at_known_scale(handle);

    le_mouse_down(handle, 25, 175);
    le_mouse_up(handle, 25, 175);

    EXPECT_EQ(le_selection_count(handle), 1);
}

TEST_F(ApiFixture, SmallMovementBetweenDownAndUpIsStillTreatedAsAClick)
{
    load_two_shapes_at_known_scale(handle);

    le_mouse_down(handle, 25, 175);
    le_mouse_up(handle, 26, 176); // well under the click/drag threshold

    EXPECT_EQ(le_selection_count(handle), 1);
}

TEST_F(ApiFixture, ClickOnEmptySpaceClearsTheSelection)
{
    load_two_shapes_at_known_scale(handle);

    le_mouse_down(handle, 25, 175);
    le_mouse_up(handle, 25, 175);
    ASSERT_EQ(le_selection_count(handle), 1);

    le_mouse_down(handle, 100, 100); // empty space
    le_mouse_up(handle, 100, 100);
    EXPECT_EQ(le_selection_count(handle), 0);
}

TEST_F(ApiFixture, PlainClickReplacesThePreviousSelectionRatherThanAddingToIt)
{
    load_two_shapes_at_known_scale(handle);

    le_mouse_down(handle, 25, 175); // PIN A
    le_mouse_up(handle, 25, 175);
    ASSERT_EQ(le_selection_count(handle), 1);

    le_mouse_down(handle, 175, 25); // PIN B, no shift
    le_mouse_up(handle, 175, 25);
    EXPECT_EQ(le_selection_count(handle), 1); // still just one - B replaced A, not added
}

TEST_F(ApiFixture, ShiftClickAddsToTheSelection)
{
    load_two_shapes_at_known_scale(handle);

    le_mouse_down(handle, 25, 175); // PIN A
    le_mouse_up(handle, 25, 175);
    ASSERT_EQ(le_selection_count(handle), 1);

    le_mouse_down(handle, 175, 25); // PIN B, with shift
    le_key_down(handle, LE_KEY_SHIFT);
    le_mouse_up(handle, 175, 25);
    le_key_up(handle, LE_KEY_SHIFT);
    EXPECT_EQ(le_selection_count(handle), 2);
}

TEST_F(ApiFixture, ShiftClickOnEmptySpaceIsANoOp)
{
    load_two_shapes_at_known_scale(handle);

    le_mouse_down(handle, 25, 175); // PIN A
    le_mouse_up(handle, 25, 175);
    ASSERT_EQ(le_selection_count(handle), 1);

    le_mouse_down(handle, 100, 100); // empty space, with shift
    le_key_down(handle, LE_KEY_SHIFT);
    le_mouse_up(handle, 100, 100);
    le_key_up(handle, LE_KEY_SHIFT);
    EXPECT_EQ(le_selection_count(handle), 1); // unchanged
}

TEST_F(ApiFixture, ClickOnAnUnselectableLayerSelectsNothing)
{
    load_two_shapes_at_known_scale(handle);
    le_set_layer_name_selectable(handle, "M1", 0);

    le_mouse_down(handle, 25, 175);
    le_mouse_up(handle, 25, 175);
    EXPECT_EQ(le_selection_count(handle), 0);
}

TEST_F(ApiFixture, DragSelectEnclosesEverySelectableShapeInTheRectangle)
{
    load_two_shapes_at_known_scale(handle);

    // Full-viewport drag - dbu (0,0) to (20,20) micron, the whole macro,
    // enclosing both pins.
    le_mouse_down(handle, 0, 200);
    le_mouse_up(handle, 200, 0);

    EXPECT_EQ(le_selection_count(handle), 2);
}

TEST_F(ApiFixture, DragSelectExcludesAShapeThatIsOnlyPartiallyEnclosed)
{
    load_two_shapes_at_known_scale(handle);

    // dbu (0,0)-(2.5,5) micron - clips through the left portion of PIN A
    // (1,1)-(4,4) micron without fully enclosing it, and nowhere near
    // PIN B.
    le_mouse_down(handle, 0, 150);
    le_mouse_up(handle, 25, 200);

    EXPECT_EQ(le_selection_count(handle), 0);
}

TEST_F(ApiFixture, ShiftDragAddsToTheExistingSelectionRatherThanReplacingIt)
{
    load_two_shapes_at_known_scale(handle);

    le_mouse_down(handle, 25, 175); // PIN A, plain click
    le_mouse_up(handle, 25, 175);
    ASSERT_EQ(le_selection_count(handle), 1);

    // Small rect tightly around just PIN B, with shift.
    le_mouse_down(handle, 155, 5);
    le_key_down(handle, LE_KEY_SHIFT);
    le_mouse_up(handle, 195, 45);
    le_key_up(handle, LE_KEY_SHIFT);
    EXPECT_EQ(le_selection_count(handle), 2);
}

TEST_F(ApiFixture, DragSelectOnAnUnselectableLayerSelectsNothing)
{
    load_two_shapes_at_known_scale(handle);
    le_set_layer_name_selectable(handle, "M1", 0);

    le_mouse_down(handle, 0, 200);
    le_mouse_up(handle, 200, 0);
    EXPECT_EQ(le_selection_count(handle), 0);
}

TEST_F(ApiFixture, MouseDownAloneDoesNotChangeTheSelection)
{
    load_two_shapes_at_known_scale(handle);

    le_mouse_down(handle, 25, 175);
    EXPECT_EQ(le_selection_count(handle), 0); // nothing committed until mouse-up
}

TEST_F(ApiFixture, MouseUpWithoutAPrecedingMouseDownIsANoOp)
{
    load_two_shapes_at_known_scale(handle);

    le_mouse_up(handle, 25, 175);
    EXPECT_EQ(le_selection_count(handle), 0);
}

TEST_F(ApiFixture, MouseDownAndUpWithNullHandleDoNotCrash)
{
    le_mouse_down(nullptr, 25, 175);
    le_mouse_up(nullptr, 25, 175);
    EXPECT_EQ(le_selection_count(nullptr), 0);
}

TEST_F(ApiFixture, ClickSelectingAShapeShowsAWhiteOutlineInTheRenderedBuffer)
{
    load_two_shapes_at_known_scale(handle);

    // Regression: render once *before* selecting anything, so
    // rasterize_frame/compose_with_overlays's caches are already warm at
    // {AbstractId, viewport_version, visibility_version} - none of which
    // change when a selection is made. Their cache keys used to stop
    // there, so this exact sequence (render, then select, then render
    // again) returned the pre-selection frame unchanged: build_picture
    // correctly produced a new SkPicture with the outline baked in, but
    // downstream, CachedStage::get only ever compares the key tuple, not
    // the SkPicture argument itself, so the stale frame from the first
    // render won. selection_version now closes that gap.
    LePixelBuffer before = le_render_pixel_buffer(handle);
    ASSERT_NE(before.data, nullptr);
    ASSERT_FALSE(region_has_white_selection_pixel(before, 8, 170, 12, 180));

    le_mouse_down(handle, 25, 175); // PIN A, device (10,160)-(40,190)
    le_mouse_up(handle, 25, 175);
    ASSERT_EQ(le_selection_count(handle), 1);

    LePixelBuffer after = le_render_pixel_buffer(handle);
    ASSERT_NE(after.data, nullptr);
    EXPECT_TRUE(region_has_white_selection_pixel(after, 8, 170, 12, 180)); // PIN A's left edge
}

TEST_F(ApiFixture, ClearingTheSelectionRemovesTheWhiteOutline)
{
    load_two_shapes_at_known_scale(handle);

    le_mouse_down(handle, 25, 175);
    le_mouse_up(handle, 25, 175);
    ASSERT_EQ(le_selection_count(handle), 1);

    // Render with the selection active first (and warm every downstream
    // cache at this selection_version), matching a real frontend that
    // redraws after every gesture - not just once at the very end.
    LePixelBuffer selected = le_render_pixel_buffer(handle);
    ASSERT_NE(selected.data, nullptr);
    ASSERT_TRUE(region_has_white_selection_pixel(selected, 8, 170, 12, 180));

    le_mouse_down(handle, 100, 100); // empty space - clears the selection
    le_mouse_up(handle, 100, 100);
    ASSERT_EQ(le_selection_count(handle), 0);

    LePixelBuffer cleared = le_render_pixel_buffer(handle);
    ASSERT_NE(cleared.data, nullptr);
    EXPECT_FALSE(region_has_white_selection_pixel(cleared, 8, 170, 12, 180));
}

TEST_F(ApiFixture, SwitchingToADifferentDesignClearsTheSelection)
{
    load_two_shapes_at_known_scale(handle); // design 0 = TWOSHAPES
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0); // design 1 = TESTCELL

    le_mouse_down(handle, 25, 175); // PIN A on TWOSHAPES
    le_mouse_up(handle, 25, 175);
    ASSERT_EQ(le_selection_count(handle), 1);

    ASSERT_EQ(le_set_current_design(handle, 1), 0); // switch to TESTCELL
    EXPECT_EQ(le_selection_count(handle), 0);
}

TEST_F(ApiFixture, ClearAllKeysMakesSubsequentClicksReplaceRatherThanAddAgain)
{
    // The exact reported bug: a stuck shift (e.g. from a missed key-up on
    // focus loss) made every later plain click behave like a shift-click.
    // le_clear_all_keys is the escape hatch - simulates a focus-loss
    // recovery.
    load_two_shapes_at_known_scale(handle);

    le_key_down(handle, LE_KEY_SHIFT); // shift held, then "stuck" (no key-up)

    le_mouse_down(handle, 25, 175); // PIN A, shift-click
    le_mouse_up(handle, 25, 175);
    ASSERT_EQ(le_selection_count(handle), 1);

    le_clear_all_keys(handle); // e.g. called from a focus-loss handler

    le_mouse_down(handle, 175, 25); // PIN B, plain click - should replace, not add
    le_mouse_up(handle, 175, 25);
    EXPECT_EQ(le_selection_count(handle), 1);
}

namespace
{
    // Shared by every selection-properties test below (UPDATES.md 7.2):
    // reads pin_and_obstruction.lef (MACRO PINOBS, PIN A at (1,1)-(4,4)
    // micron on M1, one OBS rect at (10,10)-(15,15) micron on M1 - see the
    // fixture file), and zooms a 200x200 viewport to scale 0.01 with pan
    // pinned at (0,0), same trick as load_two_shapes_at_known_scale. At
    // that scale/pan (device/image pixel space, top-left origin, y down):
    //   - PIN A occupies device (10,160)-(40,190) - center ~(25,175).
    //   - The OBS rect occupies device (100,50)-(150,100) - center ~(125,75).
    void load_pin_and_obstruction_at_known_scale(LeHandle *handle)
    {
        ASSERT_EQ(le_read_lef(handle, fixture_path("pin_and_obstruction.lef").c_str()), 0);
        ASSERT_EQ(le_set_current_design(handle, 0), 0);

        le_set_viewport_size(handle, 200, 200);
        le_zoom(handle, 0.01 - 1.0, 0, 200);
    }

    // Scans a selected object's full property table into a name -> row
    // map, so a test can look up "does this property exist and does it
    // have this value" without hard-coding property order.
    std::map<std::string, LeProperty> selected_object_properties(LeHandle *handle, int32_t selection_index)
    {
        std::map<std::string, LeProperty> properties;
        const int32_t count = le_selected_object_property_count(handle, selection_index);
        for (int32_t i = 0; i < count; ++i)
        {
            const LeProperty property = le_selected_object_property_at(handle, selection_index, i);
            properties.emplace(property.name, property);
        }
        return properties;
    }
}

TEST_F(ApiFixture, SelectedTerminalReportsItsKindNameDirectionAndPortCount)
{
    load_pin_and_obstruction_at_known_scale(handle);

    le_mouse_down(handle, 25, 175); // PIN A
    le_mouse_up(handle, 25, 175);
    ASSERT_EQ(le_selection_count(handle), 1);

    EXPECT_EQ(le_selected_object_kind(handle, 0), LE_SELECTION_KIND_TERMINAL);

    const std::map<std::string, LeProperty> properties = selected_object_properties(handle, 0);
    ASSERT_TRUE(properties.contains("name"));
    EXPECT_EQ(properties.at("name").type, LE_PROPERTY_TYPE_STRING);
    EXPECT_STREQ(properties.at("name").string_value, "A");

    ASSERT_TRUE(properties.contains("direction"));
    EXPECT_EQ(properties.at("direction").type, LE_PROPERTY_TYPE_STRING);
    EXPECT_STREQ(properties.at("direction").string_value, "INPUT");

    ASSERT_TRUE(properties.contains("port_count"));
    EXPECT_EQ(properties.at("port_count").type, LE_PROPERTY_TYPE_INT);
    EXPECT_EQ(properties.at("port_count").int_value, 1);
}

TEST_F(ApiFixture, SelectedTerminalReportsItsBoundingBoxConvertedToMicrons)
{
    load_pin_and_obstruction_at_known_scale(handle);

    le_mouse_down(handle, 25, 175); // PIN A, (1,1)-(4,4) micron
    le_mouse_up(handle, 25, 175);
    ASSERT_EQ(le_selection_count(handle), 1);

    const std::map<std::string, LeProperty> properties = selected_object_properties(handle, 0);
    ASSERT_TRUE(properties.contains("bbox_um"));
    EXPECT_EQ(properties.at("bbox_um").type, LE_PROPERTY_TYPE_STRING);
    EXPECT_STREQ(properties.at("bbox_um").string_value, "1 1 4 4");
}

TEST_F(ApiFixture, SelectedTerminalBoundingBoxTrimsTrailingZerosInWholeGroupsOfThree)
{
    // MACRO FRACPIN's PIN A is a RECT at (0.34,0.34)-(5.34,5.34) micron
    // (see fractional_pin.lef) - std::to_string would format each as 6
    // decimal digits ("0.340000"/"5.340000"); the trailing zeros should
    // trim down to the last *significant* group of three ("0.340"), not
    // strip further into the significant "340" group and not leave a
    // partial group like "0.34".
    ASSERT_EQ(le_read_lef(handle, fixture_path("fractional_pin.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design(handle, 0), 0);
    le_set_viewport_size(handle, 100, 100);
    le_zoom(handle, 0.01 - 1.0, 0, 100);

    le_mouse_down(handle, 28, 72); // inside the rect - dbu (2800,2800)
    le_mouse_up(handle, 28, 72);
    ASSERT_EQ(le_selection_count(handle), 1);

    const std::map<std::string, LeProperty> properties = selected_object_properties(handle, 0);
    ASSERT_TRUE(properties.contains("bbox_um"));
    EXPECT_STREQ(properties.at("bbox_um").string_value, "0.340 0.340 5.340 5.340");
}

TEST_F(ApiFixture, SelectedTerminalBoundingBoxKeepsFullPrecisionWhenNotAMultipleOfAThousand)
{
    // format_coordinate_um's "leave the value untouched" branch, otherwise
    // untested - every other bbox_um test uses DATABASE MICRONS 1000,
    // where um = dbu/1000 is always exactly 3-decimal-precise, so the
    // first trailing-zero-group check always succeeds there. This
    // fixture uses DATABASE MICRONS 2000 (full_precision_pin.lef) with a
    // RECT at (0.0005,0.0005)-(5.0005,5.0005) micron - a value that
    // genuinely needs all 6 decimal digits, so the trim loop's first
    // check ("500" isn't "000") must fail immediately and leave it alone.
    ASSERT_EQ(le_read_lef(handle, fixture_path("full_precision_pin.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design(handle, 0), 0);
    le_set_viewport_size(handle, 100, 100);
    le_zoom(handle, 0.01 - 1.0, 0, 100);

    le_mouse_down(handle, 50, 50); // inside the rect - dbu (5000,5000)
    le_mouse_up(handle, 50, 50);
    ASSERT_EQ(le_selection_count(handle), 1);

    const std::map<std::string, LeProperty> properties = selected_object_properties(handle, 0);
    ASSERT_TRUE(properties.contains("bbox_um"));
    EXPECT_STREQ(properties.at("bbox_um").string_value, "0.000500 0.000500 5.000500 5.000500");
}

TEST_F(ApiFixture, SelectedObstructionReportsItsKindShapesCountAndBoundingBox)
{
    load_pin_and_obstruction_at_known_scale(handle);

    le_mouse_down(handle, 125, 75); // the OBS rect, (10,10)-(15,15) micron
    le_mouse_up(handle, 125, 75);
    ASSERT_EQ(le_selection_count(handle), 1);

    EXPECT_EQ(le_selected_object_kind(handle, 0), LE_SELECTION_KIND_OBSTRUCTION);

    const std::map<std::string, LeProperty> properties = selected_object_properties(handle, 0);
    ASSERT_TRUE(properties.contains("shapes_count"));
    EXPECT_EQ(properties.at("shapes_count").type, LE_PROPERTY_TYPE_INT);
    EXPECT_EQ(properties.at("shapes_count").int_value, 1);

    ASSERT_TRUE(properties.contains("bbox_um"));
    EXPECT_STREQ(properties.at("bbox_um").string_value, "10 10 15 15");

    // Terminal-only property, should not leak onto an Obstruction's table.
    EXPECT_FALSE(properties.contains("name"));
}

TEST_F(ApiFixture, SelectedObjectPropertiesDistinguishTwoSelectedObjectsByIndex)
{
    load_pin_and_obstruction_at_known_scale(handle);

    le_mouse_down(handle, 25, 175); // PIN A
    le_mouse_up(handle, 25, 175);
    le_key_down(handle, LE_KEY_SHIFT);
    le_mouse_down(handle, 125, 75); // OBS, shift-click adds
    le_mouse_up(handle, 125, 75);
    ASSERT_EQ(le_selection_count(handle), 2);

    EXPECT_EQ(le_selected_object_kind(handle, 0), LE_SELECTION_KIND_TERMINAL);
    EXPECT_EQ(le_selected_object_kind(handle, 1), LE_SELECTION_KIND_OBSTRUCTION);

    const std::map<std::string, LeProperty> terminal_properties = selected_object_properties(handle, 0);
    EXPECT_STREQ(terminal_properties.at("name").string_value, "A");

    const std::map<std::string, LeProperty> obstruction_properties = selected_object_properties(handle, 1);
    EXPECT_EQ(obstruction_properties.at("shapes_count").int_value, 1);
}

TEST_F(ApiFixture, SelectedObjectKindAndPropertiesAreOutOfRangeSafe)
{
    load_pin_and_obstruction_at_known_scale(handle);

    EXPECT_EQ(le_selected_object_kind(handle, 0), -1); // nothing selected
    EXPECT_EQ(le_selected_object_property_count(handle, 0), 0);

    const LeProperty invalid = le_selected_object_property_at(handle, 0, 0);
    EXPECT_EQ(invalid.name, nullptr);

    le_mouse_down(handle, 25, 175); // select PIN A
    le_mouse_up(handle, 25, 175);
    ASSERT_EQ(le_selection_count(handle), 1);

    EXPECT_EQ(le_selected_object_kind(handle, 1), -1); // out of range
    EXPECT_EQ(le_selected_object_kind(handle, -1), -1);
    EXPECT_EQ(le_selected_object_property_count(handle, 1), 0);

    const LeProperty out_of_range_property = le_selected_object_property_at(handle, 0, 9999);
    EXPECT_EQ(out_of_range_property.name, nullptr);
}

TEST_F(ApiFixture, SelectedObjectKindAndPropertiesWithNullHandleDoNotCrash)
{
    EXPECT_EQ(le_selected_object_kind(nullptr, 0), -1);
    EXPECT_EQ(le_selected_object_property_count(nullptr, 0), 0);
    const LeProperty property = le_selected_object_property_at(nullptr, 0, 0);
    EXPECT_EQ(property.name, nullptr);
}

TEST_F(ApiFixture, ClickSelectingOnePortOfATwoPortTerminalReportsOnlyThatPortsBboxAndLayer)
{
    // PIN B has two ports on M1 - (16,1)-(19,4) and (16,16)-(19,19)
    // micron (see pin_and_obstruction.lef) - device (10px/micron,
    // pan (0,0)): port 1 occupies device (160,160)-(190,190) (center
    // ~(175,175)), port 2 occupies device (160,10)-(190,40) (center
    // ~(175,25)).
    load_pin_and_obstruction_at_known_scale(handle);

    le_mouse_down(handle, 175, 175); // PIN B's first port only
    le_mouse_up(handle, 175, 175);
    ASSERT_EQ(le_selection_count(handle), 1);
    ASSERT_EQ(le_selected_object_kind(handle, 0), LE_SELECTION_KIND_TERMINAL);

    const std::map<std::string, LeProperty> properties = selected_object_properties(handle, 0);
    ASSERT_TRUE(properties.contains("bbox_um"));
    EXPECT_STREQ(properties.at("bbox_um").string_value, "16 1 19 4"); // just the clicked port, not the union of both

    ASSERT_TRUE(properties.contains("layer_name"));
    EXPECT_STREQ(properties.at("layer_name").string_value, "M1");

    // Parent-level context is still present alongside the piece-scoped rows.
    ASSERT_TRUE(properties.contains("name"));
    EXPECT_STREQ(properties.at("name").string_value, "B");
    ASSERT_TRUE(properties.contains("port_count"));
    EXPECT_EQ(properties.at("port_count").int_value, 2);
}

TEST_F(ApiFixture, DragSelectingATwoPortTerminalStillReportsTheAggregateBboxWithNoLayerName)
{
    load_pin_and_obstruction_at_known_scale(handle);

    // Encloses both of PIN B's ports (device x:150-200 -> dbu 15-20
    // micron) without touching PIN A or the OBS rect.
    le_mouse_down(handle, 150, 0);
    le_mouse_up(handle, 200, 200);
    ASSERT_EQ(le_selection_count(handle), 1);
    ASSERT_EQ(le_selected_object_kind(handle, 0), LE_SELECTION_KIND_TERMINAL);

    const std::map<std::string, LeProperty> properties = selected_object_properties(handle, 0);
    ASSERT_TRUE(properties.contains("bbox_um"));
    EXPECT_STREQ(properties.at("bbox_um").string_value, "16 1 19 19"); // union of both ports

    EXPECT_FALSE(properties.contains("layer_name")); // no single piece - drag-select has none to report
}

TEST_F(ApiFixture, ShiftClickingTwoPiecesOfTheSameTerminalSelectsBothIndependently)
{
    // The actual reported bug: shift-clicking a second shape within the
    // same Terminal must add a second, independently-selected/reportable
    // entry, not replace or no-op against the first one.
    load_pin_and_obstruction_at_known_scale(handle);

    le_mouse_down(handle, 175, 175); // PIN B's first port
    le_mouse_up(handle, 175, 175);
    ASSERT_EQ(le_selection_count(handle), 1);

    le_key_down(handle, LE_KEY_SHIFT);
    le_mouse_down(handle, 175, 25); // PIN B's second port
    le_mouse_up(handle, 175, 25);
    le_key_up(handle, LE_KEY_SHIFT);
    ASSERT_EQ(le_selection_count(handle), 2); // both pieces, not one

    EXPECT_EQ(le_selected_object_kind(handle, 0), LE_SELECTION_KIND_TERMINAL);
    EXPECT_EQ(le_selected_object_kind(handle, 1), LE_SELECTION_KIND_TERMINAL);

    // Copy each bbox_um value out as its own std::string immediately -
    // LeProperty::string_value is only valid until the next
    // le_selected_object_property_at()/_count() call *for the same
    // selection_index* (see its doc comment); querying a different
    // index (as `second`'s query below does) invalidates `first`'s
    // pointers.
    const std::map<std::string, LeProperty> first_properties = selected_object_properties(handle, 0);
    ASSERT_TRUE(first_properties.contains("bbox_um"));
    const std::string first_bbox = first_properties.at("bbox_um").string_value;

    const std::map<std::string, LeProperty> second_properties = selected_object_properties(handle, 1);
    ASSERT_TRUE(second_properties.contains("bbox_um"));
    const std::string second_bbox = second_properties.at("bbox_um").string_value;

    EXPECT_NE(first_bbox, second_bbox); // genuinely distinct pieces
    EXPECT_EQ(first_bbox, "16 1 19 4");
    EXPECT_EQ(second_bbox, "16 16 19 19");
}

TEST_F(ApiFixture, ConcurrentRenderAndMousePositionCallsOnTheSameHandleDoNotCrash)
{
    // Regression: le_render_pixel_buffer (called by Flutter's own raster
    // thread, via FlutterTexture.copyPixelBuffer(), once per frame) and
    // le_set_mouse_position/le_zoom (called by the platform thread on
    // every pointer event) both run Pipeline::run() on the same handle -
    // a real crash (concurrent, unsynchronized std::map mutation inside
    // Pipeline::filter_by_layer_visibility) shipped from exactly this
    // pattern. Every LeHandle-touching function now locks the handle's
    // own mutex; this drives both call paths concurrently, repeatedly,
    // and must complete without crashing, deadlocking, or hanging.
    ASSERT_EQ(le_read_lef(handle, generate_concurrency_stress_lef(3000).c_str()), 0);
    ASSERT_EQ(le_set_current_design(handle, 0), 0);
    le_set_viewport_size(handle, 200, 200);

    constexpr int kIterations = 50;
    std::thread render_thread([&]
                               {
        for (int i = 0; i < kIterations; ++i)
        {
            // Alternates a no-op-ish zoom so viewport_version() keeps
            // changing, forcing Pipeline::run() to actually recompute
            // (not just return an already-cached result) on most calls -
            // the same "real work, not a cache hit" condition the
            // original crash needed to manifest.
            le_zoom(handle, (i % 2 == 0) ? 0.001 : -0.001, 100, 100);
            const LePixelBuffer buffer = le_render_pixel_buffer(handle);
            EXPECT_NE(buffer.data, nullptr);
        } });

    for (int i = 0; i < kIterations; ++i)
        le_set_mouse_position(handle, i % 200, (i * 7) % 200);

    render_thread.join();
}
