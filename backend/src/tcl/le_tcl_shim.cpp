#include "le_tcl_shim.hpp"

#include "api.hpp"

#include <cstdint>
#include <sstream>
#include <string>

namespace
{
    LeHandle *session()
    {
        static LeHandle *handle = le_create();
        return handle;
    }

    // See le_tcl_shim.hpp's "IDs" comment for why every id crossing this
    // shim is packed into one int64_t rather than wrapped with a custom
    // SWIG struct typemap. Generic over every LeXxxId (all identical
    // {uint32_t index, generation} layouts) rather than one pack/unpack
    // pair per type.
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
    // e.g. terminal_property_value/search_terminal/shape_rect_at below).
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

    // Packed-id-list-as-space-separated-string, shared by every
    // search_x/x_shapes function below - already a well-formed Tcl list
    // with no escaping needed, see le_tcl_shim.hpp's own comment on
    // search_terminal for why.
    template <typename IdT>
    std::string join_ids(int32_t count, IdT (*at)(LeHandle *, int32_t))
    {
        std::ostringstream out;
        for (int32_t i = 0; i < count; ++i)
        {
            if (i > 0)
            {
                out << ' ';
            }
            out << pack(at(session(), i));
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

// --- Terminal ---

long long create_terminal_cmd(long long abstract_id, const char *name, int direction)
{
    return pack(le_create_terminal(session(), unpack<LeAbstractId>(abstract_id), name, direction));
}

int terminal_property_count(long long id)
{
    return le_terminal_property_count(session(), unpack<LeTerminalId>(id));
}

const char *terminal_property_name(long long id, int index)
{
    return le_terminal_property_at(session(), unpack<LeTerminalId>(id), index).name;
}

const char *terminal_property_value(long long id, int index)
{
    return return_string(format_property_value(le_terminal_property_at(session(), unpack<LeTerminalId>(id), index)));
}

int set_terminal_name(long long id, const char *name)
{
    return le_set_terminal_name(session(), unpack<LeTerminalId>(id), name);
}

int set_terminal_direction_cmd(long long id, int direction)
{
    return le_set_terminal_direction(session(), unpack<LeTerminalId>(id), direction);
}

int delete_terminal(long long id)
{
    return le_delete_terminal(session(), unpack<LeTerminalId>(id));
}

const char *search_terminal(const char *filter_expression)
{
    int32_t count = le_search_terminal(session(), filter_expression);
    if (count <= 0)
    {
        return return_string("");
    }
    return return_string(join_ids<LeTerminalId>(count, le_search_result_terminal_at));
}

// --- TerminalPort ---

long long create_terminal_port_cmd(long long terminal_id)
{
    return pack(le_create_terminal_port(session(), unpack<LeTerminalId>(terminal_id)));
}

int terminal_port_property_count(long long id)
{
    return le_terminal_port_property_count(session(), unpack<LeTerminalPortId>(id));
}

const char *terminal_port_property_name(long long id, int index)
{
    return le_terminal_port_property_at(session(), unpack<LeTerminalPortId>(id), index).name;
}

const char *terminal_port_property_value(long long id, int index)
{
    return return_string(format_property_value(le_terminal_port_property_at(session(), unpack<LeTerminalPortId>(id), index)));
}

int delete_terminal_port(long long id)
{
    return le_delete_terminal_port(session(), unpack<LeTerminalPortId>(id));
}

const char *search_terminal_port(const char *filter_expression)
{
    int32_t count = le_search_terminal_port(session(), filter_expression);
    if (count <= 0)
    {
        return return_string("");
    }
    return return_string(join_ids<LeTerminalPortId>(count, le_search_result_terminal_port_at));
}

const char *terminal_port_shapes(long long id)
{
    LeTerminalPortId port_id = unpack<LeTerminalPortId>(id);
    int32_t count = le_terminal_port_shape_count(session(), port_id);
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
        out << pack(le_terminal_port_shape_at(session(), port_id, i));
    }
    return return_string(out.str());
}

// --- Obstruction ---

long long create_obstruction_cmd(long long abstract_id)
{
    return pack(le_create_obstruction(session(), unpack<LeAbstractId>(abstract_id)));
}

int obstruction_property_count(long long id)
{
    return le_obstruction_property_count(session(), unpack<LeObstructionId>(id));
}

const char *obstruction_property_name(long long id, int index)
{
    return le_obstruction_property_at(session(), unpack<LeObstructionId>(id), index).name;
}

const char *obstruction_property_value(long long id, int index)
{
    return return_string(format_property_value(le_obstruction_property_at(session(), unpack<LeObstructionId>(id), index)));
}

int delete_obstruction(long long id)
{
    return le_delete_obstruction(session(), unpack<LeObstructionId>(id));
}

const char *search_obstruction(const char *filter_expression)
{
    int32_t count = le_search_obstruction(session(), filter_expression);
    if (count <= 0)
    {
        return return_string("");
    }
    return return_string(join_ids<LeObstructionId>(count, le_search_result_obstruction_at));
}

const char *obstruction_shapes(long long id)
{
    LeObstructionId obstruction_id = unpack<LeObstructionId>(id);
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
        out << pack(le_obstruction_shape_at(session(), obstruction_id, i));
    }
    return return_string(out.str());
}

// --- Abstract boundary ---

int update_abstract_boundary_cmd(long long abstract_id, const double *points_um, int32_t point_coord_count)
{
    return le_update_abstract_boundary(session(), unpack<LeAbstractId>(abstract_id), points_um, point_coord_count);
}

// --- Shape ---

long long create_terminal_port_shape_cmd(long long port_id, const char *layer_name)
{
    return pack(le_create_terminal_port_shape(session(), unpack<LeTerminalPortId>(port_id), layer_name));
}

long long create_obstruction_shape_cmd(long long obstruction_id, const char *layer_name)
{
    return pack(le_create_obstruction_shape(session(), unpack<LeObstructionId>(obstruction_id), layer_name));
}

const char *shape_layer_name(long long id)
{
    return le_shape_layer_name(session(), unpack<LeShapeId>(id));
}

int set_shape_layer_name(long long id, const char *layer_name)
{
    return le_set_shape_layer_name(session(), unpack<LeShapeId>(id), layer_name);
}

int delete_shape(long long id)
{
    return le_delete_shape(session(), unpack<LeShapeId>(id));
}

int shape_rect_count(long long id)
{
    return le_shape_rect_count(session(), unpack<LeShapeId>(id));
}

const char *shape_rect_at(long long id, int index)
{
    LeRectUm rect = le_shape_rect_at(session(), unpack<LeShapeId>(id), index);
    std::ostringstream out;
    out << rect.ll_x_um << ' ' << rect.ll_y_um << ' ' << rect.ur_x_um << ' ' << rect.ur_y_um;
    return return_string(out.str());
}

int add_shape_rect_cmd(long long id, double ll_x_um, double ll_y_um, double ur_x_um, double ur_y_um)
{
    return le_add_shape_rect(session(), unpack<LeShapeId>(id), ll_x_um, ll_y_um, ur_x_um, ur_y_um);
}

int remove_shape_rect(long long id, int index)
{
    return le_remove_shape_rect(session(), unpack<LeShapeId>(id), index);
}

int shape_polygon_count(long long id)
{
    return le_shape_polygon_count(session(), unpack<LeShapeId>(id));
}

int shape_polygon_point_count(long long id, int polygon_index)
{
    return le_shape_polygon_point_count(session(), unpack<LeShapeId>(id), polygon_index);
}

const char *shape_polygon_point_at(long long id, int polygon_index, int point_index)
{
    LePointUm pt = le_shape_polygon_point_at(session(), unpack<LeShapeId>(id), polygon_index, point_index);
    std::ostringstream out;
    out << pt.x_um << ' ' << pt.y_um;
    return return_string(out.str());
}

int add_shape_polygon_cmd(long long id, const double *points_um, int32_t point_coord_count)
{
    return le_add_shape_polygon(session(), unpack<LeShapeId>(id), points_um, point_coord_count);
}

int remove_shape_polygon(long long id, int polygon_index)
{
    return le_remove_shape_polygon(session(), unpack<LeShapeId>(id), polygon_index);
}

int shape_path_count(long long id)
{
    return le_shape_path_count(session(), unpack<LeShapeId>(id));
}

double shape_path_width_um(long long id, int path_index)
{
    return le_shape_path_width_um(session(), unpack<LeShapeId>(id), path_index);
}

int shape_path_point_count(long long id, int path_index)
{
    return le_shape_path_point_count(session(), unpack<LeShapeId>(id), path_index);
}

const char *shape_path_point_at(long long id, int path_index, int point_index)
{
    LePointUm pt = le_shape_path_point_at(session(), unpack<LeShapeId>(id), path_index, point_index);
    std::ostringstream out;
    out << pt.x_um << ' ' << pt.y_um;
    return return_string(out.str());
}

int add_shape_path_cmd(long long id, double width_um, const double *points_um, int32_t point_coord_count)
{
    return le_add_shape_path(session(), unpack<LeShapeId>(id), width_um, points_um, point_coord_count);
}

int remove_shape_path(long long id, int path_index)
{
    return le_remove_shape_path(session(), unpack<LeShapeId>(id), path_index);
}
