# Phase 5 regression check (TCL_EXPLORATION.md): exercises the full
# Phase 4 CRUD/search surface through the ergonomic (item 15 -shaped)
# command layer - not just that SWIG can wrap it (smoke_test.tcl already
# covers that for the Phase 0 scalar slice), but that a real Tcl caller
# can create a Terminal/TerminalPort/Obstruction, attach rect/polygon/
# path geometry to a Shape via the coordinate-list typemap (a plain Tcl
# list, not a pre-flattened C array), search with a filter expression,
# update the Abstract boundary, and delete with cascade - end to end,
# against the real backend, not a mock.
#
# argv: <path to le_tcl shared module> <path to le_tcl_procs.tcl>
#       <path to testcell.lef fixture> <path to othercell.lef fixture>
#
# othercell.lef (MACRO OTHERCELL, PIN B) is read in as a second Design
# late in this script purely to prove UPDATES.md item 17's scoping: that
# open_design's current-view selection actually confines
# get_terminals/get_obstructions/get_terminal_ports to the Abstract in
# view, not every Abstract ever read into the session.

if {[llength $argv] != 4} {
    puts stderr "usage: crud_test.tcl <le_tcl.so> <le_tcl_procs.tcl> <testcell.lef> <othercell.lef>"
    exit 2
}
lassign $argv module_path procs_path lef_path other_lef_path

proc check {what expected actual} {
    if {$expected ne $actual} {
        puts stderr "FAIL: $what - expected {$expected}, got {$actual}"
        exit 1
    }
    puts "ok: $what = {$actual}"
}

proc check_true {what condition} {
    if {!$condition} {
        puts stderr "FAIL: $what"
        exit 1
    }
    puts "ok: $what"
}

load $module_path le_tcl
source $procs_path

check "read_lef return code" 0 [read_lef $lef_path]
check "message_count after read_lef" 0 [message_count]

set abstract_id [design_abstract_id 0]
check_true "design_abstract_id is valid" [expr {$abstract_id != $kInvalidId}]

check "open_design returns a friendly design id" design:TESTCELL [open_design TESTCELL]

if {[catch {open_design DOES_NOT_EXIST} err]} {
    check "open_design unknown name error message" "open_design: no such design \"DOES_NOT_EXIST\"" $err
} else {
    puts stderr "FAIL: open_design on an unknown name did not raise a Tcl error"
    exit 1
}

# --- Library/Design/Abstract search (UPDATES.md item 19.1) ---

check "get_libraries finds the loaded library" library:testcell [get_libraries]
check "get_libraries with a matching name expression" library:testcell [get_libraries testcell]
check "get_libraries with a non-matching name expression" {} [get_libraries DOES_NOT_EXIST]

check "get_designs -of library:testcell" design:TESTCELL [get_designs -of library:testcell]
check "get_designs default scope uses the current view's Library" design:TESTCELL [get_designs]

check "get_abstracts -of design:TESTCELL" abstract:0 [get_abstracts -of design:TESTCELL]
check "get_abstracts default scope uses the current view" abstract:0 [get_abstracts]

if {[catch {get_abstracts -of library:testcell} err]} {
    check "get_abstracts wrong -of type error message" \
        "get_abstracts: -of only accepts design: tokens (got \"library:testcell\") - only design objects are valid parents for get_abstracts" \
        $err
} else {
    puts stderr "FAIL: get_abstracts -of library:... did not raise a Tcl error"
    exit 1
}

# --- Library/Design/Abstract properties (UPDATES.md item 19.2 - new
# types, exercising get_properties beyond the Terminal-only examples in
# the item's own text) ---

check "get_properties on a library token" testcell \
    [dict get [get_properties [get_libraries]] name]
check "get_properties on a design token" TESTCELL \
    [dict get [get_properties [get_designs]] name]
check "get_properties on an abstract token" 0 \
    [dict get [get_properties [get_abstracts]] is_fixed_mask]

# --- Terminal ---

