#pragma once

#include <cstdint>

// Tcl-facing session shim (see TCL_EXPLORATION.md's "Tcl ergonomics
// layer" section for the full rationale). Owns one process-global
// LeHandle* (lazily created on first use - the single-process/
// shared-state model TCL_EXPLORATION.md's round-1 recommendation already
// settled on for GUI<->batch-mode connection), and exposes bare,
// handle-free functions named with UPDATES.md item 15's own domain
// vocabulary (read_lef, not le_read_lef) - these are what le_api.i wraps
// into Tcl commands, not api.hpp directly.
//
// Scalar/no-flag commands (read_lef, design_count, ...) are exposed
// under their final Tcl name directly. Anything item 15 shows called
// with `-flag value` syntax (e.g. `set_viewport_size -width W -height H`,
// `create_terminal -name IN0 -direction IN`) is exposed here under an
// internal `*_cmd` name taking plain positional arguments instead:
// SWIG-wrapped C++ functions are always positional, so Tcl's own
// `-flag value` calling convention has to be parsed in Tcl itself, not
// here - a matching thin proc in le_tcl_procs.tcl does that parsing and
// then calls the `*_cmd` form positionally. This split (C++ shim owns
// session state + command logic; a thin Tcl proc layer owns flag
// parsing) is the answer to "is smoke_test.tcl representative of the
// real interface?" - it wasn't; Phase 0 fixed that, and every Phase 4
// CRUD command below follows the same shape.
//
// --- IDs (Phase 5) ---
// Every database id (LeAbstractId, LeTerminalId, ...) is a plain
// {uint32_t index, generation} struct in api.hpp. Rather than write a
// custom SWIG typemap per id type (7 of them), every id crossing this
// shim is packed into one int64_t (generation in the high 32 bits, index
// in the low 32 bits, via pack_id/unpack_id in le_tcl_shim.cpp) -
// int64_t is a fundamental type SWIG's stdint.i already marshals to/from
// a plain Tcl integer with zero custom typemap code. kInvalidId (-1)
// marks an invalid id, since a packed valid id (index != UINT32_MAX) can
// never equal -1 (all 64 bits set) - see pack_id's own comment for why.
//
// --- Property tables and search results (Phase 5) ---
// Deliberately NOT built as Tcl lists/dicts in C++ here - that needs
// correct Tcl quoting (a value containing spaces/braces has to be
// list-escaped), which is Tcl's own job (`lappend`/`dict set` do it
// correctly by construction) and error-prone to hand-roll in C++. This
// shim only exposes count+by-index accessors, mirroring api.hpp's own
// shape exactly; le_tcl_procs.tcl loops over them to build the ergonomic
// dict/list a real Tcl caller wants (see e.g. `terminal_properties`).
// Every property value is exposed pre-stringified (`*_property_value`),
// not int/double/string-tagged - Tcl is "everything is a string" by its
// own design (`expr {$v + 1}` works on a numeric string exactly the same
// as a native int), so preserving LeProperty's STRING/INT/DOUBLE tag
// across this boundary isn't worth the extra accessors it would take.

int read_lef(const char *path);
int design_count();
const char *design_name(int index);
int message_count();
const char *message_at(int index);

/// @brief Positional form behind `set_viewport_size -width W -height H`
/// (see le_tcl_procs.tcl). Demonstrates the -flag-parsing split
/// end-to-end using an existing api.hpp call (le_set_viewport_size).
void set_viewport_size_cmd(int width_px, int height_px);

/// @brief Round-trips through le_render_pixel_buffer() to prove
/// set_viewport_size_cmd() actually took effect, without wrapping the
/// whole LePixelBuffer struct (its `data` pointer would need its own
/// typemap - out of scope for this demonstration).
int viewport_width();
int viewport_height();

