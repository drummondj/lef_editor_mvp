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

    le_set_pan(handle, 0, 0);
    le_set_scale(handle, 10.0); // 10 px/dbu-micron-ish, matches DATABASE MICRONS 1000 -> 1000 dbu/micron
    le_set_viewport_size(handle, 200, 200);

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

TEST_F(ApiFixture, RenderPixelBufferDrawsThePinRectAtItsExpectedLocation)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str()), 0);
    ASSERT_EQ(le_set_current_design(handle, 0), 0);

    // MACRO SIZE is 10x10 microns, PIN A's RECT is (2,2)-(8,8) microns.
    // DATABASE MICRONS 1000 -> 1 micron = 1000 dbu. Scale chosen so the
    // whole 10x10 micron (10000x10000 dbu) macro fills a 100x100px buffer.
    le_set_pan(handle, 0, 0);
    le_set_scale(handle, 100.0 / 10000.0);
    le_set_viewport_size(handle, 100, 100);

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
