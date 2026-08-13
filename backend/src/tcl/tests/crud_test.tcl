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
check "set_terminal_name failure left OUT0 untouched" OUT0 [dict get [terminal_properties $out0] name]

set props [terminal_properties $in0]
check "terminal_properties name" IN0 [dict get $props name]
check "terminal_properties direction" INPUT [dict get $props direction]

# Renaming changes what the friendly id refers to (it *is* the name) -
# $in0 ("terminal:IN0") goes stale the instant this succeeds; re-derive
# it from the new name rather than assuming the old string still
# resolves (see le_tcl_shim.hpp's own comment on set_terminal_name).
check "set_terminal_name return code" 0 [set_terminal_name $in0 IN0_RENAMED]
set in0 [get_terminals IN0_RENAMED]
check "renamed terminal is findable via its new friendly id" terminal:IN0_RENAMED $in0
check "renamed terminal_properties name" IN0_RENAMED [dict get [terminal_properties $in0] name]

check "set_terminal_name restore return code" 0 [set_terminal_name $in0 IN0]
set in0 [get_terminals IN0]
check "restored terminal is findable via its restored friendly id" terminal:IN0 $in0

check "set_terminal_direction return code" 0 [set_terminal_direction $in0 INOUT]
check "changed terminal_properties direction" INOUT [dict get [terminal_properties $in0] direction]
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

set shape [create_terminal_port_shape -port $port -layer M1]
check_true "create_terminal_port_shape returned a valid friendly id" [expr {$shape ne {}}]

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
check "get_terminals after deleting both created terminals" A [dict get [terminal_properties [get_terminals]] name]

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
    lappend other_terminal_names [dict get [terminal_properties $id] name]
}
check "get_terminals default scope in OTHERCELL's view only sees OTHERCELL's own terminal" B $other_terminal_names

check "open_design TESTCELL returns a friendly design id" design:TESTCELL [open_design TESTCELL]

set testcell_terminal_names {}
foreach id [get_terminals] {
    lappend testcell_terminal_names [dict get [terminal_properties $id] name]
}
check "get_terminals default scope back in TESTCELL's view only sees TESTCELL's own terminal, not OTHERCELL's" A $testcell_terminal_names

puts "le_tcl CRUD test passed"
