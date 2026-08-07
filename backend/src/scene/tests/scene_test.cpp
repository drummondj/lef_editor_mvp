#include "../scene.hpp"
#include <gtest/gtest.h>

using namespace le;

TEST(Scene, DefaultStateIsSensible)
{
    Scene scene;

    EXPECT_FALSE(scene.current_abstract().valid());
    EXPECT_EQ(scene.pan().x, 0);
    EXPECT_EQ(scene.pan().y, 0);
    EXPECT_DOUBLE_EQ(scene.scale(), 1.0);
    EXPECT_EQ(scene.viewport_width_px(), 0);
    EXPECT_EQ(scene.viewport_height_px(), 0);
    EXPECT_TRUE(scene.selection().empty());
    EXPECT_EQ(scene.viewport_version(), 0u);
    EXPECT_EQ(scene.visibility_version(), 0u);
    EXPECT_EQ(scene.minor_grid_spacing(), 5);
    EXPECT_EQ(scene.major_grid_spacing(), 50);
}

TEST(Scene, CurrentAbstractRoundTrips)
{
    Scene scene;
    AbstractId id{5, 1};
    scene.set_current_abstract(id);
    EXPECT_EQ(scene.current_abstract(), id);
}

TEST(Scene, SwitchingToADifferentAbstractClearsSelectionAndHover)
{
    // Regression: TerminalId/ObstructionId are plain {index,generation}
    // pool handles, not namespaced by Abstract - a selection/hover left
    // over from the old Abstract could otherwise reference nothing (best
    // case) or an unrelated object that happens to reuse the same pool
    // slot in the new Abstract (worst case).
    Scene scene;
    scene.set_current_abstract(AbstractId{1, 0});

    Shape outline;
    outline.rects.push_back(Rect{.ll = {0, 0}, .ur = {10, 10}});
    scene.select(TerminalId{1, 0});
    scene.set_hover(HoverTarget{.origin = TerminalId{1, 0}, .outline = outline});
    ASSERT_FALSE(scene.selection().empty());
    ASSERT_TRUE(scene.hover().has_value());

    scene.set_current_abstract(AbstractId{2, 0});
    EXPECT_TRUE(scene.selection().empty());
    EXPECT_FALSE(scene.hover().has_value());
}

TEST(Scene, SwitchingToADifferentAbstractBumpsSelectionVersionOnlyIfSelectionWasNonEmpty)
{
    Scene scene;
    scene.set_current_abstract(AbstractId{1, 0});
    EXPECT_EQ(scene.selection_version(), 0u);

    // Nothing selected - switching must not bump the version for nothing.
    scene.set_current_abstract(AbstractId{2, 0});
    EXPECT_EQ(scene.selection_version(), 0u);

    scene.select(TerminalId{1, 0});
    ASSERT_EQ(scene.selection_version(), 1u);

    scene.set_current_abstract(AbstractId{3, 0});
    EXPECT_EQ(scene.selection_version(), 2u);
}

TEST(Scene, SettingTheSameCurrentAbstractAgainIsANoOp)
{
    Scene scene;
    scene.set_current_abstract(AbstractId{1, 0});
    scene.select(TerminalId{1, 0});
    ASSERT_EQ(scene.selection_version(), 1u);

    scene.set_current_abstract(AbstractId{1, 0}); // same Abstract already displayed
    EXPECT_FALSE(scene.selection().empty());
    EXPECT_EQ(scene.selection_version(), 1u);
}

TEST(Scene, SelectWithNoPieceLeavesPieceUnset)
{
    Scene scene;
    scene.select(TerminalId{1, 0});

    EXPECT_TRUE(scene.is_selected(TerminalId{1, 0}));
    ASSERT_FALSE(scene.selection().empty());
    EXPECT_FALSE(scene.selection().front().piece.has_value());
}

TEST(Scene, SelectWithAPieceRecordsThatPiece)
{
    Scene scene;
    Shape piece;
    piece.rects.push_back(Rect{.ll = {0, 0}, .ur = {10, 10}});
    scene.select(TerminalId{1, 0}, piece);

    EXPECT_TRUE(scene.is_selected(TerminalId{1, 0}));
    ASSERT_FALSE(scene.selection().empty());
    ASSERT_TRUE(scene.selection().front().piece.has_value());
    EXPECT_EQ(to_string(*scene.selection().front().piece), to_string(piece));
}

