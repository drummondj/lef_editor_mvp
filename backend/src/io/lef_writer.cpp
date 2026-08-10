#include "lef_writer.hpp"
#include "../lefdef/lef/include/lefwWriter.hpp"
#include <fmt/format.h>
#include <memory>
#include <cstdio>
#include <cctype>

// The vendored library's C++ writer header (lefwWriter.hpp) wraps every
// lefw* declaration in `namespace LefDefParser` - this codebase's own
// lef_reader.cpp resolves the equivalent reader-side names the same way
// (see its own include of lefrReader.hpp). Using the plain C header
// (lefwWriter.h) instead would pull in lefiTypedefs.h's unconditional
// `#define bool int`, which corrupts any C++ header included afterward -
// confirmed the hard way: it broke fmt/format.h's own template code.
USE_LEFDEF_PARSER_NAMESPACE

namespace
{
    // Inverse of LEFReader::microns_to_dbu (see lef_reader.cpp) - dbu was
    // itself std::llround(microns * dbu_per_micron), so this can't recover
    // the exact original decimal in every case (round-trip through a
    // rounded integer), but matches to within the same dbu-grid precision
    // every other coordinate conversion in this codebase already accepts.
    double to_microns(int64_t dbu, double dbu_per_micron)
    {
        return dbu_per_micron > 0.0 ? static_cast<double>(dbu) / dbu_per_micron : 0.0;
    }

    // Inverse of LEFReader::microns_squared_to_dbu - area (LEF AREA/
    // ANTENNAAREA) scales as the *square* of dbu_per_micron, unlike every
    // other to_microns() call site here.
    double to_microns_squared(int64_t dbu_squared, double dbu_per_micron)
    {
        return dbu_per_micron > 0.0 ? static_cast<double>(dbu_squared) / (dbu_per_micron * dbu_per_micron) : 0.0;
    }

    // lefiProp::propType()'s own setPropType() calls (see lef.y's
    // PROPERTYDEFINITIONS grammar) store the owner keyword lowercase
    // ("layer", "via", ...), but lefwIntPropDef/RealPropDef/StringPropDef
    // validate objType against the uppercase LEF keywords ("LAYER", "VIA",
    // ...) and reject anything else with LEFW_BAD_DATA.
    std::string to_upper(std::string s)
    {
        for (char &c : s)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return s;
    }

    const char *routing_direction_to_string(le::RoutingDirection direction)
    {
        // NONE falls back to HORIZONTAL - lefwLayerRouting's direction
        // argument is required (LEF's own ROUTING statement always has
        // one), unlike LEFReader's own read side where DIRECTION is
        // optional per-layer (see lefrLayerCbkFn's own has*() guard).
        switch (direction)
        {
        case le::RoutingDirection::V:
            return "VERTICAL";
        case le::RoutingDirection::DIAG45:
            return "DIAG45";
        case le::RoutingDirection::DIAG135:
            return "DIAG135";
        default:
            return "HORIZONTAL";
        }
    }

    const char *signal_direction_to_string(le::SignalDirection direction)
    {
        switch (direction)
        {
        case le::SignalDirection::INPUT:
            return "INPUT";
        case le::SignalDirection::OUTPUT:
            return "OUTPUT";
        case le::SignalDirection::INOUT:
            return "INOUT";
        case le::SignalDirection::OUTPUT_TRISTATE:
            return "OUTPUT TRISTATE";
        case le::SignalDirection::FEEDTHRU:
            return "FEEDTHRU";
        default:
            return nullptr; // NONE - DIRECTION is optional, matches read side
        }
    }

    const char *orientation_to_string(le::Orientation orient)
    {
        // The enum's own member names already are the LEF orient strings
        // (N/S/E/W/FN/FS/FE/FW) - no lookup table needed, unlike the
        // int-code form LEFReader::orientation_from_parser has to decode.
        switch (orient)
        {
        case le::Orientation::N:
            return "N";
        case le::Orientation::S:
            return "S";
        case le::Orientation::E:
            return "E";
        case le::Orientation::W:
            return "W";
        case le::Orientation::FN:
            return "FN";
        case le::Orientation::FS:
            return "FS";
        case le::Orientation::FE:
            return "FE";
        case le::Orientation::FW:
            return "FW";
        default:
            return "N";
        }
    }
}

namespace le
{
    int LEFWriter::write_property_definitions(const Root &root, TechnologyId technology_id)
    {
        const TechnologyData *technology = root.get_technology(technology_id);
        if (!technology || technology->property_definitions.empty())
            return 0;

        int status = lefwStartPropDef();
        if (status)
            return status;

        for (const PropertyDefinition &def : technology->property_definitions)
        {
            const double left = def.range_min.value_or(0.0);
            const double right = def.range_max.value_or(0.0);
            const std::string owner_type = to_upper(def.owner_type);
            // 'I'nteger/'R'eal/'S'tring/'Q'uoted-string - Q is written the
            // same way as S (lefwStringPropDef has no separate "quoted"
            // form; the writer always quotes string values regardless -
            // see lefwStringProperty's own "%s \"%s\"" format).
            if (def.data_type == "I")
                status = lefwIntPropDef(owner_type.c_str(), def.name.c_str(), left, right, 0);
            else if (def.data_type == "R")
                status = lefwRealPropDef(owner_type.c_str(), def.name.c_str(), left, right, 0.0);
            else
                status = lefwStringPropDef(owner_type.c_str(), def.name.c_str(), left, right, nullptr);
            if (status)
                return status;
        }

        return lefwEndPropDef();
    }

    int LEFWriter::write_properties(const std::vector<LefProperty> &properties, bool include_numeric)
    {
        for (const LefProperty &property : properties)
        {
            if (property.is_number && !include_numeric)
                continue;
            const int status = property.is_number
                                    ? lefwRealProperty(property.name.c_str(), property.number_value)
                                    : lefwStringProperty(property.name.c_str(), property.string_value.c_str());
            if (status)
                return status;
        }
        return 0;
    }

    namespace
    {
        int write_antenna_pwl(int (*writer)(int, double *, double *), const std::vector<AntennaPWLEntry> &entries)
        {
            std::vector<double> diffusions;
            std::vector<double> ratios;
            diffusions.reserve(entries.size());
            ratios.reserve(entries.size());
            for (const AntennaPWLEntry &entry : entries)
            {
                diffusions.push_back(entry.diffusion);
                ratios.push_back(entry.ratio);
            }
            return writer(static_cast<int>(diffusions.size()), diffusions.data(), ratios.data());
        }
    }

    int LEFWriter::write_layer_antenna_models(const std::vector<AntennaModel> &models, bool is_cut)
    {
        for (const AntennaModel &model : models)
        {
            int status = lefwLayerAntennaModel(model.oxide.c_str());
            if (status)
                return status;

            if (model.area_ratio)
            {
                status = lefwLayerAntennaAreaRatio(*model.area_ratio);
                if (status)
                    return status;
            }
            if (model.cum_area_ratio)
            {
                status = lefwLayerAntennaCumAreaRatio(*model.cum_area_ratio);
                if (status)
                    return status;
            }
            if (model.area_factor)
            {
                status = lefwLayerAntennaAreaFactor(*model.area_factor, model.area_factor_diffuse_only ? "DIFFUSEONLY" : nullptr);
                if (status)
                    return status;
            }

            // KNOWN VENDORED-LIBRARY GAP: every "SideArea"-family writer
            // (lefwLayerAntennaSideAreaRatio/DiffSideAreaRatio(Pwl)/
            // CumSideAreaRatio/CumDiffSideAreaRatio(Pwl)/SideAreaFactor)
            // checks `!lefwIsRouting` and rejects CUT layers outright
            // (LEFW_BAD_DATA) - unlike AntennaModel/AreaRatio/DiffAreaRatio/
            // CumAreaRatio/AreaFactor above, which all accept
            // `!lefwIsRouting && !lefwIsCut` (confirmed by reading
            // lefwWriter.cpp's own state checks function by function). Some
            // of these functions' own doc comments in lefwWriter.hpp even
            // claim "valid... if the layer type is either ROUTING or CUT",
            // contradicting their actual .cpp implementation. lefiAntennaModel
            // has no such restriction on the read side - a CUT layer's
            // ANTENNASIDEAREARATIO etc. reads fine (lefrLayerCbkFn) - so
            // these fields are readable but not re-writable on a CUT layer
            // with this vendored writer version. Not fixed here (vendored
            // code, per CLAUDE.md's "never edit src/lefdef" rule).
            if (!is_cut)
            {
                if (model.side_area_ratio)
                {
                    status = lefwLayerAntennaSideAreaRatio(*model.side_area_ratio);
                    if (status)
                        return status;
                }
                if (model.cum_side_area_ratio)
                {
                    status = lefwLayerAntennaCumSideAreaRatio(*model.cum_side_area_ratio);
                    if (status)
                        return status;
                }
                if (model.side_area_factor)
                {
                    status = lefwLayerAntennaSideAreaFactor(*model.side_area_factor, model.side_area_factor_diffuse_only ? "DIFFUSEONLY" : nullptr);
                    if (status)
                        return status;
                }
            }

            // Each of these four is EITHER the scalar OR the pwl list,
            // never both (matching lefrLayerCbkFn's own reading of
            // lefiAntennaModel's mutually-exclusive has* flags).
            if (!model.diff_area_ratio_pwl.empty())
                status = write_antenna_pwl(lefwLayerAntennaDiffAreaRatioPwl, model.diff_area_ratio_pwl);
            else if (model.diff_area_ratio)
                status = lefwLayerAntennaDiffAreaRatio(*model.diff_area_ratio);
            else
                status = 0;
            if (status)
                return status;

            if (!model.cum_diff_area_ratio_pwl.empty())
                status = write_antenna_pwl(lefwLayerAntennaCumDiffAreaRatioPwl, model.cum_diff_area_ratio_pwl);
            else if (model.cum_diff_area_ratio)
                status = lefwLayerAntennaCumDiffAreaRatio(*model.cum_diff_area_ratio);
            else
                status = 0;
            if (status)
                return status;

            if (!is_cut)
            {
                if (!model.diff_side_area_ratio_pwl.empty())
                    status = write_antenna_pwl(lefwLayerAntennaDiffSideAreaRatioPwl, model.diff_side_area_ratio_pwl);
                else if (model.diff_side_area_ratio)
                    status = lefwLayerAntennaDiffSideAreaRatio(*model.diff_side_area_ratio);
                else
                    status = 0;
                if (status)
                    return status;

                if (!model.cum_diff_side_area_ratio_pwl.empty())
                    status = write_antenna_pwl(lefwLayerAntennaCumDiffSideAreaRatioPwl, model.cum_diff_side_area_ratio_pwl);
                else if (model.cum_diff_side_area_ratio)
                    status = lefwLayerAntennaCumDiffSideAreaRatio(*model.cum_diff_side_area_ratio);
                else
                    status = 0;
                if (status)
                    return status;
            }
        }
        return 0;
    }