/// @brief The AbstractId of the design at `design_index` (0..design_count()-1,
/// same flat indexing as design_name) - the way a script gets an id to
/// pass as `abstract_id` to create_terminal/create_obstruction/
/// update_abstract_boundary below. Scoped to the first Library only
/// (le_library_design_at's library_index=0) - correct as long as only
/// one LEF file/library is in play, same assumption this project's
/// single-shared-Technology convention already makes elsewhere. Superseded
/// for the "select a view to work in" use case by open_design below
/// (UPDATES.md item 17) - kept as-is since scripts may still want a raw
/// AbstractId without changing the session's current-view state.
/// Returns kInvalidId if index is out of range.
long long design_abstract_id(int design_index);

/// @brief Positional form behind `open_design NAME -view abstract` (see
/// le_tcl_procs.tcl) - resolves NAME to a LeDesignId. Returns kInvalidId
/// if no Design named `name` is loaded on this session.
long long design_by_name(const char *name);

/// @brief Select the Design `design_id` (as returned by design_by_name)
/// as this session's current view (UPDATES.md item 17) - every
/// subsequent get_terminals/get_obstructions/get_terminal_ports call is
/// scoped to its Abstract. Returns 0 on success, nonzero if design_id
/// doesn't name a Design on this session.
int set_current_design_cmd(long long design_id);

/// @brief Sentinel for "no such id" - see this header's own "IDs"
/// comment. Every api.hpp failure path returns an id struct with
/// index == UINT32_MAX and generation == 0 (never left uninitialized -
/// see TCL_EXPLORATION.md's "zero-init bug" note), which pack() (in
/// le_tcl_shim.cpp) always turns into exactly this value - not -1
/// (0xFFFFFFFFFFFFFFFF), which would require generation == UINT32_MAX
/// too.
constexpr long long kInvalidId = 0xFFFFFFFFLL;

/// @brief Point every subsequent shim call at an externally-owned
/// LeHandle* (packed the same way every other id crosses this shim -
/// see the "IDs" comment above) instead of the shim's own lazily-self-
/// created one - e.g. the Dart-owned handle a Flutter-embedded Tcl
/// console shares (see TCL_EXPLORATION.md's show_gui section), so a Tcl
/// command mutates the exact same database the GUI is already
/// rendering, not an unrelated standalone one. The shim never destroys
/// an injected handle - ownership (le_destroy()) stays with whoever
/// created it, same as le_shell's own self-created handle is never
/// explicitly destroyed either (process-lifetime). Must be called (if
/// at all) before any other shim function that touches session() - the
/// self-created handle is lazily latched on session()'s first call and
/// is never revisited afterward, injected or not.
void set_session_handle(long long handle_address);

// --- Terminal CRUD + search ---

/// @brief Positional form behind `create_terminal -abstract ID -name NAME
/// -direction DIR` (see le_tcl_procs.tcl; `direction` is one of
/// INPUT/OUTPUT/INOUT/NONE/OUTPUT_TRISTATE/FEEDTHRU, parsed to an
/// LeSignalDirection int by the Tcl proc). Returns kInvalidId on
/// failure.
long long create_terminal_cmd(long long abstract_id, const char *name, int direction);

int terminal_property_count(long long id);
const char *terminal_property_name(long long id, int index);
const char *terminal_property_value(long long id, int index);

int set_terminal_name(long long id, const char *name);

/// @brief Positional form behind `set_terminal_direction $id DIRECTION`
/// (see le_tcl_procs.tcl - DIRECTION is a name, same mapping
/// create_terminal_cmd's own direction argument uses, not a raw int).
int set_terminal_direction_cmd(long long id, int direction);
int delete_terminal(long long id);