TEST(Scene, SelectingADifferentPieceOfAnAlreadySelectedOriginAddsASecondEntry)
{
    // The actual reported bug: shift-clicking a second shape within the
    // same Terminal/Obstruction must add a second selection entry - both
    // pieces end up independently selected/highlighted/reportable - not
    // replace the first one or no-op against it.
    Scene scene;
    Shape piece_a;
    piece_a.rects.push_back(Rect{.ll = {0, 0}, .ur = {10, 10}});
    Shape piece_b;
    piece_b.rects.push_back(Rect{.ll = {20, 20}, .ur = {30, 30}});

    scene.select(TerminalId{1, 0}, piece_a);
    ASSERT_EQ(scene.selection_version(), 1u);
    ASSERT_EQ(scene.selection().size(), 1u);

    scene.select(TerminalId{1, 0}, piece_b); // shift-clicking a different piece of the same Terminal
    EXPECT_EQ(scene.selection_version(), 2u);
    ASSERT_EQ(scene.selection().size(), 2u); // both pieces are now separately selected

    ASSERT_TRUE(scene.selection()[0].piece.has_value());
    EXPECT_EQ(to_string(*scene.selection()[0].piece), to_string(piece_a));
    ASSERT_TRUE(scene.selection()[1].piece.has_value());
    EXPECT_EQ(to_string(*scene.selection()[1].piece), to_string(piece_b));
}

TEST(Scene, ReselectingTheExactSamePieceOfTheSameOriginIsANoOp)
{
    Scene scene;
    Shape piece;
    piece.rects.push_back(Rect{.ll = {0, 0}, .ur = {10, 10}});

    scene.select(TerminalId{1, 0}, piece);
    ASSERT_EQ(scene.selection_version(), 1u);

    scene.select(TerminalId{1, 0}, piece); // identical geometry, not a new piece
    EXPECT_EQ(scene.selection_version(), 1u);
    EXPECT_EQ(scene.selection().size(), 1u);
}

TEST(Scene, SamePieceDedupDistinguishesDifferentPolygonPiecesAndNoOpsOnIdenticalOnes)
{
    // Scene::same_piece's polygon branch, otherwise untested - every other
    // piece-selection test here uses a rect-only piece.
    Scene scene;
    Shape polygon_a;
    polygon_a.polygons.push_back(Polygon{.points = {Point{0, 0}, Point{10, 0}, Point{5, 10}}});
    Shape polygon_b;
    polygon_b.polygons.push_back(Polygon{.points = {Point{20, 20}, Point{30, 20}, Point{25, 30}}});

    scene.select(TerminalId{1, 0}, polygon_a);
    ASSERT_EQ(scene.selection().size(), 1u);

    scene.select(TerminalId{1, 0}, polygon_a); // identical polygon - no-op
    EXPECT_EQ(scene.selection().size(), 1u);

    scene.select(TerminalId{1, 0}, polygon_b); // different polygon - adds a second entry
    ASSERT_EQ(scene.selection().size(), 2u);
    EXPECT_EQ(to_string(*scene.selection()[0].piece), to_string(polygon_a));
    EXPECT_EQ(to_string(*scene.selection()[1].piece), to_string(polygon_b));
}

TEST(Scene, SamePieceDedupDistinguishesDifferentPathPiecesAndNoOpsOnIdenticalOnes)
{
    // Scene::same_piece's path branch (width + polygon), otherwise
    // untested - every other piece-selection test here uses a rect-only
    // piece.
    Scene scene;
    Shape path_a;
    path_a.paths.push_back(Path{.polygon = Polygon{.points = {Point{0, 0}, Point{10, 0}}}, .width = 2});
    Shape path_b_different_polygon;
    path_b_different_polygon.paths.push_back(Path{.polygon = Polygon{.points = {Point{0, 0}, Point{0, 10}}}, .width = 2});
    Shape path_c_different_width_only;
    path_c_different_width_only.paths.push_back(Path{.polygon = Polygon{.points = {Point{0, 0}, Point{10, 0}}}, .width = 4});

    scene.select(TerminalId{1, 0}, path_a);
    ASSERT_EQ(scene.selection().size(), 1u);

    scene.select(TerminalId{1, 0}, path_a); // identical path - no-op
    EXPECT_EQ(scene.selection().size(), 1u);

    scene.select(TerminalId{1, 0}, path_b_different_polygon); // different centerline - adds a second entry
    ASSERT_EQ(scene.selection().size(), 2u);

    scene.select(TerminalId{1, 0}, path_c_different_width_only); // same centerline, different width - adds a third entry
    ASSERT_EQ(scene.selection().size(), 3u);
}

