#include "le_tcl_shim.hpp"

#include "api.hpp"

#include <charconv>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>

namespace
{
    // Set by set_session_handle() (see le_tcl_shim.hpp) - an externally-
    // owned handle wins over the lazy self-create below whenever one has
    // been injected, e.g. by a Flutter-embedded Tcl console sharing the
    // Dart-owned LeHandle* (see TCL_EXPLORATION.md's show_gui section).
    LeHandle *&injected_handle()
    {
        static LeHandle *handle = nullptr;
        return handle;
    }

    LeHandle *session()
    {
        if (injected_handle() != nullptr)
        {
            return injected_handle();
        }
        static LeHandle *handle = le_create();
        return handle;
    }

    // See le_tcl_shim.hpp's "IDs" comment for why AbstractId/DesignId
    // cross this shim packed into one int64_t rather than wrapped with a
    // custom SWIG struct typemap. Generic over every LeXxxId (all
    // identical {uint32_t index, generation} layouts) rather than one
    // pack/unpack pair per type - also still used internally here for
    // TerminalPortId/ObstructionId/ShapeId, whose friendly string form is
    // just this same packed integer with a type prefix (see
    // resolve_numeric_friendly_id/format_numeric_friendly_id below).
    template <typename IdT>
    int64_t pack(IdT id)
    {
        return (static_cast<int64_t>(id.generation) << 32) | static_cast<int64_t>(id.index);
    }

    template <typename IdT>
    IdT unpack(int64_t packed)
    {
        IdT id{};
        id.index = static_cast<uint32_t>(static_cast<uint64_t>(packed) & 0xFFFFFFFFu);
        id.generation = static_cast<uint32_t>((static_cast<uint64_t>(packed) >> 32) & 0xFFFFFFFFu);
        return id;
    }

    // Shared scratch buffer for shim functions that format and return a
    // `const char*` built on the fly here (not memory owned by Root, see
    // e.g. terminal_property_value/get_terminals_at/shape_rect_at below).
    // Safe to share across every such function despite the project's
    // usual "valid until the next call" pointer convention: SWIG's Tcl
    // typemap for `const char*` copies the bytes into a new Tcl_Obj
    // immediately on return, before the *next* shim call (a separate
    // Tcl statement) can ever run - so two of these functions called
    // back-to-back from Tcl never actually race over this buffer.
    std::string &scratch()
    {
        static thread_local std::string buffer;
        return buffer;
    }

    const char *return_string(std::string value)
    {
        scratch() = std::move(value);
        return scratch().c_str();
    }

    // --- Friendly id formatting/parsing (see le_tcl_shim.hpp's own "IDs"
    // comment for the full contract) ---

    constexpr std::string_view kTerminalPrefix = "terminal:";
    constexpr std::string_view kObstructionPrefix = "obstruction:";
    constexpr std::string_view kTerminalPortPrefix = "terminal_port:";
    constexpr std::string_view kShapePrefix = "shape:";

    std::string format_terminal_id(const char *name)
    {
        return std::string(kTerminalPrefix) + (name ? name : "");
    }

    // Fixed-prefix compare (not "find first colon") so a LEF-legal name
    // containing ':' itself can't misparse - kTerminalPrefix always
    // matches the whole leading literal, everything after is the raw
    // name verbatim, passed straight to le_terminal_by_name (itself
    // already scoped to the current view, same as le_get_terminals).
    LeTerminalId resolve_terminal_id(const char *s)
    {
        const LeTerminalId invalid{.index = UINT32_MAX, .generation = 0};
        if (!s)
            return invalid;
        std::string_view sv(s);
        if (sv.substr(0, kTerminalPrefix.size()) != kTerminalPrefix)
            return invalid;
        return le_terminal_by_name(session(), std::string(sv.substr(kTerminalPrefix.size())).c_str());
    }

    // Obstruction/TerminalPort/Shape have no name field - their friendly
    // id is just their existing packed integer, type-prefixed for
    // self-description. A malformed string or wrong-type prefix (e.g. a
    // "terminal_port:..." id passed where "shape:..." is expected) parses to the
    // same invalid sentinel as an unknown id - api.hpp's own not-found
    // paths already degrade gracefully for that, so no separate error
    // path is needed here.
    template <typename IdT>
    IdT resolve_numeric_friendly_id(const char *s, std::string_view prefix)
    {
        const IdT invalid{.index = UINT32_MAX, .generation = 0};
        if (!s)
            return invalid;
        std::string_view sv(s);
        if (sv.substr(0, prefix.size()) != prefix)
            return invalid;
        std::string_view digits = sv.substr(prefix.size());
        if (digits.empty())
            return invalid;
        int64_t packed = 0;
        const auto result = std::from_chars(digits.data(), digits.data() + digits.size(), packed);
        if (result.ec != std::errc{} || result.ptr != digits.data() + digits.size())
            return invalid;
        return unpack<IdT>(packed);
    }

