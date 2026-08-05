#include "../api.hpp"
#include <gtest/gtest.h>
#include <string>

namespace
{
    std::string fixture_path(const std::string &name)
    {
        return std::string(API_TEST_FIXTURES_DIR) + "/" + name;
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
    // sample the center pixel, which sits inside both the 10x10 micron
    // macro boundary and PIN A's (2,2)-(8,8) micron RECT.
    const uint8_t *center = buffer.data + static_cast<size_t>(100) * static_cast<size_t>(buffer.row_bytes) + static_cast<size_t>(100) * 4;
    EXPECT_GT(center[3], 0);
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
    const uint8_t *inside = buffer.data + static_cast<size_t>(50) * static_cast<size_t>(buffer.row_bytes) + static_cast<size_t>(50) * 4;
    EXPECT_GT(inside[3], 0); // pin rect visible, same as le_set_current_design(handle, 0) would give
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
    const uint8_t *inside = buffer.data + static_cast<size_t>(50) * static_cast<size_t>(buffer.row_bytes) + static_cast<size_t>(50) * 4;
    EXPECT_GT(inside[3], 0); // pin rect still visible exactly where it was
}

TEST_F(ApiFixture, ZoomKeepsTheAnchorPixelFixedOnScreen)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design(handle, 0), 0);

    le_set_viewport_size(handle, 100, 100);
    le_zoom(handle, 100.0 / 10000.0 - 1.0, 0, 100); // -> scale 0.01, pan (0, 0)

    LePixelBuffer before = le_render_pixel_buffer(handle);
    ASSERT_NE(before.data, nullptr);
    const uint8_t *before_pixel = before.data + static_cast<size_t>(50) * static_cast<size_t>(before.row_bytes) + static_cast<size_t>(50) * 4;
    ASSERT_GT(before_pixel[3], 0); // baseline: pin rect visible at (50, 50)

    // Zoom in 2x anchored at the same pixel being sampled - the dbu point
    // under (50, 50) must still be under (50, 50) afterward, so it should
    // still show the same pin rect content.
    le_zoom(handle, 1.0, 50, 50);

    LePixelBuffer after = le_render_pixel_buffer(handle);
    ASSERT_NE(after.data, nullptr);
    const uint8_t *after_pixel = after.data + static_cast<size_t>(50) * static_cast<size_t>(after.row_bytes) + static_cast<size_t>(50) * 4;
    EXPECT_GT(after_pixel[3], 0);
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
    const uint8_t *before_pixel = before.data + static_cast<size_t>(50) * static_cast<size_t>(before.row_bytes) + static_cast<size_t>(50) * 4;
    ASSERT_GT(before_pixel[3], 0); // baseline: pin rect visible at (50, 50)

    // Pan by 2 full viewport widths in dbu x - the whole 10000-dbu-wide
    // macro (well under one viewport width at this scale) ends up entirely
    // out of view.
    le_pan(handle, 2.0, 0.0);

    LePixelBuffer after = le_render_pixel_buffer(handle);
    ASSERT_NE(after.data, nullptr);
    const uint8_t *after_pixel = after.data + static_cast<size_t>(50) * static_cast<size_t>(after.row_bytes) + static_cast<size_t>(50) * 4;
    EXPECT_EQ(after_pixel[3], 0);
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
    // Sample well inside that region, away from any antialiased edge.
    const uint8_t *inside = buffer.data + static_cast<size_t>(50) * static_cast<size_t>(buffer.row_bytes) + static_cast<size_t>(50) * 4;
    EXPECT_GT(inside[3], 0); // fill alpha - something was actually drawn here

    // Well outside the pin rect, but still inside the macro boundary
    // outline - the boundary is stroke-only (no fill), so this should be
    // fully transparent.
    const uint8_t *outside = buffer.data + static_cast<size_t>(5) * static_cast<size_t>(buffer.row_bytes) + static_cast<size_t>(5) * 4;
    EXPECT_EQ(outside[3], 0);
}