TEST(Scene, SelectDedupsCorrectlyAcrossManyDistinctPiecesSharingOneOrigin)
{
    // Regression/refactor-safety test for select()'s signature-bucketed
    // dedup (piece_signature + selection_index_), which replaced a linear
    // scan of the whole selection to fix an O(N^2) drag-select cost when
    // many pieces share one origin (a real design shape: an Obstruction's
    // whole OBS block is one ObstructionId, however many rects/polygons/
    // paths it contains) - see BENCHMARKS.md. Mixes distinct new pieces
    // with re-selecting already-selected ones (in original and reverse
    // order, so a signature-bucket-position bug couldn't hide behind
    // insertion order) and checks the exact resulting count/version,
    // proving the bucketed lookup doesn't miss real duplicates or
    // conflate distinct pieces that happen to land in the same bucket.
    Scene scene;
    const ObstructionId origin{1, 0};

    std::vector<Shape> pieces;
    for (int i = 0; i < 20; ++i)
    {
        Shape piece;
        piece.layer_name = "M1";
        piece.rects.push_back(Rect{.ll = {i * 10, 0}, .ur = {i * 10 + 5, 5}});
        pieces.push_back(piece);
    }

    for (const Shape &piece : pieces)
        scene.select(origin, piece);
    ASSERT_EQ(scene.selection().size(), 20u);
    ASSERT_EQ(scene.selection_version(), 20u);

    // Re-select every piece again, in reverse order - every one should be
    // recognized as an existing duplicate (no-op), regardless of which
    // bucket position it was originally inserted at.
    for (auto it = pieces.rbegin(); it != pieces.rend(); ++it)
        scene.select(origin, *it);
    EXPECT_EQ(scene.selection().size(), 20u);
    EXPECT_EQ(scene.selection_version(), 20u);

    // One genuinely new piece still gets added correctly afterward.
    Shape new_piece;
    new_piece.layer_name = "M1";
    new_piece.rects.push_back(Rect{.ll = {1000, 1000}, .ur = {1005, 1005}});
    scene.select(origin, new_piece);
    EXPECT_EQ(scene.selection().size(), 21u);
    EXPECT_EQ(scene.selection_version(), 21u);
}

TEST(Scene, SelectingWithNoPieceAfterAPieceWasAlreadySelectedAddsAWholeObjectEntryWithoutDisturbingThePiece)
{
    // e.g. a shift-drag that happens to re-enclose an object a previous
    // click already recorded a specific piece for - the drag path has no
    // single piece to offer (Pipeline::hit_test_rect has no per-piece
    // result). This adds a second, whole-object entry rather than
    // erasing or being absorbed into the earlier click's piece entry -
    // select()'s dedup only ever collapses two calls that agree on both
    // origin *and* piece.
    Scene scene;
    Shape piece;
    piece.rects.push_back(Rect{.ll = {0, 0}, .ur = {10, 10}});

    scene.select(TerminalId{1, 0}, piece);
    ASSERT_EQ(scene.selection().size(), 1u);

    scene.select(TerminalId{1, 0}); // no piece - as le_mouse_up's drag branch calls it
    EXPECT_EQ(scene.selection().size(), 2u);

    ASSERT_TRUE(scene.selection()[0].piece.has_value()); // original piece entry untouched
    EXPECT_EQ(to_string(*scene.selection()[0].piece), to_string(piece));
    EXPECT_FALSE(scene.selection()[1].piece.has_value()); // new whole-object entry
}

TEST(Scene, SelectingWithNoPieceTwiceForTheSameOriginStaysOneEntry)
{
    // Mirrors Pipeline::hit_test_rect's own behavior - it can push the
    // same origin more than once (once per fully-enclosed RenderedShape
    // piece of a multi-port Terminal), and those must still collapse
    // into a single whole-object selection entry, not one per push.
    Scene scene;
    scene.select(TerminalId{1, 0});
    ASSERT_EQ(scene.selection().size(), 1u);
    ASSERT_EQ(scene.selection_version(), 1u);

    scene.select(TerminalId{1, 0});
    EXPECT_EQ(scene.selection().size(), 1u);
    EXPECT_EQ(scene.selection_version(), 1u);
}

TEST(Scene, DeselectRemovesAnEntryRegardlessOfWhetherItHasAPiece)
{
    Scene scene;
    Shape piece;
    piece.rects.push_back(Rect{.ll = {0, 0}, .ur = {10, 10}});
    scene.select(TerminalId{1, 0}, piece);
    ASSERT_TRUE(scene.is_selected(TerminalId{1, 0}));

    scene.deselect(TerminalId{1, 0});
    EXPECT_FALSE(scene.is_selected(TerminalId{1, 0}));
    EXPECT_TRUE(scene.selection().empty());
}

TEST(Scene, PanAndViewportRoundTrip)
{
    Scene scene;
    scene.set_pan(Point{100, -200});
    scene.set_viewport_size(1920, 1080);

    EXPECT_EQ(scene.pan().x, 100);
    EXPECT_EQ(scene.pan().y, -200);
    EXPECT_EQ(scene.viewport_width_px(), 1920);
    EXPECT_EQ(scene.viewport_height_px(), 1080);
}

TEST(Scene, SetScaleIgnoresNonPositiveValues)
{
    Scene scene;
    scene.set_scale(2.5);
    ASSERT_DOUBLE_EQ(scene.scale(), 2.5);

    scene.set_scale(0.0);
    EXPECT_DOUBLE_EQ(scene.scale(), 2.5); // unchanged

    scene.set_scale(-1.0);
    EXPECT_DOUBLE_EQ(scene.scale(), 2.5); // unchanged
}