    template <typename IdT>
    std::string format_numeric_friendly_id(IdT id, std::string_view prefix)
    {
        return std::string(prefix) + std::to_string(pack(id));
    }

    LeObstructionId resolve_obstruction_id(const char *s) { return resolve_numeric_friendly_id<LeObstructionId>(s, kObstructionPrefix); }
    LeTerminalPortId resolve_terminal_port_id(const char *s) { return resolve_numeric_friendly_id<LeTerminalPortId>(s, kTerminalPortPrefix); }
    LeShapeId resolve_shape_id(const char *s) { return resolve_numeric_friendly_id<LeShapeId>(s, kShapePrefix); }

    std::string format_obstruction_id(LeObstructionId id) { return format_numeric_friendly_id(id, kObstructionPrefix); }
    std::string format_terminal_port_id(LeTerminalPortId id) { return format_numeric_friendly_id(id, kTerminalPortPrefix); }
    std::string format_shape_id(LeShapeId id) { return format_numeric_friendly_id(id, kShapePrefix); }

    // Tcl is "everything is a string" by design (`expr {$v + 1}` works on
    // a numeric string exactly like a native int) - see le_tcl_shim.hpp's
    // "property tables and search results" comment for why every
    // property value crossing this shim is pre-stringified rather than
    // exposed with its LePropertyType tag.
    std::string format_property_value(const LeProperty &prop)
    {
        switch (prop.type)
        {
        case LE_PROPERTY_TYPE_STRING:
            return prop.string_value ? prop.string_value : "";
        case LE_PROPERTY_TYPE_INT:
            return std::to_string(prop.int_value);
        case LE_PROPERTY_TYPE_DOUBLE:
            return std::to_string(prop.double_value);
        default:
            return "";
        }
    }

    // Friendly-numeric-id-list-as-space-separated-string, shared by
    // get_terminal_ports/get_obstructions/terminal_port_shapes/
    // obstruction_shapes below - every token is purely numeric
    // ("terminal_port:N"/"obstruction:N"/"shape:N"), never LEF-authored text, so
    // this is provably a well-formed Tcl list with no escaping needed -
    // see le_tcl_shim.hpp's own comment on why get_terminals_cmd/_at is
    // shaped differently.
    template <typename IdT>
    std::string join_friendly_ids(int32_t count, IdT (*at)(LeHandle *, int32_t), std::string_view prefix)
    {
        std::ostringstream out;
        for (int32_t i = 0; i < count; ++i)
        {
            if (i > 0)
            {
                out << ' ';
            }
            out << format_numeric_friendly_id(at(session(), i), prefix);
        }
        return out.str();
    }
}

int read_lef(const char *path)
{
    return le_read_lef(session(), path);
}

int design_count()
{
    return le_design_count(session());
}

const char *design_name(int index)
{
    return le_design_name(session(), index);
}

int message_count()
{
    return le_message_count(session());
}

const char *message_at(int index)
{
    return le_message_at(session(), index);
}

void set_viewport_size_cmd(int width_px, int height_px)
{
    le_set_viewport_size(session(), width_px, height_px);
}

int viewport_width()
{
    return le_render_pixel_buffer(session()).width;
}

int viewport_height()
{
    return le_render_pixel_buffer(session()).height;
}

long long design_abstract_id(int design_index)
{
    return pack(le_library_design_at(session(), 0, design_index).abstract_id);
}

long long design_by_name(const char *name)
{
    return pack(le_design_by_name(session(), name));
}

int set_current_design_cmd(long long design_id)
{
    return le_set_current_design_by_id(session(), unpack<LeDesignId>(design_id));
}

void set_session_handle(long long handle_address)
{
    injected_handle() = reinterpret_cast<LeHandle *>(static_cast<uintptr_t>(handle_address));
}

// --- Terminal ---

const char *create_terminal_cmd(long long abstract_id, const char *name, int direction)
{
    LeTerminalId id = le_create_terminal(session(), unpack<LeAbstractId>(abstract_id), name, direction);
    if (id.index == UINT32_MAX)
        return return_string("");
    return return_string(format_terminal_id(name));
}

