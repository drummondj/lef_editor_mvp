#include "../lef_reader.hpp"
#include <gtest/gtest.h>

using namespace le;

// src/lefdef/lef/TEST/complete.5.8.lef is the vendored LEF parser's own
// "everything" regression fixture. LEFReader only implements a subset of LEF
// (see lef_reader.cpp) - lefrSetRegisterUnusedCallbacks() means the parser
// still succeeds and just skips sections we haven't wired up a callback for.
// Assertions below cover only what LEFReader is expected to extract today;
// extend them as more LEF constructs get support.
namespace
{
    std::string complete_fixture_path()
    {
        return std::string(LEFDEF_TEST_DIR) + "/complete.5.8.lef";
    }

    std::string fixture_path(const std::string &name)
    {
        return std::string(IO_TEST_FIXTURES_DIR) + "/" + name;
    }

    TerminalId find_terminal_id(const Root &root, AbstractId abstract_id, const std::string &name)
    {
        for (auto terminal_id : root.get_abstract_terminals(abstract_id))
        {
            if (root.get_terminal(terminal_id)->name == name)
                return terminal_id;
        }
        return TerminalId{};
    }
}

class LEFReaderCompleteFixture : public ::testing::Test
{
protected:
    void SetUp() override
    {
        result = reader.read_lef(complete_fixture_path(), root, "test_lib");
    }

    Root root;
    LEFReader reader;
    int result;
};

TEST_F(LEFReaderCompleteFixture, ParsesSuccessfullyAndReadsUnits)
{
    ASSERT_EQ(result, 0);
    ASSERT_EQ(root.get_technology_size(), 1u);

    TechnologyId technology_id = root.get_technology_ids().front();
    EXPECT_DOUBLE_EQ(root.get_technology(technology_id)->database_units_microns, 20000.0);
}

TEST_F(LEFReaderCompleteFixture, CreatesAllLayersWithTypeAndDirection)
{
    // 25 uppercase `LAYER ...` blocks plus one lowercase `layer OVERLAP`.
    EXPECT_EQ(root.get_layer_ids().size(), 26u);

    LayerId m1_id = root.get_layer_by_name("M1");
    ASSERT_TRUE(m1_id.valid());
    const LayerData *m1 = root.get_layer(m1_id);
    EXPECT_EQ(m1->type, "ROUTING");
    EXPECT_EQ(m1->direction, RoutingDirection::H);

    LayerId v1_id = root.get_layer_by_name("V1");
    ASSERT_TRUE(v1_id.valid());
    EXPECT_EQ(root.get_layer(v1_id)->type, "CUT");
    EXPECT_EQ(root.get_layer(v1_id)->direction, RoutingDirection::NONE);
}

TEST_F(LEFReaderCompleteFixture, ReadsBasicScalarLayerProperties)
{
    // LAYER M1: TYPE ROUTING ; WIDTH 1 ; WIREEXTENSION 7 ; PITCH 1.8 ;
    // DIRECTION HORIZONTAL ; RESISTANCE RPERSQ 0.103 ;
    // CAPACITANCE CPERSQDIST 0.000156 ; - no OFFSET/AREA/SPACING/HEIGHT/
    // THICKNESS statements, so those stay nullopt (UPDATES.md 12 Phase 1's
    // has*()-guarded basic scalar LAYER coverage).
    LayerId m1_id = root.get_layer_by_name("M1");
    ASSERT_TRUE(m1_id.valid());
    const LayerData *m1 = root.get_layer(m1_id);

    ASSERT_TRUE(m1->width.has_value());
    EXPECT_EQ(*m1->width, 20000); // 1um * 20000 dbu/um
    ASSERT_TRUE(m1->pitch.has_value());
    EXPECT_EQ(*m1->pitch, 36000); // 1.8um * 20000 dbu/um
    ASSERT_TRUE(m1->wire_extension.has_value());
    EXPECT_EQ(*m1->wire_extension, 140000); // 7um * 20000 dbu/um
    ASSERT_TRUE(m1->resistance.has_value());
    EXPECT_DOUBLE_EQ(*m1->resistance, 0.103);
    ASSERT_TRUE(m1->capacitance.has_value());
    EXPECT_DOUBLE_EQ(*m1->capacitance, 0.000156);

    EXPECT_FALSE(m1->offset.has_value());
    EXPECT_FALSE(m1->area.has_value());
    EXPECT_FALSE(m1->spacing.has_value());
    EXPECT_FALSE(m1->height.has_value());
    EXPECT_FALSE(m1->thickness.has_value());
}