TEST(Scene, ViewportVersionBumpsOnPanScaleAndViewportSizeChangesOnly)
{
    Scene scene;
    EXPECT_EQ(scene.viewport_version(), 0u);

    scene.set_pan(Point{1, 1});
    EXPECT_EQ(scene.viewport_version(), 1u);

    scene.set_scale(2.0);
    EXPECT_EQ(scene.viewport_version(), 2u);

    scene.set_viewport_size(100, 100);
    EXPECT_EQ(scene.viewport_version(), 3u);

    // A rejected (non-positive) scale must not bump the version - nothing
    // about the viewport actually changed.
    scene.set_scale(-1.0);
    EXPECT_EQ(scene.viewport_version(), 3u);

    // Unrelated state (visibility) must not bump it either.
    scene.set_layer_name_visible("M1", false);
    EXPECT_EQ(scene.viewport_version(), 3u);
}

TEST(Scene, GridSpacingRoundTrips)
{
    Scene scene;
    scene.set_minor_grid_spacing(10);
    scene.set_major_grid_spacing(100);

    EXPECT_EQ(scene.minor_grid_spacing(), 10);
    EXPECT_EQ(scene.major_grid_spacing(), 100);
}

TEST(Scene, GridSpacingIgnoresNonPositiveValues)
{
    Scene scene;
    scene.set_minor_grid_spacing(10);
    scene.set_major_grid_spacing(100);

    scene.set_minor_grid_spacing(0);
    scene.set_minor_grid_spacing(-5);
    EXPECT_EQ(scene.minor_grid_spacing(), 10); // unchanged

    scene.set_major_grid_spacing(0);
    scene.set_major_grid_spacing(-5);
    EXPECT_EQ(scene.major_grid_spacing(), 100); // unchanged
}

TEST(Scene, GridSpacingSettersBumpVisibilityVersion)
{
    // The grid is part of the rendered picture (see Renderer::draw_grid),
    // so changing its spacing must invalidate the same render cache
    // layer visibility does - unlike selectability, which doesn't.
    Scene scene;
    EXPECT_EQ(scene.visibility_version(), 0u);

    scene.set_minor_grid_spacing(10);
    EXPECT_EQ(scene.visibility_version(), 1u);

    scene.set_major_grid_spacing(100);
    EXPECT_EQ(scene.visibility_version(), 2u);

    // A rejected (non-positive) value must not bump it.
    scene.set_minor_grid_spacing(-5);
    EXPECT_EQ(scene.visibility_version(), 2u);
}

TEST(Scene, MousePositionDefaultsToUnset)
{
    Scene scene;
    EXPECT_FALSE(scene.has_mouse_position());
    EXPECT_EQ(scene.mouse_version(), 0u);
    EXPECT_FALSE(scene.mouse_dbu_position().has_value());
    EXPECT_FALSE(scene.snapped_mouse_position().has_value());
}

TEST(Scene, SetMousePositionBumpsMouseVersionNotViewportOrVisibilityVersion)
{
    // A mouse move must invalidate only the cheap overlay-picture cache
    // (see Renderer::build_overlay_picture/compose_with_overlays), not the
    // expensive design rasterize cache keyed on viewport/visibility
    // version - see scene.hpp's own comment on mouse_version_.
    Scene scene;
    scene.set_pan(Point{0, 0});
    scene.set_viewport_size(100, 100);
    const uint64_t viewport_version_before = scene.viewport_version();
    const uint64_t visibility_version_before = scene.visibility_version();

    scene.set_mouse_position(10, 20);
    EXPECT_TRUE(scene.has_mouse_position());
    EXPECT_EQ(scene.mouse_version(), 1u);
    EXPECT_EQ(scene.viewport_version(), viewport_version_before);
    EXPECT_EQ(scene.visibility_version(), visibility_version_before);

    scene.set_mouse_position(11, 20);
    EXPECT_EQ(scene.mouse_version(), 2u);
}

TEST(Scene, MouseDbuPositionUndoesPanScaleAndYFlip)
{
    // Pixel space is top-left origin/y-down (matches le_render_pixel_buffer's
    // output image and le_zoom's x/y); dbu space is y-up - see scene.hpp's
    // mouse_dbu_position comment for the exact inverse formula.
    Scene scene;
    scene.set_pan(Point{100, 200});
    scene.set_scale(2.0);
    scene.set_viewport_size(50, 40);

    scene.set_mouse_position(10, 10);
    const std::optional<Point> dbu = scene.mouse_dbu_position();
    ASSERT_TRUE(dbu.has_value());
    EXPECT_EQ(dbu->x, 100 + 10 / 2); // pan.x + x_px / scale
    EXPECT_EQ(dbu->y, 200 + (40 - 10) / 2); // pan.y + (height - y_px) / scale
}

TEST(Scene, SnappedMousePositionRoundsToNearestMinorGridMultiple)
{
    Scene scene;
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);
    scene.set_minor_grid_spacing(10);

    // dbu (23, 100 - 27) = (23, 73); nearest multiples of 10 are 20 and 70.
    scene.set_mouse_position(23, 27);
    const std::optional<Point> snapped = scene.snapped_mouse_position();
    ASSERT_TRUE(snapped.has_value());
    EXPECT_EQ(snapped->x, 20);
    EXPECT_EQ(snapped->y, 70);
}