set in0 [create_terminal -abstract $abstract_id -name IN0 -direction INPUT]
check_true "create_terminal returned a valid friendly id" [expr {$in0 ne {}}]
check "create_terminal friendly id is name-based" terminal:IN0 $in0

set out0 [create_terminal -abstract $abstract_id -name OUT0 -direction OUTPUT]
check_true "second create_terminal returned a valid friendly id" [expr {$out0 ne {}}]

# --- Terminal-name uniqueness enforcement (UPDATES.md's friendly-id item) ---

set messages_before_duplicate [message_count]
check "create_terminal with a colliding name returns an empty id" {} \
    [create_terminal -abstract $abstract_id -name IN0 -direction INPUT]
check_true "create_terminal name collision pushed an error message" \
    [expr {[message_count] > $messages_before_duplicate}]

set messages_before_rename_collision [message_count]
check "set_terminal_name to a colliding name fails" 1 [set_terminal_name $out0 IN0]
check_true "set_terminal_name name collision pushed an error message" \
    [expr {[message_count] > $messages_before_rename_collision}]
check "set_terminal_name failure left OUT0 untouched" OUT0 [dict get [get_properties $out0] name]

set props [get_properties $in0]
check "get_properties name" IN0 [dict get $props name]
check "get_properties direction" INPUT [dict get $props direction]

# --- get_properties/report_properties shape (UPDATES.md item 19.2) ---
# Verifies all four of the item's own worked examples: single-token/no
# names -> dict (above); single-token/one-name -> scalar; single-token/
# many-names -> flat list; many-tokens/many-names -> list of flat lists.

check "get_properties single token, one name -> scalar" IN0 [get_properties $in0 .name]
check "get_properties single token, many names -> flat list" {IN0 INPUT} [get_properties $in0 {.name .direction}]

set multi_props [get_properties [list $in0 $out0] {.name .direction}]
check "get_properties many tokens, many names -> list of flat lists" \
    [list [list IN0 INPUT] [list OUT0 OUTPUT]] $multi_props

set multi_dicts [get_properties [list $in0 $out0]]
check "get_properties many tokens, no names -> list of dicts count" 2 [llength $multi_dicts]
check "get_properties many tokens, no names -> each entry is a dict" IN0 [dict get [lindex $multi_dicts 0] name]

if {[catch {get_properties {} .name}]} {
    puts "note: get_properties on an empty token list raised an error (acceptable, not exercised further)"
} else {
    check "get_properties on an empty token list returns empty" {} [get_properties {} .name]
}

if {[catch {get_properties bogus_token:1} err]} {
    # Prefix list is generated (property_accessors_for_token, see
    # generated/le_tcl_procs_generated.tcl) and covers every TCL-readable
    # class, not just the 7 with hand-written CRUD.
    check "get_properties unrecognized token error message" \
        "get_properties: unrecognized token \"bogus_token:1\" - expected a friendly id (technology:/layer:/site:/non_default_rule:/via:/via_rule:/shape:/library:/design:/abstract:/terminal:/terminal_port:/obstruction:/schematic:/instance:)" \
        $err
} else {
    puts stderr "FAIL: get_properties on an unrecognized token did not raise a Tcl error"
    exit 1
}

if {[catch {get_properties $in0 bogus_property}]} {
    puts "ok: get_properties with a missing leading '.' raised an error"
} else {
    puts stderr "FAIL: get_properties with a missing leading '.' did not raise a Tcl error"
    exit 1
}

if {[catch {get_properties $in0 .bogus_property}]} {
    puts "ok: get_properties with an unknown property name raised an error"
} else {
    puts stderr "FAIL: get_properties with an unknown property name did not raise a Tcl error"
    exit 1
}

# report_properties doesn't return anything meaningful to assert against
# (it prints) - just confirm it runs without error, same "prints, doesn't
# assert exact stdout" precedent as show_gui below.
report_properties [list $in0 $out0]