    int LEFWriter::write_pin_antenna_values(const std::vector<PinAntennaValue> &values, int (*writer)(double, const char *))
    {
        for (const PinAntennaValue &entry : values)
        {
            const int status = writer(entry.value, entry.layer_name.empty() ? nullptr : entry.layer_name.c_str());
            if (status)
                return status;
        }
        return 0;
    }

    int LEFWriter::write_layer_current_density(
        const std::vector<LayerDensityEntry> &entries,
        int (*current_density)(const char *, double),
        int (*frequency)(int, double *),
        int (*width)(int, double *),
        int (*cutarea)(int, double *),
        int (*table_entries)(int, double *),
        double dbu_per_micron)
    {
        for (const LayerDensityEntry &entry : entries)
        {
            // KNOWN VENDORED-WRITER EDGE CASE: lefwLayerACCurrentDensity/
            // DCCurrentDensity dispatch on `if (value)` - a real one_entry
            // value of exactly 0.0 would be misread as "open table form"
            // with no closing TableEntries call, producing an invalid
            // file. Not a concern for any value seen in complete.5.8.lef;
            // not worked around here (vendored code).
            if (entry.one_entry)
            {
                int status = current_density(entry.type.c_str(), *entry.one_entry);
                if (status)
                    return status;
                continue;
            }

            int status = current_density(entry.type.c_str(), 0.0);
            if (status)
                return status;

            if (frequency && !entry.frequency.empty())
            {
                std::vector<double> frequency_hz = entry.frequency;
                status = frequency(static_cast<int>(frequency_hz.size()), frequency_hz.data());
                if (status)
                    return status;
            }
            if (!entry.width.empty())
            {
                std::vector<double> width_um;
                width_um.reserve(entry.width.size());
                for (int64_t w : entry.width)
                    width_um.push_back(to_microns(w, dbu_per_micron));
                status = width(static_cast<int>(width_um.size()), width_um.data());
                if (status)
                    return status;
            }
            if (!entry.cutarea.empty())
            {
                std::vector<double> cutarea_um2;
                cutarea_um2.reserve(entry.cutarea.size());
                for (int64_t c : entry.cutarea)
                    cutarea_um2.push_back(to_microns_squared(c, dbu_per_micron));
                status = cutarea(static_cast<int>(cutarea_um2.size()), cutarea_um2.data());
                if (status)
                    return status;
            }

            // Required to close the table form.
            std::vector<double> table_values = entry.table_entries;
            status = table_entries(static_cast<int>(table_values.size()), table_values.data());
            if (status)
                return status;
        }
        return 0;
    }

    int LEFWriter::write_units(const Root &root, TechnologyId technology_id)
    {
        const TechnologyData *technology = root.get_technology(technology_id);
        if (!technology || technology->database_units_microns <= 0.0)
            return 0;

        int status = lefwStartUnits();
        if (status)
            return status;

        // time/capacitance/resistance/power/current/voltage are all 0
        // (falsy) - the vendored writer skips writing a sub-statement for
        // any falsy argument (confirmed in lefwWriter.cpp), so only
        // DATABASE MICRONS gets written, matching what LEFReader actually
        // reads today (UPDATES.md 12 Phase 1 scope).
        status = lefwUnits(0, 0, 0, 0, 0, 0, technology->database_units_microns);
        if (status)
            return status;

        return lefwEndUnits();
    }