TEST_F(LEFReaderCompleteFixture, CreatesOneLibraryAndOneDesignPerMacro)
{
    ASSERT_EQ(root.get_library_size(), 1u);
    EXPECT_TRUE(root.get_library_by_name("test_lib").valid());

    // 14 `MACRO ...` blocks in the fixture, each becomes a Design + Abstract.
    EXPECT_EQ(root.get_design_size(), 14u);
    EXPECT_EQ(root.get_abstract_size(), 14u);

    for (auto design_id : root.get_design_ids())
        EXPECT_TRUE(root.get_design_abstract(design_id).valid()) << root.get_design(design_id)->name;
}

TEST_F(LEFReaderCompleteFixture, ConvertsMacroSizeAndOriginToDbu)
{
    // MACRO INV has SIZE 67.2 BY 24, no ORIGIN, at DATABASE MICRONS 20000.
    DesignId design_id = root.get_design_by_name("INV");
    ASSERT_TRUE(design_id.valid());
    AbstractId abstract_id = root.get_design_abstract(design_id);
    const AbstractData *abstract = root.get_abstract(abstract_id);

    EXPECT_EQ(abstract->size.x, 1344000);
    EXPECT_EQ(abstract->size.y, 480000);
    EXPECT_EQ(abstract->type, "CORE");

    // No OVERLAP obstruction on this macro, so the boundary falls back to a
    // single rect polygon built from origin (default {0,0}) + size.
    ASSERT_EQ(abstract->boundary.size(), 1u);
    const Polygon &boundary = abstract->boundary.front();
    ASSERT_EQ(boundary.points.size(), 5u);
    EXPECT_EQ(boundary.points[0].x, 0);
    EXPECT_EQ(boundary.points[0].y, 0);
    EXPECT_EQ(boundary.points[2].x, 1344000);
    EXPECT_EQ(boundary.points[2].y, 480000);
}

TEST_F(LEFReaderCompleteFixture, CreatesPinsWithPortShapes)
{
    // MACRO INV has 4 PINs: Z, A, VDD, VSS.
    DesignId design_id = root.get_design_by_name("INV");
    AbstractId abstract_id = root.get_design_abstract(design_id);
    EXPECT_EQ(root.get_abstract_terminals(abstract_id).size(), 4u);

    // PIN A DIRECTION INPUT; PORT LAYER M1 ; PATH 25.2 15 ; END
    TerminalId pin_a_id = find_terminal_id(root, abstract_id, "A");
    ASSERT_TRUE(pin_a_id.valid());
    EXPECT_EQ(root.get_terminal(pin_a_id)->direction, SignalDirection::INPUT);

    auto pin_a_ports = root.get_terminal_ports(pin_a_id);
    ASSERT_EQ(pin_a_ports.size(), 1u);
    const TerminalPortData *port = root.get_terminal_port(pin_a_ports.front());

    ASSERT_EQ(port->shapes.size(), 1u);
    const Shape &shape = port->shapes.front();
    EXPECT_EQ(shape.layer_name, "M1");
    ASSERT_EQ(shape.paths.size(), 1u);
    EXPECT_EQ(shape.paths.front().width, 0u); // no WIDTH statement precedes this PATH
}