TEST(Scene, ClearMousePositionResetsHasMousePositionAndBumpsVersionOnlyIfItWasSet)
{
    Scene scene;
    scene.set_mouse_position(5, 5);
    ASSERT_EQ(scene.mouse_version(), 1u);

    scene.clear_mouse_position();
    EXPECT_FALSE(scene.has_mouse_position());
    EXPECT_FALSE(scene.mouse_dbu_position().has_value());
    EXPECT_EQ(scene.mouse_version(), 2u);

    // Clearing an already-clear position is a no-op - must not bump the
    // version again (would otherwise invalidate caches for nothing).
    scene.clear_mouse_position();
    EXPECT_EQ(scene.mouse_version(), 2u);
}

TEST(Scene, PixelToDbuMatchesMouseDbuPositionsOwnFormula)
{
    // Same scenario as MouseDbuPositionUndoesPanScaleAndYFlip, but calling
    // pixel_to_dbu directly with an arbitrary x/y rather than going
    // through the stored mouse position - the two must agree exactly,
    // since mouse_dbu_position() is defined in terms of pixel_to_dbu.
    Scene scene;
    scene.set_pan(Point{100, 200});
    scene.set_scale(2.0);
    scene.set_viewport_size(50, 40);

    const Point dbu = scene.pixel_to_dbu(10, 10);
    EXPECT_EQ(dbu.x, 100 + 10 / 2);
    EXPECT_EQ(dbu.y, 200 + (40 - 10) / 2);

    scene.set_mouse_position(10, 10);
    ASSERT_TRUE(scene.mouse_dbu_position().has_value());
    EXPECT_EQ(scene.mouse_dbu_position()->x, dbu.x);
    EXPECT_EQ(scene.mouse_dbu_position()->y, dbu.y);
}

TEST(Scene, DragDefaultsToNotDragging)
{
    Scene scene;
    EXPECT_FALSE(scene.is_dragging());
    EXPECT_FALSE(scene.drag_rect_dbu().has_value());
}

TEST(Scene, BeginDragSetsStateAndBumpsMouseVersionNotViewportOrVisibilityVersion)
{
    Scene scene;
    const uint64_t viewport_version_before = scene.viewport_version();
    const uint64_t visibility_version_before = scene.visibility_version();

    scene.begin_drag(10, 20);
    EXPECT_TRUE(scene.is_dragging());
    EXPECT_EQ(scene.drag_start_x_px(), 10);
    EXPECT_EQ(scene.drag_start_y_px(), 20);
    EXPECT_EQ(scene.mouse_version(), 1u);
    EXPECT_EQ(scene.viewport_version(), viewport_version_before);
    EXPECT_EQ(scene.visibility_version(), visibility_version_before);
}

TEST(Scene, EndDragClearsDraggingAndBumpsMouseVersion)
{
    Scene scene;
    scene.begin_drag(10, 20);
    ASSERT_EQ(scene.mouse_version(), 1u);

    scene.end_drag();
    EXPECT_FALSE(scene.is_dragging());
    EXPECT_EQ(scene.mouse_version(), 2u);
}

TEST(Scene, DragRectDbuIsNulloptWithoutAMousePositionEvenWhileDragging)
{
    Scene scene;
    scene.begin_drag(10, 10);
    EXPECT_FALSE(scene.drag_rect_dbu().has_value()); // no mouse position set yet
}

TEST(Scene, DragRectDbuNormalizesRegardlessOfDragDirection)
{
    // Dragging from bottom-right up to top-left (in pixel space) must
    // still produce a rect with ll <= ur, not a rect with swapped/negative
    // extents.
    Scene scene;
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);

    scene.begin_drag(80, 80); // dbu (80, 20)
    scene.set_mouse_position(20, 20); // dbu (20, 80)

    const std::optional<Rect> rect = scene.drag_rect_dbu();
    ASSERT_TRUE(rect.has_value());
    EXPECT_EQ(rect->ll.x, 20);
    EXPECT_EQ(rect->ll.y, 20);
    EXPECT_EQ(rect->ur.x, 80);
    EXPECT_EQ(rect->ur.y, 80);
}

TEST(Scene, KeysDefaultToNotHeld)
{
    Scene scene;
    EXPECT_FALSE(scene.is_key_held(1));
}

TEST(Scene, PressKeyThenIsKeyHeldReturnsTrue)
{
    Scene scene;
    scene.press_key(1);
    EXPECT_TRUE(scene.is_key_held(1));
}

TEST(Scene, ReleaseKeyClearsAHeldKey)
{
    Scene scene;
    scene.press_key(1);
    ASSERT_TRUE(scene.is_key_held(1));

    scene.release_key(1);
    EXPECT_FALSE(scene.is_key_held(1));
}

TEST(Scene, ReleaseKeyWithoutAPrecedingPressIsANoOp)
{
    Scene scene;
    scene.release_key(1);
    EXPECT_FALSE(scene.is_key_held(1));
}

TEST(Scene, DifferentKeyCodesAreTrackedIndependently)
{
    Scene scene;
    scene.press_key(1);
    EXPECT_TRUE(scene.is_key_held(1));
    EXPECT_FALSE(scene.is_key_held(2));
}