    int LEFWriter::write_technology_layers(const Root &root, TechnologyId technology_id)
    {
        const TechnologyData *technology = root.get_technology(technology_id);
        if (!technology)
            return 0;
        const double dbu_per_micron = technology->database_units_microns;

        for (LayerId layer_id : root.get_technology_layers(technology_id))
        {
            const LayerData *layer = root.get_layer(layer_id);
            if (!layer)
                continue;

            const bool is_routing = layer->type == "ROUTING";
            int status;

            if (is_routing)
            {
                status = lefwStartLayerRouting(layer->name.c_str());
                if (status)
                    return status;

                // lefwLayerMask only accepts LEFW_LAYERROUTING_START (not
                // LEFW_LAYERROUTING) - must be written before the
                // lefwLayerRouting() call below transitions state away
                // from _START (confirmed in lefwWriter.cpp; matches the
                // fixture's own MASK-right-after-TYPE ordering).
                if (layer->default_mask)
                {
                    status = lefwLayerMask(*layer->default_mask);
                    if (status)
                        return status;
                }

                status = lefwLayerRouting(routing_direction_to_string(layer->direction), layer->width ? to_microns(*layer->width, dbu_per_micron) : 0.0);
                if (status)
                    return status;

                // pitch_xy/offset_xy (two-value form) are mutually
                // exclusive with pitch/offset (single-value form).
                //
                // KNOWN VENDORED-WRITER GAP: pitch_xy/offset_xy/diag_pitch/
                // diag_pitch_xy/diag_spacing/diag_width all require
                // lefwIsRouting (confirmed in lefwWriter.cpp) - unwritable
                // on a CUT layer even though the reader can populate them
                // there (e.g. complete.5.8.lef's LAYER CUT01, TYPE CUT,
                // uses DIAGPITCH/two-value PITCH/two-value OFFSET). Still
                // fully read; just never re-written for a CUT layer.
                if (layer->pitch_xy)
                {
                    status = lefwLayerRoutingPitchXYDistance(to_microns(layer->pitch_xy->x, dbu_per_micron), to_microns(layer->pitch_xy->y, dbu_per_micron));
                    if (status)
                        return status;
                }
                else if (layer->pitch)
                {
                    status = lefwLayerRoutingPitch(to_microns(*layer->pitch, dbu_per_micron));
                    if (status)
                        return status;
                }
                if (layer->offset_xy)
                {
                    status = lefwLayerRoutingOffsetXYDistance(to_microns(layer->offset_xy->x, dbu_per_micron), to_microns(layer->offset_xy->y, dbu_per_micron));
                    if (status)
                        return status;
                }
                else if (layer->offset)
                {
                    status = lefwLayerRoutingOffset(to_microns(*layer->offset, dbu_per_micron));
                    if (status)
                        return status;
                }
                if (layer->area)
                {
                    status = lefwLayerRoutingArea(to_microns_squared(*layer->area, dbu_per_micron));
                    if (status)
                        return status;
                }
                if (layer->diag_pitch_xy)
                {
                    status = lefwLayerRoutingDiagPitchXYDistance(to_microns(layer->diag_pitch_xy->x, dbu_per_micron), to_microns(layer->diag_pitch_xy->y, dbu_per_micron));
                    if (status)
                        return status;
                }
                else if (layer->diag_pitch)
                {
                    status = lefwLayerRoutingDiagPitch(to_microns(*layer->diag_pitch, dbu_per_micron));
                    if (status)
                        return status;
                }
                if (layer->diag_spacing)
                {
                    status = lefwLayerRoutingDiagSpacing(to_microns(*layer->diag_spacing, dbu_per_micron));
                    if (status)
                        return status;
                }
                if (layer->diag_width)
                {
                    status = lefwLayerRoutingDiagWidth(to_microns(*layer->diag_width, dbu_per_micron));
                    if (status)
                        return status;
                }
                if (layer->diag_min_edge_length)
                {
                    status = lefwLayerRoutingDiagMinEdgeLength(to_microns(*layer->diag_min_edge_length, dbu_per_micron));
                    if (status)
                        return status;
                }
                for (const SpacingRule &rule : layer->spacing_rules)
                {
                    status = lefwLayerRoutingSpacing(to_microns(rule.distance, dbu_per_micron));
                    if (status)
                        return status;

                    // rule.center_to_center is deliberately never written
                    // here: lefwWriter.hpp's own comments list
                    // lefwLayerSpacingCenterToCenter (the ROUTING-layer
                    // CENTERTOCENTER writer) as "obsoleted in 5.7" with no
                    // replacement - only lefwLayerCutSpacingCenterToCenter
                    // (CUT layers, used below) still exists. Still fully
                    // read (lefrLayerCbkFn), just unwritable for ROUTING
                    // via this vendored writer version.

                    // At most one of RANGE/LENGTHTHRESHOLD/SAMENET follows a
                    // ROUTING SPACING statement (lefwWriter.hpp's own "either
                    // this routine ... or ..." comments on each).
                    if (rule.range_min && rule.range_max)
                    {
                        status = lefwLayerRoutingSpacingRange(to_microns(*rule.range_min, dbu_per_micron), to_microns(*rule.range_max, dbu_per_micron));
                        if (status)
                            return status;

                        if (rule.range_use_length_threshold)
                        {
                            status = lefwLayerRoutingSpacingRangeUseLengthThreshold();
                            if (status)
                                return status;
                        }
                        else if (rule.range_influence)
                        {
                            status = lefwLayerRoutingSpacingRangeInfluence(to_microns(*rule.range_influence, dbu_per_micron),
                                                                            rule.range_influence_range_min ? to_microns(*rule.range_influence_range_min, dbu_per_micron) : 0.0,
                                                                            rule.range_influence_range_max ? to_microns(*rule.range_influence_range_max, dbu_per_micron) : 0.0);
                            if (status)
                                return status;
                        }
                        else if (rule.range_range_min && rule.range_range_max)
                        {
                            status = lefwLayerRoutingSpacingRangeRange(to_microns(*rule.range_range_min, dbu_per_micron), to_microns(*rule.range_range_max, dbu_per_micron));
                            if (status)
                                return status;
                        }
                    }
                    else if (rule.length_threshold)
                    {
                        status = lefwLayerRoutingSpacingLengthThreshold(to_microns(*rule.length_threshold, dbu_per_micron), 0.0, 0.0);
                        if (status)
                            return status;
                    }
                    else if (rule.same_net)
                    {
                        status = lefwLayerRoutingSpacingSameNet(rule.same_net_pg_only ? 1 : 0);
                        if (status)
                            return status;
                    }

                    // KNOWN VENDORED-LIBRARY BUG: lefwLayerRoutingSpacingEndOfLine
                    // unconditionally flushes (";\n") whatever SPACING
                    // statement is still open *before* writing "ENDOFLINE
                    // ...", producing an orphaned top-level "ENDOFLINE ..."
                    // statement - but lef.y's own grammar only ever
                    // accepts K_ENDOFLINE nested inside a SPACING
                    // statement's own layer_spacing_cut_routing option (one
                    // grammar occurrence, confirmed by grep), so the
                    // written file is unparseable on re-read. Not fixed
                    // (vendored code). ENDOFLINE/PARALLELEDGE/TWOEDGES are
                    // still fully read (see lefrLayerCbkFn) - just never
                    // re-written.

                    // SAME BUG, same root cause: lefwLayerRoutingSpacingNotchLength/
                    // SpacingEndOfNotchWidth also flush the open SPACING
                    // statement and emit NOTCHLENGTH/ENDOFNOTCHWIDTH as a
                    // separate top-level statement, but lef.y only accepts
                    // them nested inside SPACING's own option grammar - not
                    // called here either. rule.notch_length/end_of_notch_*
                    // are read-only.
                }

                for (const MinimumCut &cut : layer->minimum_cuts)
                {
                    if (cut.within)
                    {
                        status = lefwLayerRoutingMinimumcutWithin(cut.cuts, to_microns(cut.width, dbu_per_micron), to_microns(*cut.within, dbu_per_micron));
                        if (status)
                            return status;
                    }
                    else
                    {
                        status = lefwLayerRoutingMinimumcut(cut.cuts, to_microns(cut.width, dbu_per_micron));
                        if (status)
                            return status;
                    }

                    if (!cut.connection.empty())
                    {
                        status = lefwLayerRoutingMinimumcutConnections(cut.connection.c_str());
                        if (status)
                            return status;
                    }
                    if (cut.length && cut.distance)
                    {
                        status = lefwLayerRoutingMinimumcutLengthWithin(to_microns(*cut.length, dbu_per_micron), to_microns(*cut.distance, dbu_per_micron));
                        if (status)
                            return status;
                    }
                }

                for (const MinStep &step : layer->min_steps)
                {
                    if (step.max_edges)
                    {
                        status = lefwLayerRoutingMinstepMaxEdges(to_microns(step.distance, dbu_per_micron), *step.max_edges);
                        if (status)
                            return status;
                    }
                    else if (!step.min_step_type.empty() || step.lengthsum)
                    {
                        status = lefwLayerRoutingMinstepWithOptions(to_microns(step.distance, dbu_per_micron), step.min_step_type.empty() ? nullptr : step.min_step_type.c_str(), step.lengthsum ? to_microns(*step.lengthsum, dbu_per_micron) : 0.0);
                        if (status)
                            return status;
                    }
                    else
                    {
                        status = lefwLayerRoutingMinstep(to_microns(step.distance, dbu_per_micron));
                        if (status)
                            return status;
                    }
                }

                if (layer->spacing_table_parallel_run_length)
                {
                    const ParallelRunLengthSpacingTable &table = *layer->spacing_table_parallel_run_length;
                    std::vector<double> lengths_um;
                    lengths_um.reserve(table.lengths.size());
                    for (int64_t length : table.lengths)
                        lengths_um.push_back(to_microns(length, dbu_per_micron));

                    status = lefwLayerRoutingStartSpacingtableParallel(static_cast<int>(lengths_um.size()), lengths_um.data());
                    if (status)
                        return status;

                    const size_t num_lengths = table.lengths.size();
                    for (size_t w = 0; w < table.widths.size(); w++)
                    {
                        std::vector<double> row_um;
                        row_um.reserve(num_lengths);
                        for (size_t l = 0; l < num_lengths; l++)
                            row_um.push_back(to_microns(table.spacings[w * num_lengths + l], dbu_per_micron));

                        status = lefwLayerRoutingSpacingtableParallelWidth(to_microns(table.widths[w], dbu_per_micron), static_cast<int>(row_um.size()), row_um.data());
                        if (status)
                            return status;
                    }

                    // lefwLayerRoutineEndSpacingtable (sic - the vendored
                    // header really does spell it "Routine", not "Routing")
                    // is the only call that resets lefwState from
                    // LEFW_LAYERROUTINGWIDTH (where the last
                    // SpacingtableParallelWidth call above leaves it) back
                    // to LEFW_LAYERROUTING - every other layer-routing
                    // writer function, including lefwEndLayerRouting
                    // itself, rejects LEFW_LAYERROUTINGWIDTH outright, so
                    // skipping this call would silently break every
                    // statement written after a SPACINGTABLE (found via
                    // lefwWriter.cpp's own state-constant checks, then
                    // confirmed against the vendored sample driver
                    // src/lefdef/lef/lefwrite/lefwrite.cpp's own usage,
                    // which is the only place this misspelled function name
                    // turns up).
                    status = lefwLayerRoutineEndSpacingtable();
                    if (status)
                        return status;
                }

                if (!layer->spacing_table_influence.empty())
                {
                    status = lefwLayerRoutingStartSpacingtableInfluence();
                    if (status)
                        return status;

                    for (const InfluenceSpacingEntry &entry : layer->spacing_table_influence)
                    {
                        status = lefwLayerRoutingSpacingInfluenceWidth(to_microns(entry.width, dbu_per_micron), to_microns(entry.distance, dbu_per_micron), to_microns(entry.spacing, dbu_per_micron));
                        if (status)
                            return status;
                    }

                    status = lefwLayerRoutineEndSpacingtable();
                    if (status)
                        return status;
                }

                if (!layer->spacing_table_two_widths.empty())
                {
                    // lefwLayerRoutingStartSpacingtableTwoWidths is
                    // ROUTING-only (LEFW_LAYERROUTING_START/LEFW_LAYERROUTING
                    // - confirmed in lefwWriter.cpp) despite TWOWIDTHS
                    // appearing on layers named "cutNN" in complete.5.8.lef
                    // (e.g. cut25) - those are actually TYPE ROUTING, not
                    // CUT, per the fixture's own TYPE statement.
                    status = lefwLayerRoutingStartSpacingtableTwoWidths();
                    if (status)
                        return status;
                    // KNOWN VENDORED-WRITER EDGE CASE:
                    // lefwLayerRoutingSpacingtableTwoWidthsWidth checks
                    // `if (runLength)` to decide whether to write "PRL ..."
                    // at all - a real PRL of exactly 0.0 (present in
                    // complete.5.8.lef's own "WIDTH 0.25 PRL 0.0 ...") is
                    // indistinguishable from "no PRL" and gets silently
                    // dropped. Not worked around here (vendored code).
                    for (const TwoWidthsSpacingEntry &entry : layer->spacing_table_two_widths)
                    {
                        std::vector<double> spacings_um;
                        spacings_um.reserve(entry.spacings.size());
                        for (int64_t spacing : entry.spacings)
                            spacings_um.push_back(to_microns(spacing, dbu_per_micron));
                        status = lefwLayerRoutingSpacingtableTwoWidthsWidth(to_microns(entry.width, dbu_per_micron), entry.prl ? to_microns(*entry.prl, dbu_per_micron) : 0.0, static_cast<int>(spacings_um.size()), spacings_um.data());
                        if (status)
                            return status;
                    }
                    status = lefwLayerRoutineEndSpacingtable();
                    if (status)
                        return status;
                }

                if (layer->wire_extension)
                {
                    status = lefwLayerRoutingWireExtension(to_microns(*layer->wire_extension, dbu_per_micron));
                    if (status)
                        return status;
                }
                if (layer->resistance)
                {
                    status = lefwLayerRoutingResistance(fmt::format("{}", *layer->resistance).c_str());
                    if (status)
                        return status;
                }
                if (layer->capacitance)
                {
                    status = lefwLayerRoutingCapacitance(fmt::format("{}", *layer->capacitance).c_str());
                    if (status)
                        return status;
                }
                if (layer->height)
                {
                    status = lefwLayerRoutingHeight(to_microns(*layer->height, dbu_per_micron));
                    if (status)
                        return status;
                }
                if (layer->thickness)
                {
                    status = lefwLayerRoutingThickness(to_microns(*layer->thickness, dbu_per_micron));
                    if (status)
                        return status;
                }
                if (layer->shrinkage)
                {
                    status = lefwLayerRoutingShrinkage(to_microns(*layer->shrinkage, dbu_per_micron));
                    if (status)
                        return status;
                }
                if (layer->cap_multiplier)
                {
                    status = lefwLayerRoutingCapMultiplier(*layer->cap_multiplier);
                    if (status)
                        return status;
                }
                if (layer->edge_cap)
                {
                    status = lefwLayerRoutingEdgeCap(*layer->edge_cap);
                    if (status)
                        return status;
                }
                if (layer->antenna_length)
                {
                    status = lefwLayerRoutingAntennaLength(to_microns(*layer->antenna_length, dbu_per_micron));
                    if (status)
                        return status;
                }
                if (layer->max_width)
                {
                    status = lefwLayerRoutingMaxwidth(to_microns(*layer->max_width, dbu_per_micron));
                    if (status)
                        return status;
                }
                if (layer->min_width)
                {
                    status = lefwLayerRoutingMinwidth(to_microns(*layer->min_width, dbu_per_micron));
                    if (status)
                        return status;
                }
                if (!layer->min_sizes.empty())
                {
                    std::vector<double> min_widths_um;
                    std::vector<double> min_lengths_um;
                    min_widths_um.reserve(layer->min_sizes.size());
                    min_lengths_um.reserve(layer->min_sizes.size());
                    for (const MinSizeEntry &entry : layer->min_sizes)
                    {
                        min_widths_um.push_back(to_microns(entry.width, dbu_per_micron));
                        min_lengths_um.push_back(to_microns(entry.length, dbu_per_micron));
                    }
                    status = lefwLayerRoutingMinsize(static_cast<int>(min_widths_um.size()), min_widths_um.data(), min_lengths_um.data());
                    if (status)
                        return status;
                }
                if (!layer->min_enclosed_areas.empty())
                {
                    std::vector<double> areas_um2;
                    std::vector<double> widths_um;
                    areas_um2.reserve(layer->min_enclosed_areas.size());
                    widths_um.reserve(layer->min_enclosed_areas.size());
                    for (const MinEnclosedAreaEntry &entry : layer->min_enclosed_areas)
                    {
                        areas_um2.push_back(to_microns_squared(entry.area, dbu_per_micron));
                        widths_um.push_back(entry.width ? to_microns(*entry.width, dbu_per_micron) : 0.0);
                    }
                    status = lefwLayerRoutingMinenclosedarea(static_cast<int>(areas_um2.size()), areas_um2.data(), widths_um.data());
                    if (status)
                        return status;
                }
                if (layer->protrusion_width1)
                {
                    status = lefwLayerRoutingProtrusion(to_microns(*layer->protrusion_width1, dbu_per_micron), to_microns(*layer->protrusion_length, dbu_per_micron), to_microns(*layer->protrusion_width2, dbu_per_micron));
                    if (status)
                        return status;
                }
                // layer->split_wire_width is deliberately never written -
                // no lefwLayer*SplitWireWidth* function exists anywhere in
                // the vendored writer (grepped lefwWriter.hpp/.cpp - only
                // an internal LEFW_SPLITWIREWIDTH state-name constant).
                // Still fully read (lefrLayerCbkFn) - just unwritable.
                if (layer->minimum_density)
                {
                    status = lefwMinimumDensity(*layer->minimum_density);
                    if (status)
                        return status;
                }
                if (layer->maximum_density)
                {
                    status = lefwMaximumDensity(*layer->maximum_density);
                    if (status)
                        return status;
                }
                if (layer->density_check_step)
                {
                    status = lefwDensityCheckStep(to_microns(*layer->density_check_step, dbu_per_micron));
                    if (status)
                        return status;
                }
                if (layer->density_check_window)
                {
                    status = lefwDensityCheckWindow(to_microns(layer->density_check_window->length, dbu_per_micron), to_microns(layer->density_check_window->width, dbu_per_micron));
                    if (status)
                        return status;
                }
                if (layer->fill_active_spacing)
                {
                    status = lefwFillActiveSpacing(to_microns(*layer->fill_active_spacing, dbu_per_micron));
                    if (status)
                        return status;
                }

                status = write_layer_current_density(layer->ac_current_density, lefwLayerACCurrentDensity, lefwLayerACFrequency, lefwLayerACWidth, lefwLayerACCutarea, lefwLayerACTableEntries, dbu_per_micron);
                if (status)
                    return status;
                status = write_layer_current_density(layer->dc_current_density, lefwLayerDCCurrentDensity, nullptr, lefwLayerDCWidth, lefwLayerDCCutarea, lefwLayerDCTableEntries, dbu_per_micron);
                if (status)
                    return status;

                status = write_layer_antenna_models(layer->antenna_models, /*is_cut=*/false);
                if (status)
                    return status;

                status = write_properties(layer->properties, /*include_numeric=*/false);
                if (status)
                    return status;

                status = lefwEndLayerRouting(layer->name.c_str());
                if (status)
                    return status;
            }
            else
            {
                status = lefwStartLayer(layer->name.c_str(), layer->type.c_str());
                if (status)
                    return status;

                if (layer->default_mask)
                {
                    status = lefwLayerMask(*layer->default_mask);
                    if (status)
                        return status;
                }

                if (layer->width)
                {
                    status = lefwLayerWidth(to_microns(*layer->width, dbu_per_micron));
                    if (status)
                        return status;
                }
                for (const SpacingRule &rule : layer->spacing_rules)
                {
                    status = lefwLayerCutSpacing(to_microns(rule.distance, dbu_per_micron));
                    if (status)
                        return status;

                    if (rule.center_to_center)
                    {
                        status = lefwLayerCutSpacingCenterToCenter();
                        if (status)
                            return status;
                    }
                    if (rule.same_net)
                    {
                        status = lefwLayerCutSpacingSameNet();
                        if (status)
                            return status;
                    }

                    // At most one of LAYER/ADJACENTCUTS/PARALLELOVERLAP
                    // follows a CUT SPACING statement (lefwWriter.hpp's own
                    // "either this routine ... or ..." comments on each).
                    if (!rule.second_layer_name.empty())
                    {
                        status = lefwLayerCutSpacingLayer(rule.second_layer_name.c_str(), rule.second_layer_stack ? 1 : 0);
                        if (status)
                            return status;
                    }
                    else if (rule.adjacent_cuts)
                    {
                        status = lefwLayerCutSpacingAdjacent(*rule.adjacent_cuts, rule.adjacent_within ? to_microns(*rule.adjacent_within, dbu_per_micron) : 0.0, rule.adjacent_except_same_pg_net ? 1 : 0);
                        if (status)
                            return status;
                    }
                    else if (rule.parallel_overlap)
                    {
                        status = lefwLayerCutSpacingParallel();
                        if (status)
                            return status;
                    }

                    status = lefwLayerCutSpacingEnd();
                    if (status)
                        return status;
                }

                if (!layer->spacing_table_orthogonal.empty())
                {
                    std::vector<double> cut_withins_um;
                    std::vector<double> ortho_spacings_um;
                    cut_withins_um.reserve(layer->spacing_table_orthogonal.size());
                    ortho_spacings_um.reserve(layer->spacing_table_orthogonal.size());
                    for (const OrthogonalSpacingEntry &entry : layer->spacing_table_orthogonal)
                    {
                        cut_withins_um.push_back(to_microns(entry.cut_within, dbu_per_micron));
                        ortho_spacings_um.push_back(to_microns(entry.ortho_spacing, dbu_per_micron));
                    }
                    status = lefwLayerCutSpacingTableOrtho(static_cast<int>(cut_withins_um.size()), cut_withins_um.data(), ortho_spacings_um.data());
                    if (status)
                        return status;
                }

                // KNOWN VENDORED-WRITER GAP: lefwLayerArraySpacing requires
                // lefwIsCut (confirmed in lefwWriter.cpp) - unwritable on a
                // layer whose TYPE is ROUTING even though the reader can
                // populate it there (e.g. complete.5.8.lef's LAYER cut24,
                // TYPE ROUTING, uses ARRAYSPACING). Still fully read; just
                // never re-written for a ROUTING-typed layer.
                if (!layer->array_cuts.empty() || layer->array_spacing)
                {
                    std::vector<int> cuts;
                    std::vector<double> spacings_um;
                    cuts.reserve(layer->array_cuts.size());
                    spacings_um.reserve(layer->array_cuts.size());
                    for (const ArrayCutsEntry &entry : layer->array_cuts)
                    {
                        cuts.push_back(entry.cuts);
                        spacings_um.push_back(to_microns(entry.spacing, dbu_per_micron));
                    }
                    const bool long_array = layer->array_spacing && layer->array_spacing->long_array;
                    const double via_width_um = (layer->array_spacing && layer->array_spacing->via_width) ? to_microns(*layer->array_spacing->via_width, dbu_per_micron) : 0.0;
                    const double cut_spacing_um = layer->array_spacing ? to_microns(layer->array_spacing->cut_spacing, dbu_per_micron) : 0.0;
                    status = lefwLayerArraySpacing(long_array ? 1 : 0, via_width_um, cut_spacing_um, static_cast<int>(cuts.size()), cuts.data(), spacings_um.data());
                    if (status)
                        return status;
                }

                for (const PreferEnclosureEntry &entry : layer->prefer_enclosures)
                {
                    status = lefwLayerPreferEnclosure(entry.location.c_str(), to_microns(entry.overhang1, dbu_per_micron), to_microns(entry.overhang2, dbu_per_micron), entry.min_width ? to_microns(*entry.min_width, dbu_per_micron) : 0.0);
                    if (status)
                        return status;
                }

                // width/except_extra_cut and min_length are mutually
                // exclusive per the vendored writer's own three ENCLOSURE
                // variants (see lefrLayerCbkFn's own matching read side).
                for (const EnclosureEntry &entry : layer->enclosures)
                {
                    if (entry.width)
                        status = lefwLayerEnclosureWidth(entry.location.c_str(), to_microns(entry.overhang1, dbu_per_micron), to_microns(entry.overhang2, dbu_per_micron), to_microns(*entry.width, dbu_per_micron), entry.except_extra_cut ? to_microns(*entry.except_extra_cut, dbu_per_micron) : 0.0);
                    else if (entry.min_length)
                        status = lefwLayerEnclosureLength(entry.location.c_str(), to_microns(entry.overhang1, dbu_per_micron), to_microns(entry.overhang2, dbu_per_micron), to_microns(*entry.min_length, dbu_per_micron));
                    else
                        status = lefwLayerEnclosure(entry.location.c_str(), to_microns(entry.overhang1, dbu_per_micron), to_microns(entry.overhang2, dbu_per_micron), 0.0);
                    if (status)
                        return status;
                }

                // KNOWN VENDORED-LIBRARY BUG: lefwLayerResistancePerCut
                // literally writes the keyword "RESISTANCEPERCUT", but
                // lef.y has no such token anywhere - the real CUT-layer
                // grammar rule is plain "RESISTANCE <value> ;" (same
                // keyword as ROUTING's own, just a different lefiLayer
                // accessor pair - see lefrLayerCbkFn's own comment). A file
                // written with this call fails to re-parse entirely
                // (confirmed: LEFPARS-1 "encountered an error ... on token
                // RESISTANCEPERCUT"). Not called here - layer->resistance
                // is read-only for CUT layers with this vendored writer
                // version (still fully written for ROUTING layers above).

                status = write_layer_current_density(layer->ac_current_density, lefwLayerACCurrentDensity, lefwLayerACFrequency, lefwLayerACWidth, lefwLayerACCutarea, lefwLayerACTableEntries, dbu_per_micron);
                if (status)
                    return status;
                status = write_layer_current_density(layer->dc_current_density, lefwLayerDCCurrentDensity, nullptr, lefwLayerDCWidth, lefwLayerDCCutarea, lefwLayerDCTableEntries, dbu_per_micron);
                if (status)
                    return status;

                status = write_layer_antenna_models(layer->antenna_models, /*is_cut=*/true);
                if (status)
                    return status;

                status = write_properties(layer->properties);
                if (status)
                    return status;

                status = lefwEndLayer(layer->name.c_str());
                if (status)
                    return status;
            }
        }

        return 0;
    }