int terminal_property_count(const char *id)
{
    return le_terminal_property_count(session(), resolve_terminal_id(id));
}

const char *terminal_property_name(const char *id, int index)
{
    return le_terminal_property_at(session(), resolve_terminal_id(id), index).name;
}

const char *terminal_property_value(const char *id, int index)
{
    return return_string(format_property_value(le_terminal_property_at(session(), resolve_terminal_id(id), index)));
}

int set_terminal_name(const char *id, const char *name)
{
    return le_set_terminal_name(session(), resolve_terminal_id(id), name);
}

int set_terminal_direction_cmd(const char *id, int direction)
{
    return le_set_terminal_direction(session(), resolve_terminal_id(id), direction);
}

int delete_terminal(const char *id)
{
    return le_delete_terminal(session(), resolve_terminal_id(id));
}

int get_terminals_cmd(const char *filter_expression)
{
    return le_get_terminals(session(), filter_expression);
}

const char *get_terminals_at(int index)
{
    LeTerminalId id = le_search_result_terminal_at(session(), index);
    if (id.index == UINT32_MAX)
        return return_string("");
    const char *name = le_terminal_name(session(), id);
    if (!name)
        return return_string("");
    return return_string(format_terminal_id(name));
}

// --- TerminalPort ---

const char *create_terminal_port_cmd(const char *terminal_id)
{
    LeTerminalPortId id = le_create_terminal_port(session(), resolve_terminal_id(terminal_id));
    if (id.index == UINT32_MAX)
        return return_string("");
    return return_string(format_terminal_port_id(id));
}

int terminal_port_property_count(const char *id)
{
    return le_terminal_port_property_count(session(), resolve_terminal_port_id(id));
}

const char *terminal_port_property_name(const char *id, int index)
{
    return le_terminal_port_property_at(session(), resolve_terminal_port_id(id), index).name;
}

const char *terminal_port_property_value(const char *id, int index)
{
    return return_string(format_property_value(le_terminal_port_property_at(session(), resolve_terminal_port_id(id), index)));
}

int delete_terminal_port(const char *id)
{
    return le_delete_terminal_port(session(), resolve_terminal_port_id(id));
}

const char *get_terminal_ports(const char *filter_expression)
{
    int32_t count = le_get_terminal_ports(session(), filter_expression);
    if (count <= 0)
    {
        return return_string("");
    }
    return return_string(join_friendly_ids<LeTerminalPortId>(count, le_search_result_terminal_port_at, kTerminalPortPrefix));
}

const char *terminal_port_shapes(const char *id)
{
    LeTerminalPortId terminal_port_id = resolve_terminal_port_id(id);
    int32_t count = le_terminal_port_shape_count(session(), terminal_port_id);
    if (count <= 0)
    {
        return return_string("");
    }
    std::ostringstream out;
    for (int32_t i = 0; i < count; ++i)
    {
        if (i > 0)
        {
            out << ' ';
        }
        out << format_shape_id(le_terminal_port_shape_at(session(), terminal_port_id, i));
    }
    return return_string(out.str());
}

// --- Obstruction ---

const char *create_obstruction_cmd(long long abstract_id)
{
    LeObstructionId id = le_create_obstruction(session(), unpack<LeAbstractId>(abstract_id));
    if (id.index == UINT32_MAX)
        return return_string("");
    return return_string(format_obstruction_id(id));
}

int obstruction_property_count(const char *id)
{
    return le_obstruction_property_count(session(), resolve_obstruction_id(id));
}

const char *obstruction_property_name(const char *id, int index)
{
    return le_obstruction_property_at(session(), resolve_obstruction_id(id), index).name;
}

const char *obstruction_property_value(const char *id, int index)
{
    return return_string(format_property_value(le_obstruction_property_at(session(), resolve_obstruction_id(id), index)));
}

int delete_obstruction(const char *id)
{
    return le_delete_obstruction(session(), resolve_obstruction_id(id));
}

const char *get_obstructions(const char *filter_expression)
{
    int32_t count = le_get_obstructions(session(), filter_expression);
    if (count <= 0)
    {
        return return_string("");
    }
    return return_string(join_friendly_ids<LeObstructionId>(count, le_search_result_obstruction_at, kObstructionPrefix));
}

