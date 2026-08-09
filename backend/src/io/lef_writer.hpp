#pragma once
#include <string>
#include <vector>
#include "../database/database.hpp"

namespace le
{
    /// @brief Writes a LEF file for a given AbstractId, using the vendored
    /// procedural writer API (lefwWriter.h/.hpp - lefwInit(FILE*) then
    /// direct lefw* calls, matching the vendored sample driver
    /// src/lefdef/lef/lefwrite/lefwrite.cpp, not the alternate callback-
    /// registration lefwWriterCalls.h API).
    ///
    /// UPDATES.md item 12 Phase 1 scope: UNITS (database_units_microns
    /// only), LAYER (the basic scalar properties LEFReader reads - type,
    /// direction, width, pitch, offset, area, spacing, resistance,
    /// capacitance, height, thickness, wire_extension, shrinkage,
    /// cap_multiplier, edge_cap, antenna_area, antenna_length - see
    /// lef_reader.cpp's lefrLayerCbkFn for the exact set and why it stops
    /// there), and MACRO/PIN/PORT/OBS (class, foreigns, size, origin,
    /// symmetry, site, geometry including RECT/PATH/POLYGON ITERATE).
    /// VIA/VIARULE/SITE/NONDEFAULTRULE/PROPERTYDEFINITIONS/SPACING/ARRAY
    /// and the electrical/misc sections are out of scope for this phase -
    /// see the phase's own plan for the follow-up ordering.
    class LEFWriter
    {
    public:
        /// @brief What to write about the Technology's layers, alongside
        /// (or instead of) `abstract_id`'s own MACRO content - mirrors
        /// UPDATES.md item 12 step 1's own wording ("an option to choose
        /// whether to include Technology layers or not, or just write out
        /// Technology layers").
        enum class LayerWriteMode
        {
            /// Don't write any LAYER statements - just `abstract_id`'s MACRO.
            None,
            /// Write every layer in the Root's (first) Technology, then
            /// `abstract_id`'s own MACRO.
            IncludeWithAbstract,
            /// Write only the Technology's layers - no MACRO at all
            /// (`abstract_id` is ignored in this mode).
            TechnologyOnly,
        };

        /// @brief Writes `path`, returning 0 on success (matches
        /// LEFReader::read_lef's own convention) or a nonzero lefw* error
        /// code (or 1 for a local failure, e.g. the file couldn't be
        /// opened) otherwise. An unknown/invalid `abstract_id` (with a
        /// LayerWriteMode that would write a MACRO) writes just the
        /// Technology layers (if requested) and no MACRO, rather than
        /// failing - Root's own lookups already degrade gracefully for
        /// that, matching this codebase's stated convention.
        int write_lef(const std::string &path, const Root &root, AbstractId abstract_id, LayerWriteMode mode);

        // Messages produced by the most recent write_lef() call - this
        // class's own synthesized error messages (e.g. "could not open
        // file"), not vendored-parser output (the lefw* writer API has no
        // log-function hook the way the lefr* reader API does). Cleared
        // and repopulated at the start of every write_lef() call.
        const std::vector<std::string> &messages() const { return messages_; }

    private:
        static int write_technology_layers(const Root &root, TechnologyId technology_id);
        static int write_units(const Root &root, TechnologyId technology_id);
        static int write_macro(const Root &root, AbstractId abstract_id, double dbu_per_micron);
        static int write_terminal(const Root &root, TerminalId terminal_id, double dbu_per_micron);
        static int write_obstruction(const Root &root, ObstructionId obstruction_id, double dbu_per_micron);
        static int write_shape_geometry(const Shape &shape, double dbu_per_micron, bool is_pin_port);

        std::vector<std::string> messages_;
    };
}