    int LEFWriter::write_via_layers(const std::vector<ViaLayer> &layers, double dbu_per_micron)
    {
        auto to_um = [&](int64_t v)
        { return to_microns(v, dbu_per_micron); };

        for (const ViaLayer &layer : layers)
        {
            int status = lefwViaLayer(layer.layer_name.c_str());
            if (status)
                return status;

            for (const Rect &rect : layer.rects)
            {
                status = lefwViaLayerRect(to_um(rect.ll.x), to_um(rect.ll.y), to_um(rect.ur.x), to_um(rect.ur.y), 0);
                if (status)
                    return status;
            }

            // KNOWN VENDORED-LIBRARY BUG (lefwWriter.cpp's own
            // lefwViaLayerPolygon, not our code, so not something to
            // hand-edit per CLAUDE.md's "never edit src/lefdef/" rule):
            // its non-encrypted branch prints the first point as
            // "%.11g %.11g" (no trailing separator) and every later
            // point as "%.11g %.11g " (no LEADING separator either),
            // so point 0's y and point 1's x land back-to-back with
            // zero characters between them - e.g. y0=-1, x1=-0.2
            // writes as the single unparseable token "-1-0.2" (found
            // via the lef_roundtrip_diff dev tool against
            // complete.5.8.lef's myVia23, which has real via-layer
            // POLYGON geometry - it made lefdiff choke partway through
            // and silently truncate the rest of that dump). Every
            // OTHER polygon writer in this file (lefwMacroPinPortLayerPolygon/
            // lefwMacroObsLayerPolygon, called from write_shape_geometry)
            // does NOT have this bug - only the VIA-specific one does.
            // No fixture in this codebase's own test suite exercises a
            // multi-point VIA POLYGON, so this doesn't affect CI, but a
            // real design with polygonal via geometry would write a
            // corrupt, unreadable LEF file - flagging for anyone
            // touching this path next, not fixing here (out of scope:
            // would require patching vendored source).
            for (const Polygon &polygon : layer.polygons)
            {
                std::vector<double> xs;
                std::vector<double> ys;
                xs.reserve(polygon.points.size());
                ys.reserve(polygon.points.size());
                for (const Point &point : polygon.points)
                {
                    xs.push_back(to_um(point.x));
                    ys.push_back(to_um(point.y));
                }
                status = lefwViaLayerPolygon(static_cast<int>(xs.size()), xs.data(), ys.data(), 0);
                if (status)
                    return status;
            }
        }

        return 0;
    }