# Renaming changes what the friendly id refers to (it *is* the name) -
# $in0 ("terminal:IN0") goes stale the instant this succeeds; re-derive
# it from the new name rather than assuming the old string still
# resolves (see le_tcl_shim.hpp's own comment on set_terminal_name).
check "set_terminal_name return code" 0 [set_terminal_name $in0 IN0_RENAMED]
set in0 [get_terminals IN0_RENAMED]
check "renamed terminal is findable via its new friendly id" terminal:IN0_RENAMED $in0
check "renamed get_properties name" IN0_RENAMED [dict get [get_properties $in0] name]

check "set_terminal_name restore return code" 0 [set_terminal_name $in0 IN0]
set in0 [get_terminals IN0]
check "restored terminal is findable via its restored friendly id" terminal:IN0 $in0

check "set_terminal_direction return code" 0 [set_terminal_direction $in0 INOUT]
check "changed get_properties direction" INOUT [dict get [get_properties $in0] direction]
check "set_terminal_direction restore return code" 0 [set_terminal_direction $in0 INPUT]

set matches [get_terminals IN*]
check "get_terminals name-expression glob match" $in0 $matches

set both [get_terminals -filter {.direction == INPUT || .direction == OUTPUT}]
check "get_terminals -filter or-expression match count" 3 [llength $both]

check "get_terminals name-expression no match" {} [get_terminals DOES_NOT_EXIST]

# A bare positional name-expression is never a parse error (it's just a
# glob pattern, matched literally if it contains no wildcard) - the
# parse-error path only exists behind -filter now.
set messages_before_parse_error [message_count]
check "get_terminals -filter parse-error result" {} [get_terminals -filter {not a filter expression}]
check_true "get_terminals -filter parse error was logged" [expr {[message_count] > $messages_before_parse_error}]

set messages_before_bad_field [message_count]
check "get_terminals -filter with an unknown field returns empty" {} [get_terminals -filter {.bogus_field == 1}]
check_true "get_terminals -filter unknown-field error was logged" [expr {[message_count] > $messages_before_bad_field}]

# --- TerminalPort + Shape (rect/polygon/path via the coordinate typemap) ---

set port [create_terminal_port -terminal $in0]
check_true "create_terminal_port returned a valid friendly id" [expr {$port ne {}}]

# --- get_properties chained/dot-notation paths (dot-notation follow-up to
# item 19.2) ---

check "get_properties chained path through a hop" IN0 [get_properties $port .terminal.name]
check "get_properties chained path, many names -> flat list" {IN0 INPUT} \
    [get_properties $port {.terminal.name .terminal.direction}]
check "get_properties chained list-hop path before any shape exists -> empty" {} \
    [get_properties $port .shapes.layer_name]

if {[catch {get_properties $port .bogus_hop.name}]} {
    puts "ok: get_properties with an unknown hop in a chained path raised an error"
} else {
    puts stderr "FAIL: get_properties with an unknown hop in a chained path did not raise a Tcl error"
    exit 1
}

set shape [create_terminal_port_shape -port $port -layer M1]
check_true "create_terminal_port_shape returned a valid friendly id" [expr {$shape ne {}}]

check "get_properties chained list-hop path after a shape exists" M1 \
    [get_properties $port .shapes.layer_name]

check "shape_layer_name" M1 [shape_layer_name $shape]
check "set_shape_layer_name return code" 0 [set_shape_layer_name $shape M1]

check "add_shape_rect return code" 0 [add_shape_rect -shape $shape -rect {2 2 8 8}]
check "add_shape_polygon return code" 0 [add_shape_polygon -shape $shape -points {0 0 5 0 5 5 0 5}]
check "add_shape_path return code" 0 [add_shape_path -shape $shape -width 0.5 -points {0 0 10 10 20 0}]

set rects [shape_rects $shape]
check "shape_rects count" 1 [llength $rects]
check "shape_rects contents" {2 2 8 8} [lindex $rects 0]

set polygons [shape_polygons $shape]
check "shape_polygons count" 1 [llength $polygons]
check "shape_polygons point count" 4 [llength [lindex $polygons 0]]
check "shape_polygons first point" {0 0} [lindex [lindex $polygons 0] 0]

