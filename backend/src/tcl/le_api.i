// Phase 0 spike proved SWIG can wrap a Tcl-facing shim into a loadable
// Tcl extension. Phase 5 (TCL_EXPLORATION.md) extends that to the full
// Phase 4 CRUD/search surface and adds the one genuinely new SWIG
// mechanism this phase needs: a custom typemap converting a Tcl list of
// doubles into the flat `(const double*, int32_t count)` pairs
// le_add_shape_polygon/le_add_shape_path/le_update_abstract_boundary all
// take, so a Tcl caller passes `{0 0 10 0 10 10}` rather than
// pre-flattening into a raw C array by hand.
//
// Wraps le_tcl_shim.hpp, not api.hpp directly - see this file's own
// Phase 0 header comment (still accurate) for why: api.hpp's
// handle-per-call design is for Dart FFI; the Tcl-facing surface item 15
// wants has no visible handle and uses domain verb names, not
// le_<verb>_<noun> ones. le_tcl_shim.hpp/.cpp supplies both by calling
// into api.hpp underneath a hidden process-global LeHandle*.
//
// Every database id (LeAbstractId, LeTerminalId, ...) crosses this
// boundary packed into a plain `long long`, not as a wrapped C struct -
// see le_tcl_shim.hpp's own "IDs" comment for why (avoids 7 custom
// struct typemaps; `long long`/`int32_t` are fundamental/portable types
// SWIG already knows how to marshal to/from a Tcl integer). This means
// no id-related typemap is needed here at all - only the coordinate-list
// one below.
//
// `-flag value` commands (create_terminal, update_abstract_boundary,
// add_shape_rect, ...) are declared here under their internal `*_cmd`
// name, taking plain positional arguments - SWIG-wrapped C++ functions
// are always positional. le_tcl_procs.tcl supplies the real, flag-
// parsing command name on top of each (see its own header comment for
// why that split exists).
%module le_tcl

%{
#include "le_tcl_shim.hpp"
#include <vector>
%}

// Gives SWIG a portable definition of int32_t/int64_t (and their Tcl
// marshalling) independent of whatever <cstdint> resolves to on this
// toolchain - needed so the typemap pattern below (and the plain
// int32_t point_coord_count/coord_count parameters elsewhere) actually
// matches.
%include <stdint.i>

// --- Coordinate-list typemap (Phase 5's own reason to exist) ---
//
// Converts a Tcl list of doubles ($input) into the temp buffer's backing
// storage, then hands the CRUD shim function a plain (const double*,
// int32_t count) pair pointing into it. `temp` is declared as a typemap
// local variable (the third parens block) so it lives for the whole
// wrapper function body, not just this typemap's own code block -
// otherwise $1 would point into a vector already destroyed by the time
// the wrapped function runs.
%typemap(in) (const double *POINTS_ARRAY_UM, int32_t POINTS_COORD_COUNT) (std::vector<double> temp) {
    int listc;
    Tcl_Obj **listv;
    if (Tcl_ListObjGetElements(interp, $input, &listc, &listv) != TCL_OK) {
        SWIG_fail;
    }
    temp.resize(static_cast<size_t>(listc));
    for (int i = 0; i < listc; i++) {
        double val;
        if (Tcl_GetDoubleFromObj(interp, listv[i], &val) != TCL_OK) {
            SWIG_fail;
        }
        temp[static_cast<size_t>(i)] = val;
    }
    $1 = temp.empty() ? nullptr : temp.data();
    $2 = static_cast<int32_t>(listc);
}

// Applied to every real (const double*, int32_t count) parameter pair
// this module wraps: add_shape_polygon_cmd/add_shape_path_cmd's and
// update_abstract_boundary_cmd's `points_um`/`point_coord_count` (see
// le_tcl_shim.hpp - api.hpp's own `coords_um`/`coord_count` wording for
// the boundary case was normalized to the same points_um/
// point_coord_count names in the shim, so one %apply covers every
// caller).
%apply (const double *POINTS_ARRAY_UM, int32_t POINTS_COORD_COUNT) { (const double *points_um, int32_t point_coord_count) };

int read_lef(const char *path);
int design_count();
const char *design_name(int index);
int message_count();
const char *message_at(int index);
void set_viewport_size_cmd(int width_px, int height_px);
int viewport_width();
int viewport_height();

long long design_abstract_id(int design_index);

// --- Terminal ---
long long create_terminal_cmd(long long abstract_id, const char *name, int direction);
int terminal_property_count(long long id);
const char *terminal_property_name(long long id, int index);
const char *terminal_property_value(long long id, int index);
int set_terminal_name(long long id, const char *name);
int set_terminal_direction_cmd(long long id, int direction);
int delete_terminal(long long id);
const char *search_terminal(const char *filter_expression);

// --- TerminalPort ---
long long create_terminal_port_cmd(long long terminal_id);
int terminal_port_property_count(long long id);
const char *terminal_port_property_name(long long id, int index);
const char *terminal_port_property_value(long long id, int index);
int delete_terminal_port(long long id);
const char *search_terminal_port(const char *filter_expression);
const char *terminal_port_shapes(long long id);

// --- Obstruction ---
long long create_obstruction_cmd(long long abstract_id);
int obstruction_property_count(long long id);
const char *obstruction_property_name(long long id, int index);
const char *obstruction_property_value(long long id, int index);
int delete_obstruction(long long id);
const char *search_obstruction(const char *filter_expression);
const char *obstruction_shapes(long long id);

// --- Abstract boundary ---
int update_abstract_boundary_cmd(long long abstract_id, const double *points_um, int32_t point_coord_count);

// --- Shape ---
long long create_terminal_port_shape_cmd(long long port_id, const char *layer_name);
long long create_obstruction_shape_cmd(long long obstruction_id, const char *layer_name);
const char *shape_layer_name(long long id);
int set_shape_layer_name(long long id, const char *layer_name);
int delete_shape(long long id);

int shape_rect_count(long long id);
const char *shape_rect_at(long long id, int index);
int add_shape_rect_cmd(long long id, double ll_x_um, double ll_y_um, double ur_x_um, double ur_y_um);
int remove_shape_rect(long long id, int index);

int shape_polygon_count(long long id);
int shape_polygon_point_count(long long id, int polygon_index);
const char *shape_polygon_point_at(long long id, int polygon_index, int point_index);
int add_shape_polygon_cmd(long long id, const double *points_um, int32_t point_coord_count);
int remove_shape_polygon(long long id, int polygon_index);

int shape_path_count(long long id);
double shape_path_width_um(long long id, int path_index);
int shape_path_point_count(long long id, int path_index);
const char *shape_path_point_at(long long id, int path_index, int point_index);
int add_shape_path_cmd(long long id, double width_um, const double *points_um, int32_t point_coord_count);
int remove_shape_path(long long id, int path_index);