    int LEFWriter::write_vias(const Root &root, TechnologyId technology_id)
    {
        const TechnologyData *technology = root.get_technology(technology_id);
        if (!technology)
            return 0;
        const double dbu_per_micron = technology->database_units_microns;
        auto to_um = [&](int64_t v)
        { return to_microns(v, dbu_per_micron); };

        for (ViaId via_id : root.get_technology_vias(technology_id))
        {
            const ViaData *via = root.get_via(via_id);
            if (!via)
                continue;

            int status = lefwStartVia(via->name.c_str(), via->is_default ? "DEFAULT" : nullptr);
            if (status)
                return status;

            if (via->foreign)
            {
                status = lefwViaForeignStr(via->foreign->name.c_str(), to_um(via->foreign->origin.x), to_um(via->foreign->origin.y), orientation_to_string(via->foreign->orient));
                if (status)
                    return status;
            }

            // Mutually exclusive per lefwViaResistance/lefwViaViarule's
            // own "either...or" contract - the reader only ever sets one
            // or the other (see lefrViaCbkFn's own comment).
            if (via->resistance)
            {
                status = lefwViaResistance(*via->resistance);
                if (status)
                    return status;
            }
            else if (via->via_rule)
            {
                const ViaRuleReference &vr = *via->via_rule;
                status = lefwViaViarule(vr.via_rule_name.c_str(), to_um(vr.cut_size.x), to_um(vr.cut_size.y),
                                         vr.bot_layer_name.c_str(), vr.cut_layer_name.c_str(), vr.top_layer_name.c_str(),
                                         to_um(vr.cut_spacing.x), to_um(vr.cut_spacing.y),
                                         to_um(vr.bot_enclosure.x), to_um(vr.bot_enclosure.y),
                                         to_um(vr.top_enclosure.x), to_um(vr.top_enclosure.y));
                if (status)
                    return status;
            }

            status = write_properties(via->properties);
            if (status)
                return status;

            status = write_via_layers(via->layers, dbu_per_micron);
            if (status)
                return status;

            status = lefwEndVia(via->name.c_str());
            if (status)
                return status;
        }

        return 0;
    }

    int LEFWriter::write_via_rules(const Root &root, TechnologyId technology_id)
    {
        const TechnologyData *technology = root.get_technology(technology_id);
        if (!technology)
            return 0;
        const double dbu_per_micron = technology->database_units_microns;
        auto to_um = [&](int64_t v)
        { return to_microns(v, dbu_per_micron); };
        auto to_um_opt = [&](const std::optional<int64_t> &v)
        { return v ? to_um(*v) : 0.0; };

        for (ViaRuleId via_rule_id : root.get_technology_via_rules(technology_id))
        {
            const ViaRuleData *via_rule = root.get_via_rule(via_rule_id);
            if (!via_rule)
                continue;

            int status;
            if (via_rule->is_generate)
            {
                status = lefwStartViaRuleGen(via_rule->name.c_str());
                if (status)
                    return status;

                if (via_rule->is_default)
                {
                    status = lefwViaRuleGenDefault();
                    if (status)
                        return status;
                }

                for (size_t i = 0; i < via_rule->layers.size() && i < 2; i++)
                {
                    const ViaRuleLayer &layer = via_rule->layers[i];
                    // lefwViaRuleGenLayer's DIRECTION/OVERHANG/METALOVERHANG
                    // args are rejected with LEFW_OBSOLETE for any version
                    // >= 5.6 (see lefwWriter.cpp's shared lefwViaRulePrtLayer
                    // helper) - write_lef always writes VERSION 5.8, so
                    // those three must never be passed; ENCLOSURE is the
                    // only way to write overhang data at this version,
                    // matching the reader's own translation (see
                    // lefrViaRuleCbkFn / K_OVERHANG in lef.y).
                    if (layer.enclosure_overhang1 && layer.enclosure_overhang2)
                        status = lefwViaRuleGenLayerEnclosure(layer.layer_name.c_str(), to_um(*layer.enclosure_overhang1), to_um(*layer.enclosure_overhang2), to_um_opt(layer.width_min), to_um_opt(layer.width_max));
                    else
                        status = lefwViaRuleGenLayer(layer.layer_name.c_str(), nullptr, to_um_opt(layer.width_min), to_um_opt(layer.width_max), 0.0, 0.0);
                    if (status)
                        return status;
                }

                if (via_rule->layers.size() >= 3 && via_rule->layers[2].rect)
                {
                    const ViaRuleLayer &cut_layer = via_rule->layers[2];
                    status = lefwViaRuleGenLayer3(cut_layer.layer_name.c_str(),
                                                   to_um(cut_layer.rect->ll.x), to_um(cut_layer.rect->ll.y),
                                                   to_um(cut_layer.rect->ur.x), to_um(cut_layer.rect->ur.y),
                                                   to_um_opt(cut_layer.spacing_step_x), to_um_opt(cut_layer.spacing_step_y),
                                                   cut_layer.resistance.value_or(0.0));
                    if (status)
                        return status;
                }

                status = lefwEndViaRuleGen(via_rule->name.c_str());
                if (status)
                    return status;
            }
            else
            {
                status = lefwStartViaRule(via_rule->name.c_str());
                if (status)
                    return status;

                for (size_t i = 0; i < via_rule->layers.size() && i < 2; i++)
                {
                    const ViaRuleLayer &layer = via_rule->layers[i];
                    // Same LEFW_OBSOLETE-at-5.6+ restriction as the
                    // GENERATE branch above - lefwViaRuleLayer shares the
                    // same lefwViaRulePrtLayer helper, so DIRECTION/
                    // OVERHANG/METALOVERHANG can't be written here either
                    // at this writer's fixed VERSION 5.8.
                    status = lefwViaRuleLayer(layer.layer_name.c_str(), nullptr, to_um_opt(layer.width_min), to_um_opt(layer.width_max), 0.0, 0.0);
                    if (status)
                        return status;
                }

                for (const std::string &via_name : via_rule->via_names)
                {
                    status = lefwViaRuleVia(via_name.c_str());
                    if (status)
                        return status;
                }

                // GENERATE via rules never reach here (see the branch
                // above) - the vendored writer's generic property
                // functions accept LEFW_VIARULE/_START but not
                // LEFW_VIARULEGEN/_START, so GENERATE VIARULE properties
                // are readable but not writable via this API (see
                // lef_writer.hpp's own class-level comment).
                status = write_properties(via_rule->properties);
                if (status)
                    return status;

                status = lefwEndViaRule(via_rule->name.c_str());
                if (status)
                    return status;
            }
        }

        return 0;
    }