set paths [shape_paths $shape]
check "shape_paths count" 1 [llength $paths]
check "shape_paths width" 0.5 [dict get [lindex $paths 0] width_um]
check "shape_paths point count" 3 [llength [dict get [lindex $paths 0] points]]

check "terminal_port_shapes lists the created shape" $shape [terminal_port_shapes $port]

set port_matches [get_terminal_ports -filter {.terminal.name =~ IN* && .shapes.layer_name == M1}]
check "get_terminal_ports -filter relational + list-hop match" $port $port_matches

check "get_shapes -of \$port finds only the created shape" $shape [get_shapes -of $port]
# The default (no -of) scope also picks up the fixture's own LEF-authored
# PIN A, which has its own Port+Shape (shape:0, created while reading
# testcell.lef) - not just the one this test just created (shape:1).
check "get_shapes default scope finds every Shape in the current view" {shape:0 shape:1} [get_shapes]

check "get_properties on a shape token" M1 [dict get [get_properties $shape] layer_name]

# --- get_properties on non-pooled object-list fields (rects/polygons/
# paths/...) returns the full list with all coordinates, not a bare
# "<field>_count" - clean, micron-converted, brace-nested coordinates
# ("{{ll_x ll_y} {ur_x ur_y}}" per rect), generated generically for every
# class (codegen/codegen/schema.py's `dbu` field type + Field.wrap_with_*
# methods - see backend/CLAUDE.md's Database codegen section), not a
# Shape-only hand-written override anymore. A Path's own "polygon" field
# is itself a reference to an embedded Polygon, so it gets its own
# wrapping brace group like any other embedded-klass reference (Polygon's
# points list nested one level inside that, width a sibling alongside
# it) - not flattened flush with width. ---

set shape_props [get_properties $shape]
check "get_properties rects is a clean micron-converted list, not a count" \
    {{{2 2} {8 8}}} \
    [dict get $shape_props rects]
check "get_properties polygons is a clean micron-converted point list" \
    {{{0 0} {5 0} {5 5} {0 5}}} \
    [dict get $shape_props polygons]
check "get_properties paths is a clean micron-converted point list plus width" \
    {{{{0 0} {10 10} {20 0}} 0.500}} \
    [dict get $shape_props paths]
check_true "rects_count is no longer a property (removed, confusing once the list itself is available)" \
    [expr {![dict exists $shape_props rects_count]}]
check_true "polygons_count is no longer a property" [expr {![dict exists $shape_props polygons_count]}]
check_true "paths_count is no longer a property" [expr {![dict exists $shape_props paths_count]}]
# A list of a plain scalar (not an embedded Klass) is untouched - still a
# count, since there's no per-element to_property_string() to expand into.
check "rect_masks_count is unaffected (list of a plain int, not an object)" \
    0 [dict get $shape_props rect_masks_count]

# Two rects stay unambiguously grouped (not flattened into one 4-point
# list) - add a second rect, then remove it again so the rest of this
# test still sees exactly the one rect it expects below.
check "add second shape rect return code" 0 [add_shape_rect -shape $shape -rect {10 10 20 20}]
set two_rects [dict get [get_properties $shape] rects]
check "get_properties rects keeps each rect as its own list element" 2 [llength $two_rects]
check "get_properties rects first element" {{2 2} {8 8}} [lindex $two_rects 0]
check "get_properties rects second element" {{10 10} {20 20}} [lindex $two_rects 1]
check "remove second shape rect return code" 0 [remove_shape_rect $shape 1]

# --- get_properties single-segment dot-path lookup on rects/polygons/
# paths (get_properties $shape .rects) - a regression check for a real
# dangling-pointer bug: le_X_property_path used to build its LeProperty
# result from a std::optional<PropertyValue>/vector local to the
# function, whose .c_str() pointers went stale the instant the function
# returned. A short value ("IN0") happened to keep "working" by sheer
# luck (still-intact bytes in the just-freed stack slot, small enough for
# std::string's small-string optimization), which is exactly why this
# went unnoticed until a long, heap-allocated value (a formatted rects/
# polygons/paths coordinate list) came back corrupted instead. Fixed by
# routing every le_X_property_path result through a handle-owned cache
# slot (same "valid until the next call" convention every other
# LeProperty-returning accessor here already relies on). ---