TEST_F(LEFReaderCompleteFixture, ObstructionCollectsRectsAndPathsButIgnoresVias)
{
    // MACRO INV's OBS block on LAYER M1: 1 RECT, a 2x1 RECT ITERATE, a 1x2
    // PATH ITERATE, 2 more PATHs, 4 VIAs (unsupported, must be ignored - not
    // counted as rects/paths/iterates), then a final RECT. Width 0.1um.
    // ITERATE statements are stored raw (UPDATES.md 12 Phase 1's ITERATE
    // rework), not pre-expanded - see rect_iterates/path_iterates below,
    // and Pipeline::generate_shapes for where they're expanded.
    DesignId design_id = root.get_design_by_name("INV");
    AbstractId abstract_id = root.get_design_abstract(design_id);

    ASSERT_EQ(root.get_abstract_obstructions(abstract_id).size(), 1u);
    const ObstructionData *obstruction = root.get_obstruction(root.get_abstract_obstructions(abstract_id).front());

    ASSERT_EQ(obstruction->shapes.size(), 1u);
    const Shape &shape = obstruction->shapes.front();
    EXPECT_EQ(shape.layer_name, "M1");
    EXPECT_EQ(shape.rects.size(), 2u); // 1 + 1 final, VIAs excluded, ITERATE stored separately
    EXPECT_EQ(shape.paths.size(), 2u); // 2 singles, VIAs excluded, ITERATE stored separately
    EXPECT_EQ(shape.polygons.size(), 0u);

    ASSERT_EQ(shape.rect_iterates.size(), 1u);
    EXPECT_EQ(shape.rect_iterates.front().num_x, 2);
    EXPECT_EQ(shape.rect_iterates.front().num_y, 1);
    EXPECT_EQ(shape.rect_iterates.front().space_x, 400000); // 20.0um * 20000 dbu/um
    EXPECT_EQ(shape.rect_iterates.front().space_y, 0);

    ASSERT_EQ(shape.path_iterates.size(), 1u);
    EXPECT_EQ(shape.path_iterates.front().num_x, 1);
    EXPECT_EQ(shape.path_iterates.front().num_y, 2);
    EXPECT_EQ(shape.path_iterates.front().space_x, 0);
    EXPECT_EQ(shape.path_iterates.front().space_y, 28920000); // 1446um * 20000 dbu/um
    EXPECT_EQ(shape.path_iterates.front().path.width, 2000);  // WIDTH 0.1um * 20000 dbu/um

    for (const auto &path : shape.paths)
        EXPECT_EQ(path.width, 2000); // WIDTH 0.1um * 20000 dbu/um
}

// Two overlapping bugs made every FOREIGN past the first one silently lose
// its origin: (1) lef_reader.cpp called hasForeignOrigin()/hasForeignOrient()
// with no index, always checking foreign #0's flags regardless of which
// foreign was being processed; (2) the vendored parser's hasForeignOrigin_ is
// actually populated from the orientation code, not a real "has a point"
// flag (lefiMacro::addForeign) - hasForeignPoint(i) is the field that's
// genuinely wired to it. FOREIGNTEST's F0 (index 0, so bug #1 alone wouldn't
// expose it) has an explicit ORIENT N (code 0, falsy) alongside a real
// origin, which bug #2 alone was enough to misread as "no origin".
TEST(LEFReaderForeignIndex, EachForeignKeepsItsOwnOriginAndOrient)
{
    Root root;
    LEFReader reader;
    int result = reader.read_lef(fixture_path("foreign_index.lef"), root, "test_lib");
    ASSERT_EQ(result, 0);

    DesignId design_id = root.get_design_by_name("FOREIGNTEST");
    ASSERT_TRUE(design_id.valid());
    const AbstractData *abstract = root.get_abstract(root.get_design_abstract(design_id));

    ASSERT_EQ(abstract->foreigns.size(), 4u);

    const Foreign &f0 = abstract->foreigns[0]; // FOREIGN F0 ( 1 2 ) N ;
    EXPECT_EQ(f0.origin.x, 1000);
    EXPECT_EQ(f0.origin.y, 2000);
    EXPECT_EQ(f0.orient, Orientation::N);

    const Foreign &f1 = abstract->foreigns[1]; // FOREIGN F1 ( 3 4 ) ;
    EXPECT_EQ(f1.origin.x, 3000);
    EXPECT_EQ(f1.origin.y, 4000);
    EXPECT_EQ(f1.orient, Orientation::N); // no orient specified -> default

    const Foreign &f2 = abstract->foreigns[2]; // FOREIGN F2 ;
    EXPECT_EQ(f2.origin.x, 0);
    EXPECT_EQ(f2.origin.y, 0);
    EXPECT_EQ(f2.orient, Orientation::N);

    const Foreign &f3 = abstract->foreigns[3]; // FOREIGN F3 ( 5 6 ) E ;
    EXPECT_EQ(f3.origin.x, 5000);
    EXPECT_EQ(f3.origin.y, 6000);
    EXPECT_EQ(f3.orient, Orientation::E);
}

