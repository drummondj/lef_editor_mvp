#include "lef_writer.hpp"
#include "../lefdef/lef/include/lefwWriter.hpp"
#include <fmt/format.h>
#include <memory>
#include <cstdio>

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

    const char *routing_direction_to_string(le::RoutingDirection direction)
    {
        // NONE falls back to HORIZONTAL - lefwLayerRouting's direction
        // argument is required (LEF's own ROUTING statement always has
        // one), unlike LEFReader's own read side where DIRECTION is
        // optional per-layer (see lefrLayerCbkFn's own has*() guard).
        return direction == le::RoutingDirection::V ? "VERTICAL" : "HORIZONTAL";
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
                status = lefwLayerRouting(routing_direction_to_string(layer->direction), layer->width ? to_microns(*layer->width, dbu_per_micron) : 0.0);
                if (status)
                    return status;

                if (layer->pitch)
                {
                    status = lefwLayerRoutingPitch(to_microns(*layer->pitch, dbu_per_micron));
                    if (status)
                        return status;
                }
                if (layer->offset)
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

                status = lefwEndLayerRouting(layer->name.c_str());
                if (status)
                    return status;
            }
            else
            {
                status = lefwStartLayer(layer->name.c_str(), layer->type.c_str());
                if (status)
                    return status;

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

            status = lefwEndSite(site->name.c_str());
            if (status)
                return status;
        }

        return 0;
    }

    int LEFWriter::write_non_default_rules(const Root &root, TechnologyId technology_id)
    {
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

        int status = is_pin_port ? lefwMacroPinPortLayer(shape.layer_name.c_str(), 0) : lefwMacroObsLayer(shape.layer_name.c_str(), 0);
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

        for (TerminalPortId port_id : root.get_terminal_ports(terminal_id))
        {
            const TerminalPortData *port = root.get_terminal_port(port_id);
            if (!port)
                continue;

            status = lefwStartMacroPinPort(nullptr);
            if (status)
                return status;

            for (const Shape &shape : port->shapes)
            {
                status = write_shape_geometry(shape, dbu_per_micron, true);
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

        for (const Shape &shape : obstruction->shapes)
        {
            status = write_shape_geometry(shape, dbu_per_micron, false);
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