check "get_properties single-segment .rects (not just the bare-token dict form)" \
    {{{2 2} {8 8}}} [get_properties $shape .rects]
check "get_properties single-segment .polygons" \
    {{{0 0} {5 0} {5 5} {0 5}}} [get_properties $shape .polygons]
check "get_properties single-segment .paths" \
    {{{{0 0} {10 10} {20 0}} 0.500}} [get_properties $shape .paths]
check "get_properties many single-segment object-list names together" \
    [list {{{2 2} {8 8}}} {{{0 0} {5 0} {5 5} {0 5}}} {{{{0 0} {10 10} {20 0}} 0.500}} M1] \
    [get_properties $shape {.rects .polygons .paths .layer_name}]

check "remove_shape_rect return code" 0 [remove_shape_rect $shape 0]
check "shape_rects after remove" {} [shape_rects $shape]

# --- Obstruction ---

set obstruction [create_obstruction -abstract $abstract_id]
check_true "create_obstruction returned a valid friendly id" [expr {$obstruction ne {}}]

set obstruction_shape [create_obstruction_shape -obstruction $obstruction -layer M1]
check "add rect to obstruction shape" 0 [add_shape_rect -shape $obstruction_shape -rect {0 0 1 1}]
check "obstruction_shapes lists the created shape" $obstruction_shape [obstruction_shapes $obstruction]

set obstruction_matches [get_obstructions -filter {.shapes.layer_name == M1}]
check "get_obstructions -filter list-hop match" $obstruction $obstruction_matches

check "delete_obstruction return code" 0 [delete_obstruction $obstruction]
check "get_obstructions after delete" {} [get_obstructions -filter {.shapes.layer_name == M1}]

# --- Abstract boundary ---

check "update_abstract_boundary return code" 0 [update_abstract_boundary -abstract $abstract_id -points {0 0 10 0 10 10 0 10}]

# --- Cascade delete: deleting the Terminal must take its TerminalPort
# (and that port's Shape) with it, since neither is reachable any other
# way (see le_delete_terminal's own doc comment in api.hpp). ---

check "delete_terminal return code" 0 [delete_terminal $in0]
check "get_terminal_ports after cascade delete" {} [get_terminal_ports -filter {.terminal.name == IN0}]

check "delete_terminal (out0) return code" 0 [delete_terminal $out0]

# The fixture's own LEF-authored PIN A is untouched by any of the above -
# it's the only Terminal left once both terminals this test created are
# gone.
check "get_terminals after deleting both created terminals" A [dict get [get_properties [get_terminals]] name]

# --- Current-view scoping (UPDATES.md item 17) ---
#
# Read a second, independent Design (othercell.lef's OTHERCELL/PIN B)
# into the same session and confirm switching the current view via
# open_design actually confines get_terminals to the Abstract in view -
# neither Design's terminals leak into the other's results.
# get_obstructions/get_terminal_ports share get_terminals' own
# current-abstract scoping mechanism (api.cpp), so this one check
# exercises the same code path all three rely on.

check "read_lef (othercell.lef) return code" 0 [read_lef $other_lef_path]

check "open_design OTHERCELL returns a friendly design id" design:OTHERCELL [open_design OTHERCELL]

set other_terminal_names {}
foreach id [get_terminals] {
    lappend other_terminal_names [dict get [get_properties $id] name]
}
check "get_terminals default scope in OTHERCELL's view only sees OTHERCELL's own terminal" B $other_terminal_names

check "open_design TESTCELL returns a friendly design id" design:TESTCELL [open_design TESTCELL]

set testcell_terminal_names {}
foreach id [get_terminals] {
    lappend testcell_terminal_names [dict get [get_properties $id] name]
}
check "get_terminals default scope back in TESTCELL's view only sees TESTCELL's own terminal, not OTHERCELL's" A $testcell_terminal_names

puts "le_tcl CRUD test passed"