/// @brief Search the Terminals belonging to the current view's Abstract
/// (see open_design/set_current_design_cmd - UPDATES.md item 17) for
/// `filter_expression` (see backend/src/database/filter.hpp for the
/// grammar, or pass "*" to match every Terminal in the current view
/// without parsing a filter expression at all) - returns a
/// space-separated string of packed ids (already a well-formed Tcl list:
/// packed ids are plain integers, never containing whitespace/braces, so
/// no escaping is needed - see this header's own "property tables and
/// search results" comment for why that's not always true elsewhere).
/// Empty string on no match, no current design selected, or a parse
/// error - check message_count() for a parse error same as any other
/// backend-originated message.
const char *get_terminals(const char *filter_expression);

// --- TerminalPort CRUD + search ---

long long create_terminal_port_cmd(long long terminal_id);
int terminal_port_property_count(long long id);
const char *terminal_port_property_name(long long id, int index);
const char *terminal_port_property_value(long long id, int index);
int delete_terminal_port(long long id);

/// @brief Search the TerminalPorts whose Terminal belongs to the current
/// view's Abstract - see get_terminals' own comment for the full
/// contract (grammar/"*"/empty-string-on-no-match), identical here, just
/// scoped to TerminalPort.
const char *get_terminal_ports(const char *filter_expression);

/// @brief Space-separated string of packed ShapeIds owned by the
/// TerminalPort at `id` - same "already a well-formed Tcl list"
/// reasoning as get_terminals' own comment.
const char *terminal_port_shapes(long long id);

// --- Obstruction CRUD + search ---

long long create_obstruction_cmd(long long abstract_id);
int obstruction_property_count(long long id);
const char *obstruction_property_name(long long id, int index);
const char *obstruction_property_value(long long id, int index);
int delete_obstruction(long long id);

/// @brief Search the Obstructions belonging to the current view's
/// Abstract - see get_terminals' own comment for the full contract,
/// identical here, just scoped to Obstruction.
const char *get_obstructions(const char *filter_expression);
const char *obstruction_shapes(long long id);

// --- Abstract boundary ---

/// @brief Positional form behind `update_abstract_boundary -abstract ID
/// -points {x y x y ...}` (see le_tcl_procs.tcl) - `points_um` is the
/// coordinate-list typemap this phase exists to write (see le_api.i),
/// not a raw double*/count pair a Tcl caller would have to build by
/// hand.
int update_abstract_boundary_cmd(long long abstract_id, const double *points_um, int32_t point_coord_count);

// --- Shape CRUD (rects/polygons/paths - texts deliberately excluded,
// see TCL_EXPLORATION.md's round-7 finding: they're a Pipeline-computed
// render-time label, never LEF-authored data) ---

long long create_terminal_port_shape_cmd(long long port_id, const char *layer_name);
long long create_obstruction_shape_cmd(long long obstruction_id, const char *layer_name);
const char *shape_layer_name(long long id);
int set_shape_layer_name(long long id, const char *layer_name);
int delete_shape(long long id);

int shape_rect_count(long long id);
/// @brief The rect at `index`, as a 4-element "ll_x ll_y ur_x ur_y"
/// microns string (already a well-formed Tcl list of 4 numbers).
const char *shape_rect_at(long long id, int index);
int add_shape_rect_cmd(long long id, double ll_x_um, double ll_y_um, double ur_x_um, double ur_y_um);
int remove_shape_rect(long long id, int index);

int shape_polygon_count(long long id);
int shape_polygon_point_count(long long id, int polygon_index);
/// @brief The point at `point_index` in the polygon at `polygon_index`,
/// as a 2-element "x y" microns string.
const char *shape_polygon_point_at(long long id, int polygon_index, int point_index);
int add_shape_polygon_cmd(long long id, const double *points_um, int32_t point_coord_count);
int remove_shape_polygon(long long id, int polygon_index);

int shape_path_count(long long id);
double shape_path_width_um(long long id, int path_index);
int shape_path_point_count(long long id, int path_index);
const char *shape_path_point_at(long long id, int path_index, int point_index);
int add_shape_path_cmd(long long id, double width_um, const double *points_um, int32_t point_coord_count);
int remove_shape_path(long long id, int path_index);