// src/lefdef/lef/TEST/complete.5.8.lef has no MACRO with an OBS on the
// OVERLAP layer, so post_process()'s union-of-OVERLAP-shapes boundary path
// (as opposed to the SIZE/ORIGIN fallback) was never exercised. This
// hand-written fixture covers it.
TEST(LEFReaderOverlapBoundary, BoundaryComesFromOverlapObsNotMacroSize)
{
    Root root;
    LEFReader reader;
    int result = reader.read_lef(fixture_path("overlap_boundary.lef"), root, "test_lib");
    ASSERT_EQ(result, 0);

    DesignId design_id = root.get_design_by_name("OVERLAPTEST");
    ASSERT_TRUE(design_id.valid());
    AbstractId abstract_id = root.get_design_abstract(design_id);
    const AbstractData *abstract = root.get_abstract(abstract_id);

    // SIZE 10 BY 10 at DATABASE MICRONS 1000 would give a (0,0)-(10000,10000)
    // fallback boundary; OBS has an OVERLAP RECT 1 1 9 9 (-> (1000,1000)-
    // (9000,9000) dbu) plus an M1 RECT 0 0 2 2 that must NOT be included.
    ASSERT_EQ(abstract->boundary.size(), 1u);
    const Polygon &boundary = abstract->boundary.front();
    ASSERT_GE(boundary.points.size(), 4u);

    int64_t min_x = boundary.points.front().x, max_x = min_x;
    int64_t min_y = boundary.points.front().y, max_y = min_y;
    for (const auto &point : boundary.points)
    {
        min_x = std::min(min_x, point.x);
        max_x = std::max(max_x, point.x);
        min_y = std::min(min_y, point.y);
        max_y = std::max(max_y, point.y);
    }

    EXPECT_EQ(min_x, 1000);
    EXPECT_EQ(min_y, 1000);
    EXPECT_EQ(max_x, 9000);
    EXPECT_EQ(max_y, 9000);
}

TEST(LEFReaderErrors, FileNotFoundReturnsOne)
{
    Root root;
    LEFReader reader;
    EXPECT_EQ(reader.read_lef("/no/such/file.lef", root, "test_lib"), 1);
    ASSERT_FALSE(reader.messages().empty());
    EXPECT_NE(reader.messages().front().find("/no/such/file.lef"), std::string::npos);
}

TEST(LEFReaderErrors, MalformedFileReturnsTwo)
{
    Root root;
    LEFReader reader;
    EXPECT_EQ(reader.read_lef(fixture_path("malformed.lef"), root, "test_lib"), 2);
    EXPECT_FALSE(reader.messages().empty());
}

// LEFReader::messages() - the queue api.cpp's le_read_lef drains into a
// persistent, ever-growing handle-owned list for the GUI (UPDATES.md
// item 3). A distinct suite from LEFReaderErrors above since these
// tests are about message *content*, not read_lef's own return code.
TEST(LEFReaderMessages, SuccessfulReadWithNoDiagnosticsLeavesMessagesEmpty)
{
    Root root;
    LEFReader reader;
    ASSERT_EQ(reader.read_lef(fixture_path("units_1000.lef"), root, "test_lib"), 0);
    EXPECT_TRUE(reader.messages().empty());
}

