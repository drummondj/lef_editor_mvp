#include "../lef_reader.hpp"
#include <gtest/gtest.h>

using namespace le;

// LEFReader::*_from_parser are pure, static, and take no parser/instance
// state - exercised directly here rather than only incidentally through
// src/lefdef/lef/TEST/complete.5.8.lef (lef_reader_test.cpp), since the
// fixture doesn't hit every branch (e.g. FS/FE/FW orientations, INOUT's
// FEEDTHRU/"OUTPUT TRISTATE" aliases, unknown/default fallbacks).

TEST(OrientationFromParser, MapsEachLefOrientationCode)
{
    // This is lef.y's `orientation` rule (R0=N, R90=W, R180=S, R270=E,
    // MY=FN, MYR90=FW, MX=FS, MXR90=FE), confirmed against the vendored
    // grammar - NOT the naive 0=N,1=S,2=E,3=W ordering it's easy to assume.
    // Also does not match le::Orientation's own underlying enum values (see
    // orientation.hpp), so the mapping must be checked by name, not by
    // casting the int.
    EXPECT_EQ(LEFReader::orientation_from_parser(0), Orientation::N);
    EXPECT_EQ(LEFReader::orientation_from_parser(1), Orientation::W);
    EXPECT_EQ(LEFReader::orientation_from_parser(2), Orientation::S);
    EXPECT_EQ(LEFReader::orientation_from_parser(3), Orientation::E);
    EXPECT_EQ(LEFReader::orientation_from_parser(4), Orientation::FN);
    EXPECT_EQ(LEFReader::orientation_from_parser(5), Orientation::FW);
    EXPECT_EQ(LEFReader::orientation_from_parser(6), Orientation::FS);
    EXPECT_EQ(LEFReader::orientation_from_parser(7), Orientation::FE);
}

TEST(OrientationFromParser, UnknownCodeDefaultsToNorth)
{
    EXPECT_EQ(LEFReader::orientation_from_parser(-1), Orientation::N);
    EXPECT_EQ(LEFReader::orientation_from_parser(8), Orientation::N);
    EXPECT_EQ(LEFReader::orientation_from_parser(100), Orientation::N);
}

TEST(RoutingDirectionFromParser, MapsKnownStrings)
{
    EXPECT_EQ(LEFReader::routing_direction_from_parser("HORIZONTAL"), RoutingDirection::H);
    EXPECT_EQ(LEFReader::routing_direction_from_parser("VERTICAL"), RoutingDirection::V);
}

TEST(RoutingDirectionFromParser, NullUnknownOrWrongCaseIsNone)
{
    EXPECT_EQ(LEFReader::routing_direction_from_parser(nullptr), RoutingDirection::NONE);
    EXPECT_EQ(LEFReader::routing_direction_from_parser(""), RoutingDirection::NONE);
    EXPECT_EQ(LEFReader::routing_direction_from_parser("DIAGONAL"), RoutingDirection::NONE);
    // The matches are case-sensitive; a lowercase LEF value (which the
    // grammar shouldn't produce, but the parser doesn't normalize either)
    // falls through to NONE rather than matching HORIZONTAL.
    EXPECT_EQ(LEFReader::routing_direction_from_parser("horizontal"), RoutingDirection::NONE);
}

TEST(SignalDirectionFromParser, MapsKnownStrings)
{
    EXPECT_EQ(LEFReader::signal_direction_from_parser("INPUT"), SignalDirection::INPUT);
    EXPECT_EQ(LEFReader::signal_direction_from_parser("OUTPUT"), SignalDirection::OUTPUT);
    EXPECT_EQ(LEFReader::signal_direction_from_parser("INOUT"), SignalDirection::INOUT);
}

TEST(SignalDirectionFromParser, FeedthruAndOutputTristateAliasToInout)
{
    EXPECT_EQ(LEFReader::signal_direction_from_parser("FEEDTHRU"), SignalDirection::INOUT);
    EXPECT_EQ(LEFReader::signal_direction_from_parser("OUTPUT TRISTATE"), SignalDirection::INOUT);
}

TEST(SignalDirectionFromParser, NullOrUnknownIsNone)
{
    EXPECT_EQ(LEFReader::signal_direction_from_parser(nullptr), SignalDirection::NONE);
    EXPECT_EQ(LEFReader::signal_direction_from_parser(""), SignalDirection::NONE);
    EXPECT_EQ(LEFReader::signal_direction_from_parser("BOGUS"), SignalDirection::NONE);
}
