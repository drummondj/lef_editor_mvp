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
// --- IDs (Phase 5, revised for friendly ids) ---
// AbstractId/DesignId (design_abstract_id, design_by_name,
// set_current_design_cmd, update_abstract_boundary_cmd's abstract_id
// argument) are still a plain {uint32_t index, generation} struct in
// api.hpp, packed into one int64_t (generation in the high 32 bits, index
// in the low 32 bits, via pack<IdT>/unpack<IdT> in le_tcl_shim.cpp) -
// int64_t is a fundamental type SWIG's stdint.i already marshals to/from
// a plain Tcl integer with zero custom typemap code. kInvalidId
// (0xFFFFFFFF) marks an invalid id for these two types only, since a
// packed valid id (index != UINT32_MAX) can never equal it.
//
// TerminalId/TerminalPortId/ObstructionId/ShapeId cross this shim as
// type-prefixed strings instead - not user-friendly to hand a script a
// raw packed integer as its only handle on an object. `"terminal:<name>"`
// (Terminal's own name field - see le_create_terminal/le_set_terminal_name's
// new uniqueness enforcement in api.hpp, which makes a name a safe,
// unambiguous key within one Abstract) and `"obstruction:<n>"`/
// `"terminal_port:<n>"`/`"shape:<n>"` (Obstruction/TerminalPort/Shape have no name
// field - `<n>` is the same packed integer as before, just type-prefixed
// for self-description, resolved with resolve_numeric_friendly_id in
// le_tcl_shim.cpp) are built/parsed entirely in le_tcl_shim.cpp -
// api.hpp's own return types for these are unchanged (still typed
// LeTerminalId/etc structs), matching this shim's usual "ergonomics stay
// here, not in the shared C API" split. Empty string `""` is the uniform
// "not found / invalid" signal for all four - a malformed string or a
// wrong-type prefix (e.g. passing an `"obstruction:..."` id where a
// `"terminal:..."` one is expected) resolves to the same invalid sentinel
// api.hpp's own not-found paths already produce, indistinguishable from
// "no such object" - no separate error path needed for it.
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
//
// get_terminals is the one search-result accessor built as count+by-index
// (get_terminals_cmd/get_terminals_at) rather than one space-joined
// string, unlike get_obstructions/get_terminal_ports/terminal_port_shapes/
// obstruction_shapes: a Terminal's friendly id embeds its own
// (LEF-authored, not this project's to constrain) name, so nothing rules
// out whitespace/braces inside a joined "terminal:..." token - the other
// four's tokens are always purely numeric (`obstruction:N`/`terminal_port:N`/
// `shape:N`), so they're provably safe to keep space-joining.

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

/// @brief Sentinel for "no such id" for AbstractId/DesignId - see this
/// header's own "IDs" comment. Every api.hpp failure path for these two
/// types returns an id struct with index == UINT32_MAX and generation ==
/// 0 (never left uninitialized - see TCL_EXPLORATION.md's "zero-init
/// bug" note), which pack() (in le_tcl_shim.cpp) always turns into
/// exactly this value - not -1 (0xFFFFFFFFFFFFFFFF), which would require
/// generation == UINT32_MAX too. Terminal/TerminalPort/Obstruction/Shape
/// ids use an empty string ("") as their own invalid sentinel instead -
/// see the "IDs" comment above.
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
/// LeSignalDirection int by the Tcl proc). Returns a friendly
/// `"terminal:NAME"` id (see this header's own "IDs" comment), or "" on
/// failure - including a NAME collision with an existing Terminal on the
/// same Abstract (see le_create_terminal's own uniqueness comment in
/// api.hpp; check message_count() for the pushed error).
const char *create_terminal_cmd(long long abstract_id, const char *name, int direction);

int terminal_property_count(const char *id);
const char *terminal_property_name(const char *id, int index);
const char *terminal_property_value(const char *id, int index);

/// @brief Rename the Terminal `id` refers to - note that `id` itself
/// (`"terminal:OLDNAME"`) becomes stale/dangling the moment this
/// succeeds, since the friendly id *is* the name; a script needs to
/// re-derive `"terminal:NEWNAME"` (it already has NEWNAME literally) to
/// keep addressing the same Terminal afterward. Returns nonzero on a
/// NAME collision with a different Terminal on the same Abstract - see
/// le_set_terminal_name's own comment in api.hpp.
int set_terminal_name(const char *id, const char *name);

/// @brief Positional form behind `set_terminal_direction $id DIRECTION`
/// (see le_tcl_procs.tcl - DIRECTION is a name, same mapping
/// create_terminal_cmd's own direction argument uses, not a raw int).
int set_terminal_direction_cmd(const char *id, int direction);
int delete_terminal(const char *id);

