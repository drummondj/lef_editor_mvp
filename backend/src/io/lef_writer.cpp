#include "lef_writer.hpp"
#include "../lefdef/lef/include/lefwWriter.hpp"
#include <fmt/format.h>
#include <memory>

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
                if (layer->spacing)
                {
                    status = lefwLayerRoutingSpacing(to_microns(*layer->spacing, dbu_per_micron));
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
                if (layer->spacing)
                {
                    status = lefwLayerCutSpacing(to_microns(*layer->spacing, dbu_per_micron));
                    if (status)
                        return status;
                    status = lefwLayerCutSpacingEnd();
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

        for (const Rect &rect : shape.rects)
        {
            status = is_pin_port
                         ? lefwMacroPinPortLayerRect(to_um(rect.ll.x), to_um(rect.ll.y), to_um(rect.ur.x), to_um(rect.ur.y), 0, 0, 0, 0, 0)
                         : lefwMacroObsLayerRect(to_um(rect.ll.x), to_um(rect.ll.y), to_um(rect.ur.x), to_um(rect.ur.y), 0, 0, 0, 0, 0);
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

        auto write_path = [&](const Path &path, int num_x, int num_y, double space_x_um, double space_y_um) -> int
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
                       ? lefwMacroPinPortLayerPath(static_cast<int>(xs.size()), xs.data(), ys.data(), num_x, num_y, space_x_um, space_y_um, 0)
                       : lefwMacroObsLayerPath(static_cast<int>(xs.size()), xs.data(), ys.data(), num_x, num_y, space_x_um, space_y_um, 0);
        };

        for (const Path &path : shape.paths)
        {
            status = write_path(path, 0, 0, 0, 0);
            if (status)
                return status;
        }

        for (const PathIterate &it : shape.path_iterates)
        {
            status = write_path(it.path, it.num_x, it.num_y, to_um(it.space_x), to_um(it.space_y));
            if (status)
                return status;
        }

        auto write_polygon = [&](const Polygon &polygon, int num_x, int num_y, double space_x_um, double space_y_um) -> int
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
                       ? lefwMacroPinPortLayerPolygon(static_cast<int>(xs.size()), xs.data(), ys.data(), num_x, num_y, space_x_um, space_y_um, 0)
                       : lefwMacroObsLayerPolygon(static_cast<int>(xs.size()), xs.data(), ys.data(), num_x, num_y, space_x_um, space_y_um, 0);
        };

        for (const Polygon &polygon : shape.polygons)
        {
            status = write_polygon(polygon, 0, 0, 0, 0);
            if (status)
                return status;
        }

        for (const PolygonIterate &it : shape.polygon_iterates)
        {
            status = write_polygon(it.polygon, it.num_x, it.num_y, to_um(it.space_x), to_um(it.space_y));
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