// CURRENTDEN inside a LAYER block is unconditionally flagged obsolete by
// the vendored parser on any LEF version >= 5.2 (lef.tab.cpp's
// `layer_option: K_CURRENTDEN int_number ';'` rule calls
// lefWarning(2079, ...) regardless of layer type once past that version
// check) - a reliable way to exercise the warning path (routed through
// lefrSetWarningLogFunction, same registration lefInfo also uses)
// without the parse itself failing.
TEST(LEFReaderMessages, ObsoleteCurrentdenStatementProducesAWarningOnAnOtherwiseSuccessfulRead)
{
    Root root;
    LEFReader reader;
    ASSERT_EQ(reader.read_lef(fixture_path("warning_currentden.lef"), root, "test_lib"), 0);
    ASSERT_FALSE(reader.messages().empty());
    EXPECT_NE(reader.messages().front().find("WARNING"), std::string::npos);
    EXPECT_NE(reader.messages().front().find("obsolete"), std::string::npos);
}

// messages_ is cleared at the top of every read_lef() call (not
// accumulated across calls on a reused instance) - api.cpp's
// le_read_lef is what accumulates these into a persistent queue, not
// LEFReader itself.
TEST(LEFReaderMessages, MessagesAreClearedNotAccumulatedAcrossReusedReaderCalls)
{
    Root root;
    LEFReader reader;
    ASSERT_EQ(reader.read_lef(fixture_path("malformed.lef"), root, "test_lib"), 2);
    ASSERT_FALSE(reader.messages().empty());

    Root root2;
    ASSERT_EQ(reader.read_lef(fixture_path("units_1000.lef"), root2, "test_lib"), 0);
    EXPECT_TRUE(reader.messages().empty());
}

// shapes_from_parser() has an `if (!shape.has_value())` guard before every
// RECT/RECT-ITERATE/PATH/PATH-ITERATE/POLYGON/POLYGON-ITERATE case, in case
// one appears before any LAYER. This confirms the grammar itself already
// rejects that (a parse failure, before our callback ever runs) - so those
// guards are unreachable in practice, but kept anyway as defense-in-depth
// at a file-parsing boundary rather than removed like pure-internal dead
// code (see geometry.hpp's union_shapes/get_label_location cleanup).
TEST(LEFReaderErrors, GeometryWithoutPrecedingLayerFailsAtTheGrammarLevel)
{
    Root root;
    LEFReader reader;
    EXPECT_EQ(reader.read_lef(fixture_path("rect_without_layer.lef"), root, "test_lib"), 2);
}

TEST(LEFReaderErrors, DuplicateLayerNameIsIgnored)
{
    // Second `LAYER M1` (DIRECTION VERTICAL) must be dropped, keeping the
    // first (DIRECTION HORIZONTAL) - not overwritten, not duplicated.
    Root root;
    LEFReader reader;
    ASSERT_EQ(reader.read_lef(fixture_path("duplicate_layer.lef"), root, "test_lib"), 0);

    EXPECT_EQ(root.get_layer_ids().size(), 1u);
    LayerId m1_id = root.get_layer_by_name("M1");
    ASSERT_TRUE(m1_id.valid());
    EXPECT_EQ(root.get_layer(m1_id)->direction, RoutingDirection::H);

    // log_warning (UPDATES.md item 3) - internal diagnostics, not just
    // the vendored parser's own, reach messages() too.
    ASSERT_FALSE(reader.messages().empty());
    EXPECT_NE(reader.messages().front().find("WARNING"), std::string::npos);
    EXPECT_NE(reader.messages().front().find("M1"), std::string::npos);
}

TEST(LEFReaderErrors, PinWithoutDirectionDoesNotInheritThePreviousPinsDirection)
{
    // Regression: the vendored parser reuses one scratch lefiPin across
    // every PIN statement and never resets direction_ between them
    // (same hazard as lefiLayer's direction_) - PIN B declares no
    // DIRECTION at all and must read back NONE, not silently inherit
    // PIN A's DIRECTION INPUT.
    Root root;
    LEFReader reader;
    ASSERT_EQ(reader.read_lef(fixture_path("pin_without_direction.lef"), root, "test_lib"), 0);

    ASSERT_EQ(root.get_design_size(), 1u);
    AbstractId abstract_id = root.get_design_abstract(root.get_design_ids().front());

    TerminalId pin_a_id = find_terminal_id(root, abstract_id, "A");
    ASSERT_TRUE(pin_a_id.valid());
    EXPECT_EQ(root.get_terminal(pin_a_id)->direction, SignalDirection::INPUT);

    TerminalId pin_b_id = find_terminal_id(root, abstract_id, "B");
    ASSERT_TRUE(pin_b_id.valid());
    EXPECT_EQ(root.get_terminal(pin_b_id)->direction, SignalDirection::NONE);
}