    int LEFWriter::write_sites(const Root &root, TechnologyId technology_id)
    {
        const TechnologyData *technology = root.get_technology(technology_id);
        if (!technology)
            return 0;
        const double dbu_per_micron = technology->database_units_microns;
        auto to_um = [&](int64_t v)
        { return to_microns(v, dbu_per_micron); };

        for (SiteId site_id : root.get_technology_sites(technology_id))
        {
            const SiteData *site = root.get_site(site_id);
            if (!site)
                continue;

            // lefwSite takes one space-joined symmetry string, same
            // convention as write_macro's own SYMMETRY handling.
            std::string symmetry;
            if (site->symmetry.x)
                symmetry += "X ";
            if (site->symmetry.y)
                symmetry += "Y ";
            if (site->symmetry.r90)
                symmetry += "R90 ";
            if (!symmetry.empty())
                symmetry.pop_back(); // trailing space

            int status = lefwSite(site->name.c_str(), site->site_class.empty() ? nullptr : site->site_class.c_str(),
                                   symmetry.empty() ? nullptr : symmetry.c_str(),
                                   site->size ? to_um(site->size->x) : 0.0, site->size ? to_um(site->size->y) : 0.0);
            if (status)
                return status;

            for (const RowPatternEntry &entry : site->row_pattern)
            {
                status = lefwSiteRowPatternStr(entry.site_name.c_str(), orientation_to_string(entry.orient));
                if (status)
                    return status;
            }

            // site->properties is deliberately never written: the vendored
            // writer's lefwStringProperty/lefwRealProperty/lefwIntProperty
            // only accept lefwState values LEFW_VIA/LAYER/VIARULE/MACRO(_START)/
            // VIA_START/VIARULE_START/LAYER_START/BEGINEXT/VIAVIARULE/
            // LAYERROUTING(_START) (confirmed in lefwWriter.cpp) - LEFW_SITE
            // is not among them, so SITE properties are readable but not
            // writable via this API. Same gap for NONDEFAULTRULE, see
            // write_non_default_rules below.
            status = lefwEndSite(site->name.c_str());
            if (status)
                return status;
        }

        return 0;
    }

    int LEFWriter::write_non_default_rules(const Root &root, TechnologyId technology_id)
    {
        // rule->properties is deliberately never written here either -
        // LEFW_NONDEFAULTRULE(_START) isn't among the states the vendored
        // writer's generic property functions accept (see write_sites's
        // own comment for the full accepted-state list).
        const TechnologyData *technology = root.get_technology(technology_id);
        if (!technology)
            return 0;
        const double dbu_per_micron = technology->database_units_microns;
        auto to_um = [&](int64_t v)
        { return to_microns(v, dbu_per_micron); };

        for (NonDefaultRuleId rule_id : root.get_technology_non_default_rules(technology_id))
        {
            const NonDefaultRuleData *rule = root.get_non_default_rule(rule_id);
            if (!rule)
                continue;

            int status = lefwStartNonDefaultRule(rule->name.c_str());
            if (status)
                return status;

            // HARDSPACING must come first - lef.y's own grammar has
            // nd_hardspacing appear before nd_rules (LAYER/VIA/...) in the
            // NONDEFAULTRULE production, not just anywhere in the block.
            if (rule->hard_spacing)
            {
                status = lefwNonDefaultRuleHardspacing();
                if (status)
                    return status;
            }

            for (const NonDefaultRuleLayer &layer : rule->layers)
            {
                // diag_width has no writer parameter in this vendored
                // version's lefwNonDefaultRuleLayer (confirmed against
                // lefwWriter.hpp - width/minSpacing/wireExtension/
                // resistance/capacitance/edgeCap only) - read-only, same
                // "vendored writer gap" pattern as this phase's other
                // documented cases.
                status = lefwNonDefaultRuleLayer(layer.layer_name.c_str(),
                                                  layer.width ? to_um(*layer.width) : 0.0,
                                                  layer.spacing ? to_um(*layer.spacing) : 0.0,
                                                  layer.wire_extension ? to_um(*layer.wire_extension) : 0.0,
                                                  layer.resistance.value_or(0.0),
                                                  layer.capacitance.value_or(0.0),
                                                  layer.edge_cap.value_or(0.0));
                if (status)
                    return status;
            }

            for (const NonDefaultRuleVia &via : rule->vias)
            {
                status = lefwNonDefaultRuleStartVia(via.name.c_str(), via.is_default ? "DEFAULT" : nullptr);
                if (status)
                    return status;

                if (via.foreign)
                {
                    status = lefwViaForeignStr(via.foreign->name.c_str(), to_um(via.foreign->origin.x), to_um(via.foreign->origin.y), orientation_to_string(via.foreign->orient));
                    if (status)
                        return status;
                }
                if (via.resistance)
                {
                    status = lefwViaResistance(*via.resistance);
                    if (status)
                        return status;
                }

                // lefwNonDefaultRuleStartVia sets lefwState =
                // LEFW_VIA_START, the same state a top-level VIA uses -
                // write_properties' generic property functions already
                // accept that state (see its own comment), so this works
                // without any of the SITE/NONDEFAULTRULE-itself gaps.
                status = write_properties(via.properties);
                if (status)
                    return status;

                status = write_via_layers(via.layers, dbu_per_micron);
                if (status)
                    return status;

                status = lefwNonDefaultRuleEndVia(via.name.c_str());
                if (status)
                    return status;
            }

            for (const std::string &via_name : rule->use_via_names)
            {
                status = lefwNonDefaultRuleUseVia(via_name.c_str());
                if (status)
                    return status;
            }

            for (const std::string &via_rule_name : rule->use_via_rule_names)
            {
                status = lefwNonDefaultRuleUseViaRule(via_rule_name.c_str());
                if (status)
                    return status;
            }

            for (const MinCutOverride &min_cut : rule->min_cuts)
            {
                status = lefwNonDefaultRuleMinCuts(min_cut.cut_layer_name.c_str(), min_cut.num_cuts);
                if (status)
                    return status;
            }

            status = lefwEndNonDefaultRule(rule->name.c_str());
            if (status)
                return status;
        }

        return 0;
    }