TEST(Scene, ClearAllKeysReleasesEveryHeldKey)
{
    // The fix for a real reported bug: a modifier held at the moment a
    // widget loses keyboard focus never gets its matching release event,
    // so it would otherwise stay "held" forever, silently turning every
    // later plain click into a shift-click.
    Scene scene;
    scene.press_key(1);
    scene.press_key(2);
    ASSERT_TRUE(scene.is_key_held(1));
    ASSERT_TRUE(scene.is_key_held(2));

    scene.clear_all_keys();
    EXPECT_FALSE(scene.is_key_held(1));
    EXPECT_FALSE(scene.is_key_held(2));
}

TEST(Scene, ClearAllKeysWithNothingHeldIsANoOp)
{
    Scene scene;
    scene.clear_all_keys();
    EXPECT_FALSE(scene.is_key_held(1));
}

TEST(Scene, HoverDefaultsToUnset)
{
    Scene scene;
    EXPECT_FALSE(scene.hover().has_value());
}

TEST(Scene, HoverRoundTrips)
{
    Scene scene;
    Shape outline;
    outline.rects.push_back(Rect{.ll = {0, 0}, .ur = {10, 10}});

    scene.set_hover(HoverTarget{.origin = TerminalId{5, 1}, .outline = outline});

    ASSERT_TRUE(scene.hover().has_value());
    ASSERT_TRUE(std::holds_alternative<TerminalId>(scene.hover()->origin));
    EXPECT_EQ(std::get<TerminalId>(scene.hover()->origin), (TerminalId{5, 1}));
    ASSERT_EQ(scene.hover()->outline.rects.size(), 1u);
    EXPECT_EQ(scene.hover()->outline.rects.front().ur.x, 10);
}

TEST(Scene, ClearHoverResetsToUnset)
{
    Scene scene;
    Shape outline;
    scene.set_hover(HoverTarget{.origin = ObstructionId{2, 0}, .outline = outline});
    ASSERT_TRUE(scene.hover().has_value());

    scene.clear_hover();
    EXPECT_FALSE(scene.hover().has_value());
}

TEST(Scene, SetHoverAndClearHoverDoNotBumpViewportOrVisibilityVersion)
{
    // Hover is driven by every pointer-move event (see le_set_mouse_position)
    // - it must not invalidate the expensive design rasterize cache, only
    // the small mouse-overlay picture (already keyed on mouse_version,
    // which set_mouse_position bumps independently - see scene.hpp's own
    // comment on why set_hover needs no version counter of its own).
    Scene scene;
    const uint64_t viewport_version_before = scene.viewport_version();
    const uint64_t visibility_version_before = scene.visibility_version();

    Shape outline;
    scene.set_hover(HoverTarget{.origin = TerminalId{1, 0}, .outline = outline});
    scene.clear_hover();

    EXPECT_EQ(scene.viewport_version(), viewport_version_before);
    EXPECT_EQ(scene.visibility_version(), visibility_version_before);
}

TEST(Scene, LayerNameVisibilityDefaultsToTrueUntilSet)
{
    Scene scene;

    EXPECT_TRUE(scene.is_layer_name_visible("M1"));

    scene.set_layer_name_visible("M1", false);
    EXPECT_FALSE(scene.is_layer_name_visible("M1"));

    scene.set_layer_name_visible("M1", true);
    EXPECT_TRUE(scene.is_layer_name_visible("M1"));

    // A different, never-toggled layer name is unaffected.
    EXPECT_TRUE(scene.is_layer_name_visible("M2"));
}

TEST(Scene, PurposeVisibilityDefaultsToTrueUntilSet)
{
    Scene scene;

    EXPECT_TRUE(scene.is_purpose_visible(ViewLayerPurpose::OBSTRUCTION));

    scene.set_purpose_visible(ViewLayerPurpose::OBSTRUCTION, false);
    EXPECT_FALSE(scene.is_purpose_visible(ViewLayerPurpose::OBSTRUCTION));

    scene.set_purpose_visible(ViewLayerPurpose::OBSTRUCTION, true);
    EXPECT_TRUE(scene.is_purpose_visible(ViewLayerPurpose::OBSTRUCTION));

    // A different, never-toggled purpose is unaffected.
    EXPECT_TRUE(scene.is_purpose_visible(ViewLayerPurpose::TERMINAL));
}