TEST(LEFReaderErrors, SecondReadWithDifferentUnitsIsIgnored)
{
    // Reading a second LEF into the same Root with a different DATABASE
    // MICRONS must warn and keep the technology's original value.
    Root root;
    LEFReader reader1, reader2;
    ASSERT_EQ(reader1.read_lef(fixture_path("units_1000.lef"), root, "test_lib"), 0);
    ASSERT_EQ(reader2.read_lef(fixture_path("units_2000.lef"), root, "test_lib"), 0);

    ASSERT_EQ(root.get_technology_size(), 1u);
    EXPECT_DOUBLE_EQ(root.get_technology(root.get_technology_ids().front())->database_units_microns, 1000.0);

    // The warning fires on the second read (reader2) - log_warning
    // reaches messages() there, not on the first, unaffected read.
    ASSERT_FALSE(reader2.messages().empty());
    EXPECT_NE(reader2.messages().front().find("WARNING"), std::string::npos);
    EXPECT_TRUE(reader1.messages().empty());
}

TEST(LEFReaderErrors, GeometryBeforeDatabaseMicronsEverDeclaredIsAnError)
{
    // Every microns_to_dbu()/microns_squared_to_dbu() call silently
    // multiplies by 0 (the "unset" sentinel database_units_microns
    // defaults to) if DATABASE MICRONS was never declared - previously
    // undetected, producing silently-wrong all-zero coordinates. Caught
    // once at the end of read_lef (not mid-parse) via
    // used_dbu_before_units_declared_.
    Root root;
    LEFReader reader;
    const int result = reader.read_lef(fixture_path("no_units_with_geometry.lef"), root, "test_lib");
    EXPECT_EQ(result, 3);

    ASSERT_FALSE(reader.messages().empty());
    EXPECT_NE(reader.messages().back().find("ERROR"), std::string::npos);
    EXPECT_NE(reader.messages().back().find("DATABASE MICRONS"), std::string::npos);
}

TEST(LEFReaderErrors, GeometryAfterAnEarlierReadAlreadyDeclaredDatabaseMicronsIsFine)
{
    // The same no-UNITS macro file, but read into a Root that already has
    // DATABASE MICRONS from an earlier read (the realistic "tech file
    // first, macro file second" pattern) - not an error, since
    // database_units_microns is no longer the unset sentinel by the time
    // this file's own geometry gets converted.
    Root root;
    LEFReader tech_reader;
    ASSERT_EQ(tech_reader.read_lef(fixture_path("units_1000.lef"), root, "test_lib"), 0);

    LEFReader macro_reader;
    EXPECT_EQ(macro_reader.read_lef(fixture_path("no_units_with_geometry.lef"), root, "test_lib"), 0);
}

TEST(LEFReaderErrors, DuplicateMacroNameIsRejected)
{
    // The second `MACRO DUPTEST` must be rejected (an abstract already
    // exists for that design name); only the first's Design/Abstract stick.
    Root root;
    LEFReader reader;
    EXPECT_EQ(reader.read_lef(fixture_path("duplicate_macro.lef"), root, "test_lib"), 2);
    EXPECT_EQ(root.get_design_ids().size(), 1u);

    // log_error (UPDATES.md item 3) - this failure comes from our own
    // lefrMacroBeginCbkFn, not the vendored parser's own log callback,
    // and still reaches messages().
    ASSERT_FALSE(reader.messages().empty());
    EXPECT_NE(reader.messages().front().find("ERROR"), std::string::npos);
    EXPECT_NE(reader.messages().front().find("DUPTEST"), std::string::npos);
}