    int LEFWriter::write_shape_geometry(const Shape &shape, double dbu_per_micron, bool is_pin_port)
    {
        auto to_um = [&](int64_t v)
        { return to_microns(v, dbu_per_micron); };

        // LAYER/DESIGNRULEWIDTH/EXCEPTPGNET are mutually exclusive per
        // lef.y's own grammar - one combined statement opens this LAYER
        // occurrence, matching lefwMacroObsLayer's own doc comment
        // ("Either this routine, lefwMacroObsDesignRuleWidth, ... or
        // lefwMacroExceptPGNet must be called").
        int status;
        if (shape.except_pg_net && !is_pin_port)
        {
            // KNOWN VENDORED-WRITER GAP: lefwMacroExceptPGNet only
            // accepts !lefwIsMacroObs (confirmed in lefwWriter.cpp) - it
            // cannot be called from a PIN PORT context at all, even
            // though lef.y's own layer_exceptpgnet grammar rule is shared
            // by both PORT and OBS geometry. It also guards on an
            // internal lefwSpacingVal flag reset only once per OBS
            // section (not per LAYER) - once any LAYER-with-SPACING has
            // been written anywhere earlier in this OBS section,
            // EXCEPTPGNET is permanently blocked for the rest of it.
            // Restricted to OBS here (is_pin_port already excluded above)
            // and callers should avoid mixing SPACING-layers before an
            // EXCEPTPGNET-layer in the same OBS, matching how
            // complete.5.8.lef's own OBS blocks never mix the two.
            status = lefwMacroExceptPGNet(shape.layer_name.c_str());
        }
        else if (shape.design_rule_width != 0)
        {
            // KNOWN LIMITATION: a real DESIGNRULEWIDTH of exactly 0
            // (complete.5.8.lef has one: "LAYER a1sig DESIGNRULEWIDTH 0")
            // is indistinguishable from "unset" with this 0-means-unset
            // representation (see schema.py's own field comment on why
            // Shape.design_rule_width isn't is_optional) and falls
            // through to the plain LAYER branch below instead - a narrow,
            // accepted tradeoff matching several other 0-as-sentinel
            // conventions already in this codebase.
            status = is_pin_port
                         ? lefwMacroPinPortDesignRuleWidth(shape.layer_name.c_str(), to_um(shape.design_rule_width))
                         : lefwMacroObsDesignRuleWidth(shape.layer_name.c_str(), to_um(shape.design_rule_width));
        }
        else
        {
            status = is_pin_port ? lefwMacroPinPortLayer(shape.layer_name.c_str(), to_um(shape.spacing)) : lefwMacroObsLayer(shape.layer_name.c_str(), to_um(shape.spacing));
        }
        if (status)
            return status;

        // LEF's WIDTH statement applies to subsequent PATHs within this
        // LAYER context until it next changes - re-derived here from each
        // Path's own stored width (Shape stores width per-Path, not as
        // separate parse-time state - see schema.py's Path Klass) rather
        // than tracked separately.
        bool has_current_width = false;
        uint64_t current_width = 0;

        // rect_masks/polygon_masks/path_masks are parallel arrays (see
        // shapes_from_parser's own reading) - 0 (no mask) if a given
        // index is out of range, e.g. for Shapes built directly rather
        // than read from a LEF file that never set one.
        for (size_t i = 0; i < shape.rects.size(); i++)
        {
            const Rect &rect = shape.rects[i];
            const int mask = i < shape.rect_masks.size() ? shape.rect_masks[i] : 0;
            status = is_pin_port
                         ? lefwMacroPinPortLayerRect(to_um(rect.ll.x), to_um(rect.ll.y), to_um(rect.ur.x), to_um(rect.ur.y), 0, 0, 0, 0, mask)
                         : lefwMacroObsLayerRect(to_um(rect.ll.x), to_um(rect.ll.y), to_um(rect.ur.x), to_um(rect.ur.y), 0, 0, 0, 0, mask);
            if (status)
                return status;
        }

        for (const RectIterate &it : shape.rect_iterates)
        {
            status = is_pin_port
                         ? lefwMacroPinPortLayerRect(to_um(it.rect.ll.x), to_um(it.rect.ll.y), to_um(it.rect.ur.x), to_um(it.rect.ur.y), it.num_x, it.num_y, to_um(it.space_x), to_um(it.space_y), 0)
                         : lefwMacroObsLayerRect(to_um(it.rect.ll.x), to_um(it.rect.ll.y), to_um(it.rect.ur.x), to_um(it.rect.ur.y), it.num_x, it.num_y, to_um(it.space_x), to_um(it.space_y), 0);
            if (status)
                return status;
        }

        auto write_path = [&](const Path &path, int num_x, int num_y, double space_x_um, double space_y_um, int mask) -> int
        {
            if (!has_current_width || current_width != path.width)
            {
                const double width_um = to_um(static_cast<int64_t>(path.width));
                const int width_status = is_pin_port ? lefwMacroPinPortLayerWidth(width_um) : lefwMacroObsLayerWidth(width_um);
                if (width_status)
                    return width_status;
                has_current_width = true;
                current_width = path.width;
            }

            std::vector<double> xs;
            std::vector<double> ys;
            xs.reserve(path.polygon.points.size());
            ys.reserve(path.polygon.points.size());
            for (const Point &point : path.polygon.points)
            {
                xs.push_back(to_um(point.x));
                ys.push_back(to_um(point.y));
            }

            return is_pin_port
                       ? lefwMacroPinPortLayerPath(static_cast<int>(xs.size()), xs.data(), ys.data(), num_x, num_y, space_x_um, space_y_um, mask)
                       : lefwMacroObsLayerPath(static_cast<int>(xs.size()), xs.data(), ys.data(), num_x, num_y, space_x_um, space_y_um, mask);
        };

        for (size_t i = 0; i < shape.paths.size(); i++)
        {
            const int mask = i < shape.path_masks.size() ? shape.path_masks[i] : 0;
            status = write_path(shape.paths[i], 0, 0, 0, 0, mask);
            if (status)
                return status;
        }

        for (const PathIterate &it : shape.path_iterates)
        {
            status = write_path(it.path, it.num_x, it.num_y, to_um(it.space_x), to_um(it.space_y), 0);
            if (status)
                return status;
        }

        auto write_polygon = [&](const Polygon &polygon, int num_x, int num_y, double space_x_um, double space_y_um, int mask) -> int
        {
            std::vector<double> xs;
            std::vector<double> ys;
            xs.reserve(polygon.points.size());
            ys.reserve(polygon.points.size());
            for (const Point &point : polygon.points)
            {
                xs.push_back(to_um(point.x));
                ys.push_back(to_um(point.y));
            }

            return is_pin_port
                       ? lefwMacroPinPortLayerPolygon(static_cast<int>(xs.size()), xs.data(), ys.data(), num_x, num_y, space_x_um, space_y_um, mask)
                       : lefwMacroObsLayerPolygon(static_cast<int>(xs.size()), xs.data(), ys.data(), num_x, num_y, space_x_um, space_y_um, mask);
        };

        for (size_t i = 0; i < shape.polygons.size(); i++)
        {
            const int mask = i < shape.polygon_masks.size() ? shape.polygon_masks[i] : 0;
            status = write_polygon(shape.polygons[i], 0, 0, 0, 0, mask);
            if (status)
                return status;
        }

        for (const PolygonIterate &it : shape.polygon_iterates)
        {
            status = write_polygon(it.polygon, it.num_x, it.num_y, to_um(it.space_x), to_um(it.space_y), 0);
            if (status)
                return status;
        }

        // lefwMacroObsVia/lefwMacroPinPortVia are callable at any point
        // once the section is open (lefwIsMacroObs/lefwIsMacroPinPort are
        // simple flags, not fine-grained state) - matches
        // complete.5.8.lef's own usage of VIA nested after LAYER/RECT
        // within one PORT block.
        for (const ShapeVia &via : shape.vias)
        {
            status = is_pin_port
                         ? lefwMacroPinPortVia(to_um(via.origin.x), to_um(via.origin.y), via.via_name.c_str(), 0, 0, 0, 0, via.mask.value_or(0))
                         : lefwMacroObsVia(to_um(via.origin.x), to_um(via.origin.y), via.via_name.c_str(), 0, 0, 0, 0, via.mask.value_or(0));
            if (status)
                return status;
        }
        for (const ShapeViaIterate &via_iter : shape.via_iterates)
        {
            status = is_pin_port
                         ? lefwMacroPinPortVia(to_um(via_iter.origin.x), to_um(via_iter.origin.y), via_iter.via_name.c_str(), via_iter.num_x, via_iter.num_y, to_um(via_iter.space_x), to_um(via_iter.space_y), via_iter.mask.value_or(0))
                         : lefwMacroObsVia(to_um(via_iter.origin.x), to_um(via_iter.origin.y), via_iter.via_name.c_str(), via_iter.num_x, via_iter.num_y, to_um(via_iter.space_x), to_um(via_iter.space_y), via_iter.mask.value_or(0));
            if (status)
                return status;
        }

        return 0;
    }

    int LEFWriter::write_terminal(const Root &root, TerminalId terminal_id, double dbu_per_micron)
    {
        const TerminalData *terminal = root.get_terminal(terminal_id);
        if (!terminal)
            return 0;

        int status = lefwStartMacroPin(terminal->name.c_str());
        if (status)
            return status;

        if (const char *direction = signal_direction_to_string(terminal->direction))
        {
            status = lefwMacroPinDirection(direction);
            if (status)
                return status;
        }
        if (!terminal->use.empty())
        {
            status = lefwMacroPinUse(terminal->use.c_str());
            if (status)
                return status;
        }
        if (!terminal->shape.empty())
        {
            status = lefwMacroPinShape(terminal->shape.c_str());
            if (status)
                return status;
        }
        if (!terminal->must_join.empty())
        {
            status = lefwMacroPinMustjoin(terminal->must_join.c_str());
            if (status)
                return status;
        }
        if (!terminal->net_expr.empty())
        {
            status = lefwMacroPinNetExpr(terminal->net_expr.c_str());
            if (status)
                return status;
        }
        // terminal->leq is deliberately never written - lefwMacroPinLEQ is
        // obsoleted for VERSION >= 5.6 with no replacement (same
        // unreachable-at-5.8 gap as the macro-level LEQ above).
        if (!terminal->taper_rule.empty())
        {
            status = lefwMacroPinTaperRule(terminal->taper_rule.c_str());
            if (status)
                return status;
        }
        if (!terminal->supply_sensitivity.empty())
        {
            status = lefwMacroPinSupplySensitivity(terminal->supply_sensitivity.c_str());
            if (status)
                return status;
        }
        if (!terminal->ground_sensitivity.empty())
        {
            status = lefwMacroPinGroundSensitivity(terminal->ground_sensitivity.c_str());
            if (status)
                return status;
        }
        // terminal->rise_slew_limit/fall_slew_limit/max_load are
        // deliberately never written - lefwWriter.hpp/.cpp contain zero
        // lefwMacroPin* functions for RISESLEWLIMIT/FALLSLEWLIMIT/MAXLOAD
        // (confirmed by grep) even though lefiPin fully reads all three -
        // no vendored writer entry point exists at all, same class of gap
        // as split_wire_width.

        // Flat pre-5.5 (value, layer) antenna fields - AntennaGateArea has
        // no such flat form (see lefrPinCbkFn's own comment), only the
        // oxide-scoped one below.
        status = write_pin_antenna_values(terminal->antenna_partial_metal_area, lefwMacroPinAntennaPartialMetalArea);
        if (status)
            return status;
        status = write_pin_antenna_values(terminal->antenna_partial_metal_side_area, lefwMacroPinAntennaPartialMetalSideArea);
        if (status)
            return status;
        status = write_pin_antenna_values(terminal->antenna_partial_cut_area, lefwMacroPinAntennaPartialCutArea);
        if (status)
            return status;
        status = write_pin_antenna_values(terminal->antenna_diff_area, lefwMacroPinAntennaDiffArea);
        if (status)
            return status;

        // 5.5 oxide-scoped antenna models - lefwMacroPinAntennaModel sets
        // "current oxide" state, no explicit end call (same flat
        // sequential-call pattern as lefwLayerAntennaModel above).
        for (const PinAntennaModel &model : terminal->antenna_models)
        {
            status = lefwMacroPinAntennaModel(model.oxide.c_str());
            if (status)
                return status;
            status = write_pin_antenna_values(model.gate_area, lefwMacroPinAntennaGateArea);
            if (status)
                return status;
            status = write_pin_antenna_values(model.max_area_car, lefwMacroPinAntennaMaxAreaCar);
            if (status)
                return status;
            status = write_pin_antenna_values(model.max_side_area_car, lefwMacroPinAntennaMaxSideAreaCar);
            if (status)
                return status;
            status = write_pin_antenna_values(model.max_cut_car, lefwMacroPinAntennaMaxCutCar);
            if (status)
                return status;
        }

        // lefwStartMacroPin doesn't change lefwState (it stays LEFW_MACRO,
        // tracked instead via the separate lefwIsMacroPin flag) - PIN
        // properties work via the same generic functions MACRO properties
        // use, unlike SITE/NONDEFAULTRULE/GENERATE-VIARULE.
        status = write_properties(terminal->properties);
        if (status)
            return status;

        for (TerminalPortId port_id : root.get_terminal_ports(terminal_id))
        {
            const TerminalPortData *port = root.get_terminal_port(port_id);
            if (!port)
                continue;

            status = lefwStartMacroPinPort(port->port_class.empty() ? nullptr : port->port_class.c_str());
            if (status)
                return status;

            for (ShapeId shape_id : root.get_terminal_port_shapes(port_id))
            {
                const Shape *shape = root.get_shape(shape_id);
                if (!shape)
                    continue;
                status = write_shape_geometry(*shape, dbu_per_micron, true);
                if (status)
                    return status;
            }

            status = lefwEndMacroPinPort();
            if (status)
                return status;
        }

        return lefwEndMacroPin(terminal->name.c_str());
    }