TEST(Scene, IsViewLayerVisibleIsTheAndOfBothAxes)
{
    Scene scene;

    // Both axes default true -> visible.
    EXPECT_TRUE(scene.is_view_layer_visible("M1", ViewLayerPurpose::TERMINAL));

    // Layer name off, purpose still on -> not visible.
    scene.set_layer_name_visible("M1", false);
    EXPECT_FALSE(scene.is_view_layer_visible("M1", ViewLayerPurpose::TERMINAL));
    // A different layer name is unaffected by M1's toggle.
    EXPECT_TRUE(scene.is_view_layer_visible("M2", ViewLayerPurpose::TERMINAL));

    // Layer name back on, but now the purpose is off -> still not visible.
    scene.set_layer_name_visible("M1", true);
    scene.set_purpose_visible(ViewLayerPurpose::TERMINAL, false);
    EXPECT_FALSE(scene.is_view_layer_visible("M1", ViewLayerPurpose::TERMINAL));
    // OBSTRUCTION on the same layer is unaffected by the TERMINAL toggle.
    EXPECT_TRUE(scene.is_view_layer_visible("M1", ViewLayerPurpose::OBSTRUCTION));

    // Both axes on again -> visible.
    scene.set_purpose_visible(ViewLayerPurpose::TERMINAL, true);
    EXPECT_TRUE(scene.is_view_layer_visible("M1", ViewLayerPurpose::TERMINAL));
}

TEST(Scene, VisibilityVersionBumpsOnSetLayerNameVisibleAndSetPurposeVisible)
{
    Scene scene;
    EXPECT_EQ(scene.visibility_version(), 0u);

    scene.set_layer_name_visible("M1", false);
    EXPECT_EQ(scene.visibility_version(), 1u);

    scene.set_purpose_visible(ViewLayerPurpose::OBSTRUCTION, false);
    EXPECT_EQ(scene.visibility_version(), 2u);

    // Unrelated state (viewport) must not bump it.
    scene.set_pan(Point{5, 5});
    EXPECT_EQ(scene.visibility_version(), 2u);
}

TEST(Scene, LayerNameSelectabilityDefaultsToTrueUntilSet)
{
    Scene scene;

    EXPECT_TRUE(scene.is_layer_name_selectable("M1"));

    scene.set_layer_name_selectable("M1", false);
    EXPECT_FALSE(scene.is_layer_name_selectable("M1"));

    scene.set_layer_name_selectable("M1", true);
    EXPECT_TRUE(scene.is_layer_name_selectable("M1"));

    EXPECT_TRUE(scene.is_layer_name_selectable("M2"));
}

TEST(Scene, PurposeSelectabilityDefaultsToTrueUntilSet)
{
    Scene scene;

    EXPECT_TRUE(scene.is_purpose_selectable(ViewLayerPurpose::OBSTRUCTION));

    scene.set_purpose_selectable(ViewLayerPurpose::OBSTRUCTION, false);
    EXPECT_FALSE(scene.is_purpose_selectable(ViewLayerPurpose::OBSTRUCTION));

    scene.set_purpose_selectable(ViewLayerPurpose::OBSTRUCTION, true);
    EXPECT_TRUE(scene.is_purpose_selectable(ViewLayerPurpose::OBSTRUCTION));

    EXPECT_TRUE(scene.is_purpose_selectable(ViewLayerPurpose::TERMINAL));
}

TEST(Scene, IsViewLayerSelectableIsTheAndOfBothAxes)
{
    Scene scene;
    EXPECT_TRUE(scene.is_view_layer_selectable("M1", ViewLayerPurpose::OBSTRUCTION));

    scene.set_layer_name_selectable("M1", false);
    EXPECT_FALSE(scene.is_view_layer_selectable("M1", ViewLayerPurpose::OBSTRUCTION));

    scene.set_layer_name_selectable("M1", true);
    scene.set_purpose_selectable(ViewLayerPurpose::OBSTRUCTION, false);
    EXPECT_FALSE(scene.is_view_layer_selectable("M1", ViewLayerPurpose::OBSTRUCTION));
}

TEST(Scene, SetLayerNameSelectableAndSetPurposeSelectableDoNotBumpVisibilityVersion)
{
    // Selectability isn't consumed by Pipeline/Renderer caching (unlike
    // visibility) - it must not bump visibility_version(), or every
    // selectability toggle would force an unnecessary re-render.
    Scene scene;
    EXPECT_EQ(scene.visibility_version(), 0u);

    scene.set_layer_name_selectable("M1", false);
    EXPECT_EQ(scene.visibility_version(), 0u);

    scene.set_purpose_selectable(ViewLayerPurpose::OBSTRUCTION, false);
    EXPECT_EQ(scene.visibility_version(), 0u);
}

TEST(Scene, SelectDeselectAndClear)
{
    Scene scene;
    TerminalId terminal{1, 0};
    ObstructionId obstruction{2, 0};

    scene.select(terminal);
    scene.select(obstruction);
    EXPECT_TRUE(scene.is_selected(terminal));
    EXPECT_TRUE(scene.is_selected(obstruction));
    EXPECT_EQ(scene.selection().size(), 2u);

    // Selecting the same ref again must not duplicate it.
    scene.select(terminal);
    EXPECT_EQ(scene.selection().size(), 2u);

    scene.deselect(terminal);
    EXPECT_FALSE(scene.is_selected(terminal));
    EXPECT_TRUE(scene.is_selected(obstruction));
    EXPECT_EQ(scene.selection().size(), 1u);

    scene.clear_selection();
    EXPECT_TRUE(scene.selection().empty());
}