const char *obstruction_shapes(const char *id)
{
    LeObstructionId obstruction_id = resolve_obstruction_id(id);
    int32_t count = le_obstruction_shape_count(session(), obstruction_id);
    if (count <= 0)
    {
        return return_string("");
    }
    std::ostringstream out;
    for (int32_t i = 0; i < count; ++i)
    {
        if (i > 0)
        {
            out << ' ';
        }
        out << format_shape_id(le_obstruction_shape_at(session(), obstruction_id, i));
    }
    return return_string(out.str());
}

// --- Abstract boundary ---

int update_abstract_boundary_cmd(long long abstract_id, const double *points_um, int32_t point_coord_count)
{
    return le_update_abstract_boundary(session(), unpack<LeAbstractId>(abstract_id), points_um, point_coord_count);
}

// --- Shape ---

const char *create_terminal_port_shape_cmd(const char *terminal_port_id, const char *layer_name)
{
    LeShapeId id = le_create_terminal_port_shape(session(), resolve_terminal_port_id(terminal_port_id), layer_name);
    if (id.index == UINT32_MAX)
        return return_string("");
    return return_string(format_shape_id(id));
}

const char *create_obstruction_shape_cmd(const char *obstruction_id, const char *layer_name)
{
    LeShapeId id = le_create_obstruction_shape(session(), resolve_obstruction_id(obstruction_id), layer_name);
    if (id.index == UINT32_MAX)
        return return_string("");
    return return_string(format_shape_id(id));
}

const char *shape_layer_name(const char *id)
{
    return le_shape_layer_name(session(), resolve_shape_id(id));
}

int set_shape_layer_name(const char *id, const char *layer_name)
{
    return le_set_shape_layer_name(session(), resolve_shape_id(id), layer_name);
}

int delete_shape(const char *id)
{
    return le_delete_shape(session(), resolve_shape_id(id));
}

int shape_rect_count(const char *id)
{
    return le_shape_rect_count(session(), resolve_shape_id(id));
}

const char *shape_rect_at(const char *id, int index)
{
    LeRectUm rect = le_shape_rect_at(session(), resolve_shape_id(id), index);
    std::ostringstream out;
    out << rect.ll_x_um << ' ' << rect.ll_y_um << ' ' << rect.ur_x_um << ' ' << rect.ur_y_um;
    return return_string(out.str());
}

int add_shape_rect_cmd(const char *id, double ll_x_um, double ll_y_um, double ur_x_um, double ur_y_um)
{
    return le_add_shape_rect(session(), resolve_shape_id(id), ll_x_um, ll_y_um, ur_x_um, ur_y_um);
}

int remove_shape_rect(const char *id, int index)
{
    return le_remove_shape_rect(session(), resolve_shape_id(id), index);
}

int shape_polygon_count(const char *id)
{
    return le_shape_polygon_count(session(), resolve_shape_id(id));
}

int shape_polygon_point_count(const char *id, int polygon_index)
{
    return le_shape_polygon_point_count(session(), resolve_shape_id(id), polygon_index);
}

const char *shape_polygon_point_at(const char *id, int polygon_index, int point_index)
{
    LePointUm pt = le_shape_polygon_point_at(session(), resolve_shape_id(id), polygon_index, point_index);
    std::ostringstream out;
    out << pt.x_um << ' ' << pt.y_um;
    return return_string(out.str());
}

int add_shape_polygon_cmd(const char *id, const double *points_um, int32_t point_coord_count)
{
    return le_add_shape_polygon(session(), resolve_shape_id(id), points_um, point_coord_count);
}

int remove_shape_polygon(const char *id, int polygon_index)
{
    return le_remove_shape_polygon(session(), resolve_shape_id(id), polygon_index);
}

int shape_path_count(const char *id)
{
    return le_shape_path_count(session(), resolve_shape_id(id));
}

double shape_path_width_um(const char *id, int path_index)
{
    return le_shape_path_width_um(session(), resolve_shape_id(id), path_index);
}

int shape_path_point_count(const char *id, int path_index)
{
    return le_shape_path_point_count(session(), resolve_shape_id(id), path_index);
}

const char *shape_path_point_at(const char *id, int path_index, int point_index)
{
    LePointUm pt = le_shape_path_point_at(session(), resolve_shape_id(id), path_index, point_index);
    std::ostringstream out;
    out << pt.x_um << ' ' << pt.y_um;
    return return_string(out.str());
}

int add_shape_path_cmd(const char *id, double width_um, const double *points_um, int32_t point_coord_count)
{
    return le_add_shape_path(session(), resolve_shape_id(id), width_um, points_um, point_coord_count);
}

int remove_shape_path(const char *id, int path_index)
{
    return le_remove_shape_path(session(), resolve_shape_id(id), path_index);
}