    int LEFWriter::write_obstruction(const Root &root, ObstructionId obstruction_id, double dbu_per_micron)
    {
        const ObstructionData *obstruction = root.get_obstruction(obstruction_id);
        if (!obstruction)
            return 0;

        int status = lefwStartMacroObs();
        if (status)
            return status;

        for (ShapeId shape_id : root.get_obstruction_shapes(obstruction_id))
        {
            const Shape *shape = root.get_shape(shape_id);
            if (!shape)
                continue;
            status = write_shape_geometry(*shape, dbu_per_micron, false);
            if (status)
                return status;
        }

        return lefwEndMacroObs();
    }

    int LEFWriter::write_macro(const Root &root, AbstractId abstract_id, double dbu_per_micron)
    {
        const AbstractData *abstract = root.get_abstract(abstract_id);
        if (!abstract)
            return 0;

        const DesignData *design = root.get_design(abstract->design);
        if (!design)
            return 0;

        auto to_um = [&](int64_t v)
        { return to_microns(v, dbu_per_micron); };

        int status = lefwStartMacro(design->name.c_str());
        if (status)
            return status;

        if (!abstract->type.empty())
        {
            // abstract->type stores LEF's `CLASS <base> [<subtype>] ;`
            // verbatim as ONE space-joined string (confirmed by reading
            // lef.y's own class_type grammar rule - e.g. "CORE WELLTAP",
            // "BLOCK BLACKBOX" are single strings from lef_macro->
            // macroClass(), not two separate fields), but lefwMacroClass
            // takes base/subtype as two separate arguments and validates
            // the base against a fixed known list - passing the combined
            // string as a single argument never matches and fails the
            // whole MACRO write with LEFW_BAD_DATA, silently losing 100%
            // of that macro's content. Split back apart here.
            const size_t space_pos = abstract->type.find(' ');
            const std::string macro_class = space_pos == std::string::npos ? abstract->type : abstract->type.substr(0, space_pos);
            const std::string macro_subclass = space_pos == std::string::npos ? std::string() : abstract->type.substr(space_pos + 1);
            status = lefwMacroClass(macro_class.c_str(), macro_subclass.empty() ? nullptr : macro_subclass.c_str());
            if (status)
                return status;
        }

        // abstract->leq/power/source are deliberately never written: for a
        // VERSION >= 5.6 file (this writer always writes 5.8), MACRO LEQ
        // is obsoleted with no replacement (lefwMacroLEQ's own
        // "versionNum >= 5.6 -> LEFW_OBSOLETE" check) and POWER/SOURCE are
        // obsolete at >= 5.4/5.6 respectively - and lef.y's own grammar
        // drops all three silently on READ at those versions too (never
        // populated in the first place), so this is a consistent,
        // unreachable-at-5.8 gap on both sides, not an asymmetry to work
        // around.
        if (!abstract->eeq.empty())
        {
            status = lefwMacroEEQ(abstract->eeq.c_str());
            if (status)
                return status;
        }
        if (abstract->is_fixed_mask)
        {
            status = lefwMacroFixedMask();
            if (status)
                return status;
        }

        for (const Foreign &foreign : abstract->foreigns)
        {
            status = lefwMacroForeignStr(foreign.name.c_str(), to_um(foreign.origin.x), to_um(foreign.origin.y), orientation_to_string(foreign.orient));
            if (status)
                return status;
        }

        status = lefwMacroOrigin(to_um(abstract->origin.x), to_um(abstract->origin.y));
        if (status)
            return status;

        status = lefwMacroSize(to_um(abstract->size.x), to_um(abstract->size.y));
        if (status)
            return status;

        std::string symmetry;
        if (abstract->symmetry.x)
            symmetry += "X ";
        if (abstract->symmetry.y)
            symmetry += "Y ";
        if (abstract->symmetry.r90)
            symmetry += "R90 ";
        if (!symmetry.empty())
        {
            symmetry.pop_back(); // trailing space
            status = lefwMacroSymmetry(symmetry.c_str());
            if (status)
                return status;
        }

        if (!abstract->site.empty())
        {
            status = lefwMacroSite(abstract->site.c_str());
            if (status)
                return status;
        }

        // Distinct, mutually-exclusive grammar alternative from the
        // singular site name above (see lefrMacroCbkFn's own comment on
        // setSiteName vs setSitePattern).
        for (const MacroSitePlacement &placement : abstract->site_placements)
        {
            status = lefwMacroSitePatternStr(placement.site_name.c_str(), to_um(placement.origin.x), to_um(placement.origin.y), orientation_to_string(placement.orient),
                                              placement.num_x.value_or(0), placement.num_y.value_or(0),
                                              placement.step_x ? to_um(*placement.step_x) : 0.0, placement.step_y ? to_um(*placement.step_y) : 0.0);
            if (status)
                return status;
        }

        status = write_properties(abstract->properties);
        if (status)
            return status;

        for (TerminalId terminal_id : root.get_abstract_terminals(abstract_id))
        {
            status = write_terminal(root, terminal_id, dbu_per_micron);
            if (status)
                return status;
        }

        for (ObstructionId obstruction_id : root.get_abstract_obstructions(abstract_id))
        {
            status = write_obstruction(root, obstruction_id, dbu_per_micron);
            if (status)
                return status;
        }

        // abstract->densities is deliberately never written: the vendored
        // lefwStartMacroDensity(layerName) prints "DENSITY <layerName>\n"
        // directly, with no "LAYER" keyword - but lef.y's own macro_density
        // grammar rule requires "DENSITY" alone, then one "LAYER name ;"
        // statement per layer group (density_layer) - so the written text
        // can never be re-parsed as a DENSITY statement at all, for any
        // layer count. lefwStartMacroDensity also flatly refuses a second
        // call in the same macro (lefwIsMacroDensity guard), so even a
        // syntax-correct workaround couldn't cover more than one layer.
        // Fully readable (lefrDensityCbkFn) - not writable via this
        // vendored version. Not fixed (vendored code).

        return lefwEndMacro(design->name.c_str());
    }

    int LEFWriter::write_lef(const std::string &path, const Root &root, AbstractId abstract_id, LayerWriteMode mode)
    {
        messages_.clear();

        std::unique_ptr<FILE, int (*)(FILE *)> file(fopen(path.c_str(), "w"), &fclose);
        if (!file)
        {
            messages_.push_back(fmt::format("ERROR: Could not open {} for writing.", path));
            return 1;
        }

        int status = lefwInit(file.get());
        if (status)
        {
            messages_.push_back(fmt::format("ERROR: lefwInit failed with status {}.", status));
            return status;
        }

        status = lefwVersion(5, 8);
        if (status)
        {
            messages_.push_back(fmt::format("ERROR: lefwVersion failed with status {}.", status));
            return status;
        }

        const auto technology_ids = root.get_technology_ids();
        const TechnologyId technology_id = technology_ids.empty() ? TechnologyId{} : technology_ids.front();
        const TechnologyData *technology = technology_id.valid() ? root.get_technology(technology_id) : nullptr;
        const double dbu_per_micron = technology ? technology->database_units_microns : 0.0;

        // Written unconditionally (like VERSION above), not gated on
        // `mode` - these are top-level header statements, not layer/macro
        // content, and there's no case where writing a legal LEF file
        // should omit them if we have them.
        if (technology && !technology->bus_bit_chars.empty())
        {
            status = lefwBusBitChars(technology->bus_bit_chars.c_str());
            if (status)
            {
                messages_.push_back(fmt::format("ERROR: lefwBusBitChars failed with status {}.", status));
                return status;
            }
        }
        if (technology && !technology->divider_char.empty())
        {
            status = lefwDividerChar(technology->divider_char.c_str());
            if (status)
            {
                messages_.push_back(fmt::format("ERROR: lefwDividerChar failed with status {}.", status));
                return status;
            }
        }

        if (technology_id.valid() && mode != LayerWriteMode::None)
        {
            status = write_units(root, technology_id);
            if (status)
            {
                messages_.push_back(fmt::format("ERROR: Writing UNITS failed with status {}.", status));
                return status;
            }

            // After UNITS, before LAYER/VIA/... - matches complete.5.8.lef's
            // own real ordering, and LEF's own requirement that
            // PROPERTYDEFINITIONS precede the first use of any property
            // name it declares.
            status = write_property_definitions(root, technology_id);
            if (status)
            {
                messages_.push_back(fmt::format("ERROR: Writing PROPERTYDEFINITIONS failed with status {}.", status));
                return status;
            }

            status = write_technology_layers(root, technology_id);
            if (status)
            {
                messages_.push_back(fmt::format("ERROR: Writing LAYERs failed with status {}.", status));
                return status;
            }

            // After LAYER (VIA/VIARULE reference layers by name), before
            // MACRO (LEF's own required ordering).
            status = write_vias(root, technology_id);
            if (status)
            {
                messages_.push_back(fmt::format("ERROR: Writing VIAs failed with status {}.", status));
                return status;
            }

            status = write_via_rules(root, technology_id);
            if (status)
            {
                messages_.push_back(fmt::format("ERROR: Writing VIARULEs failed with status {}.", status));
                return status;
            }

            // NONDEFAULTRULE then SITE, matching complete.5.8.lef's own
            // real ordering (after VIA/VIARULE, before MACRO).
            status = write_non_default_rules(root, technology_id);
            if (status)
            {
                messages_.push_back(fmt::format("ERROR: Writing NONDEFAULTRULEs failed with status {}.", status));
                return status;
            }

            status = write_sites(root, technology_id);
            if (status)
            {
                messages_.push_back(fmt::format("ERROR: Writing SITEs failed with status {}.", status));
                return status;
            }
        }

        if (mode != LayerWriteMode::TechnologyOnly)
        {
            status = write_macro(root, abstract_id, dbu_per_micron);
            if (status)
            {
                messages_.push_back(fmt::format("ERROR: Writing MACRO failed with status {}.", status));
                return status;
            }
        }

        status = lefwEnd();
        if (status)
        {
            messages_.push_back(fmt::format("ERROR: lefwEnd failed with status {}.", status));
            return status;
        }

        return 0;
    }
}