TEST(Scene, FitToContentCentersAndScalesToFillTheTighterAxis)
{
    Scene scene;
    scene.set_viewport_size(1000, 1000);

    // 100x50 dbu content, 20px padding on each side -> usable 960px, scale
    // bound by the wider (x) axis: 960 / 100 = 9.6.
    Rect bbox{.ll = Point{0, 0}, .ur = Point{100, 50}};
    scene.fit_to_content(bbox, 20);

    EXPECT_DOUBLE_EQ(scene.scale(), 9.6);

    // Content is centered: leftover space on y is (1000/9.6 - 50) ~= 54.17,
    // half of that offsets pan below the bbox's own ll.y.
    const double expected_pan_y = 0 - (1000.0 / 9.6 - 50.0) / 2.0;
    EXPECT_EQ(scene.pan().x, 0 - static_cast<int64_t>((1000.0 / 9.6 - 100.0) / 2.0));
    EXPECT_EQ(scene.pan().y, static_cast<int64_t>(expected_pan_y));
}

TEST(Scene, FitToContentWithNoBboxFallsBackToDefaultScaleAndPan)
{
    Scene scene;
    scene.set_viewport_size(500, 500);
    scene.set_scale(3.0);
    scene.set_pan(Point{10, 10});

    scene.fit_to_content(std::nullopt, 10);

    EXPECT_DOUBLE_EQ(scene.scale(), 1.0);
    EXPECT_EQ(scene.pan().x, 0);
    EXPECT_EQ(scene.pan().y, 0);
}

TEST(Scene, FitToContentWithZeroSizedViewportFallsBackToDefault)
{
    Scene scene;
    Rect bbox{.ll = Point{0, 0}, .ur = Point{100, 100}};
    scene.fit_to_content(bbox, 10);

    EXPECT_DOUBLE_EQ(scene.scale(), 1.0);
    EXPECT_EQ(scene.pan().x, 0);
    EXPECT_EQ(scene.pan().y, 0);
}

TEST(Scene, FitToContentWithDegenerateZeroWidthBboxDoesNotDivideByZero)
{
    Scene scene;
    scene.set_viewport_size(200, 200);

    // Zero-width content (e.g. a single vertical line) - scale must be
    // bounded by the y axis only, not blow up via a 1/0 on x.
    Rect bbox{.ll = Point{5, 0}, .ur = Point{5, 100}};
    scene.fit_to_content(bbox, 0);

    EXPECT_DOUBLE_EQ(scene.scale(), 2.0); // 200 / 100
}

TEST(Scene, SelectionDistinguishesObjectKindsWithTheSameRawId)
{
    // TerminalId{5,0} and ObstructionId{5,0} share the same {index,
    // generation}, but are different types - the variant must not confuse them.
    Scene scene;
    TerminalId terminal{5, 0};
    ObstructionId obstruction{5, 0};

    scene.select(terminal);
    EXPECT_TRUE(scene.is_selected(terminal));
    EXPECT_FALSE(scene.is_selected(obstruction));
}

TEST(Scene, SelectBumpsSelectionVersionOnlyOnAnActualChange)
{
    Scene scene;
    TerminalId terminal{5, 0};
    EXPECT_EQ(scene.selection_version(), 0u);

    scene.select(terminal);
    EXPECT_EQ(scene.selection_version(), 1u);

    // Already selected - a no-op, must not bump again.
    scene.select(terminal);
    EXPECT_EQ(scene.selection_version(), 1u);
}

TEST(Scene, DeselectBumpsSelectionVersionOnlyOnAnActualChange)
{
    Scene scene;
    TerminalId terminal{5, 0};
    scene.select(terminal);
    ASSERT_EQ(scene.selection_version(), 1u);

    scene.deselect(terminal);
    EXPECT_FALSE(scene.is_selected(terminal));
    EXPECT_EQ(scene.selection_version(), 2u);

    // Already gone - a no-op, must not bump again.
    scene.deselect(terminal);
    EXPECT_EQ(scene.selection_version(), 2u);
}

TEST(Scene, ClearSelectionBumpsSelectionVersionOnlyIfSelectionWasNonEmpty)
{
    Scene scene;
    TerminalId terminal{5, 0};

    scene.clear_selection(); // already empty - a no-op
    EXPECT_EQ(scene.selection_version(), 0u);

    scene.select(terminal);
    ASSERT_EQ(scene.selection_version(), 1u);

    scene.clear_selection();
    EXPECT_TRUE(scene.selection().empty());
    EXPECT_EQ(scene.selection_version(), 2u);
}

TEST(Scene, SelectionChangesDoNotBumpViewportOrVisibilityVersion)
{
    Scene scene;
    const uint64_t viewport_version_before = scene.viewport_version();
    const uint64_t visibility_version_before = scene.visibility_version();

    scene.select(TerminalId{5, 0});
    scene.clear_selection();

    EXPECT_EQ(scene.viewport_version(), viewport_version_before);
    EXPECT_EQ(scene.visibility_version(), visibility_version_before);
}