/// @brief Search the Terminals belonging to the current view's Abstract
/// (see open_design/set_current_design_cmd - UPDATES.md item 17) for
/// `filter_expression` (see backend/src/database/filter.hpp for the
/// grammar, or pass "*" to match every Terminal in the current view
/// without parsing a filter expression at all). Returns the match count
/// (0 on no match, no current design selected, or a null
/// filter_expression), or -1 on a parse error (check message_count()).
/// Read results back via get_terminals_at, same "valid until the next
/// get_terminals_cmd call" convention as this shim's other cached-result
/// accessors - see this header's own "IDs"/"property tables and search
/// results" comments for why this is count+by-index rather than one
/// joined string, unlike get_obstructions/get_terminal_ports.
int get_terminals_cmd(const char *filter_expression);

/// @brief The friendly `"terminal:NAME"` id at `index`
/// (0..get_terminals_cmd's last return value - 1) from the most recent
/// get_terminals_cmd call. Returns "" if index is out of range.
const char *get_terminals_at(int index);

// --- TerminalPort CRUD + search ---

/// @brief Positional form behind `create_terminal_port -terminal ID`
/// (see le_tcl_procs.tcl). `terminal_id` is a friendly `"terminal:NAME"`
/// id. Returns a friendly `"terminal_port:N"` id, or "" on failure.
const char *create_terminal_port_cmd(const char *terminal_id);
int terminal_port_property_count(const char *id);
const char *terminal_port_property_name(const char *id, int index);
const char *terminal_port_property_value(const char *id, int index);
int delete_terminal_port(const char *id);

/// @brief Search the TerminalPorts whose Terminal belongs to the current
/// view's Abstract - see get_terminals_cmd's own comment for the filter
/// grammar/"*"/scoping contract. Unlike get_terminals_cmd, this stays a
/// single space-separated-string return: every token is a purely numeric
/// `"terminal_port:N"` id, never LEF-authored text, so joining is provably safe
/// (already a well-formed Tcl list - see this header's own "property
/// tables and search results" comment). Empty string on no match, no
/// current design selected, or a parse error - check message_count().
const char *get_terminal_ports(const char *filter_expression);

/// @brief Space-separated string of friendly `"shape:N"` ids owned by
/// the TerminalPort at `id` - same "already a well-formed Tcl list"
/// reasoning as get_terminal_ports' own comment.
const char *terminal_port_shapes(const char *id);

// --- Obstruction CRUD + search ---

/// @brief Positional form behind `create_obstruction -abstract ID` (see
/// le_tcl_procs.tcl). Returns a friendly `"obstruction:N"` id, or "" on
/// failure.
const char *create_obstruction_cmd(long long abstract_id);
int obstruction_property_count(const char *id);
const char *obstruction_property_name(const char *id, int index);
const char *obstruction_property_value(const char *id, int index);
int delete_obstruction(const char *id);

/// @brief Search the Obstructions belonging to the current view's
/// Abstract - see get_terminal_ports' own comment for the full contract
/// (grammar/"*"/space-joined-is-safe reasoning), identical here, just
/// scoped to Obstruction; tokens are purely numeric `"obstruction:N"`.
const char *get_obstructions(const char *filter_expression);
const char *obstruction_shapes(const char *id);

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

/// @brief `terminal_port_id`/`obstruction_id` are friendly ids
/// (`"terminal_port:N"`/`"obstruction:N"`); both return a friendly `"shape:N"` id,
/// or "" on failure.
const char *create_terminal_port_shape_cmd(const char *terminal_port_id, const char *layer_name);
const char *create_obstruction_shape_cmd(const char *obstruction_id, const char *layer_name);
const char *shape_layer_name(const char *id);
int set_shape_layer_name(const char *id, const char *layer_name);
int delete_shape(const char *id);

int shape_rect_count(const char *id);
/// @brief The rect at `index`, as a 4-element "ll_x ll_y ur_x ur_y"
/// microns string (already a well-formed Tcl list of 4 numbers).
const char *shape_rect_at(const char *id, int index);
int add_shape_rect_cmd(const char *id, double ll_x_um, double ll_y_um, double ur_x_um, double ur_y_um);
int remove_shape_rect(const char *id, int index);

int shape_polygon_count(const char *id);
int shape_polygon_point_count(const char *id, int polygon_index);
/// @brief The point at `point_index` in the polygon at `polygon_index`,
/// as a 2-element "x y" microns string.
const char *shape_polygon_point_at(const char *id, int polygon_index, int point_index);
int add_shape_polygon_cmd(const char *id, const double *points_um, int32_t point_coord_count);
int remove_shape_polygon(const char *id, int polygon_index);

int shape_path_count(const char *id);
double shape_path_width_um(const char *id, int path_index);
int shape_path_point_count(const char *id, int path_index);
const char *shape_path_point_at(const char *id, int path_index, int point_index);
int add_shape_path_cmd(const char *id, double width_um, const double *points_um, int32_t point_coord_count);
int remove_shape_path(const char *id, int path_index);
