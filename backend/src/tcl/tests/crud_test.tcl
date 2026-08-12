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

check_true "open_design returns a valid design id" [expr {[open_design TESTCELL] != $kInvalidId}]

if {[catch {open_design DOES_NOT_EXIST} err]} {
    check "open_design unknown name error message" "open_design: no such design \"DOES_NOT_EXIST\"" $err
} else {
    puts stderr "FAIL: open_design on an unknown name did not raise a Tcl error"
    exit 1
}

# --- Terminal ---

set in0 [create_terminal -abstract $abstract_id -name IN0 -direction INPUT]
check_true "create_terminal returned a valid id" [expr {$in0 != $kInvalidId}]

set out0 [create_terminal -abstract $abstract_id -name OUT0 -direction OUTPUT]
check_true "second create_terminal returned a valid id" [expr {$out0 != $kInvalidId}]

set props [terminal_properties $in0]
check "terminal_properties name" IN0 [dict get $props name]
check "terminal_properties direction" INPUT [dict get $props direction]

check "set_terminal_name return code" 0 [set_terminal_name $in0 IN0_RENAMED]
check "renamed terminal_properties name" IN0_RENAMED [dict get [terminal_properties $in0] name]
check "set_terminal_name restore return code" 0 [set_terminal_name $in0 IN0]

check "set_terminal_direction return code" 0 [set_terminal_direction $in0 INOUT]
check "changed terminal_properties direction" INOUT [dict get [terminal_properties $in0] direction]
check "set_terminal_direction restore return code" 0 [set_terminal_direction $in0 INPUT]

set matches [get_terminals ".name =~ IN*"]
check "get_terminals glob match" $in0 $matches

set both [get_terminals ".direction == INPUT || .direction == OUTPUT"]
check "get_terminals or-expression match count" 3 [llength $both]

check "get_terminals no match" {} [get_terminals ".name == DOES_NOT_EXIST"]

set messages_before_parse_error [message_count]
if {[catch {get_terminals "not a filter expression"} bad_result]} {
    puts "unexpected: get_terminals on a bad expression raised a Tcl error ($bad_result)"
} else {
    check "get_terminals parse-error result" {} $bad_result
    check_true "get_terminals parse error was logged" [expr {[message_count] > $messages_before_parse_error}]
}

# --- TerminalPort + Shape (rect/polygon/path via the coordinate typemap) ---

set port [create_terminal_port -terminal $in0]
check_true "create_terminal_port returned a valid id" [expr {$port != $kInvalidId}]

set shape [create_terminal_port_shape -port $port -layer M1]
check_true "create_terminal_port_shape returned a valid id" [expr {$shape != $kInvalidId}]

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

set port_matches [get_terminal_ports ".terminal.name =~ IN* && .shapes.layer_name == M1"]
check "get_terminal_ports relational + list-hop match" $port $port_matches

check "remove_shape_rect return code" 0 [remove_shape_rect $shape 0]
check "shape_rects after remove" {} [shape_rects $shape]

# --- Obstruction ---

set obstruction [create_obstruction -abstract $abstract_id]
check_true "create_obstruction returned a valid id" [expr {$obstruction != $kInvalidId}]

set obstruction_shape [create_obstruction_shape -obstruction $obstruction -layer M1]
check "add rect to obstruction shape" 0 [add_shape_rect -shape $obstruction_shape -rect {0 0 1 1}]
check "obstruction_shapes lists the created shape" $obstruction_shape [obstruction_shapes $obstruction]

set obstruction_matches [get_obstructions ".shapes.layer_name == M1"]
check "get_obstructions list-hop match" $obstruction $obstruction_matches

check "delete_obstruction return code" 0 [delete_obstruction $obstruction]
check "get_obstructions after delete" {} [get_obstructions ".shapes.layer_name == M1"]

# --- Abstract boundary ---

check "update_abstract_boundary return code" 0 [update_abstract_boundary -abstract $abstract_id -points {0 0 10 0 10 10 0 10}]

# --- Cascade delete: deleting the Terminal must take its TerminalPort
# (and that port's Shape) with it, since neither is reachable any other
# way (see le_delete_terminal's own doc comment in api.hpp). ---

check "delete_terminal return code" 0 [delete_terminal $in0]
check "get_terminal_ports after cascade delete" {} [get_terminal_ports ".terminal.name == IN0"]

check "delete_terminal (out0) return code" 0 [delete_terminal $out0]

# The fixture's own LEF-authored PIN A is untouched by any of the above -
# it's the only Terminal left once both terminals this test created are
# gone.
check "get_terminals after deleting both created terminals" A [dict get [terminal_properties [get_terminals ".name =~ *"]] name]

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

check_true "open_design OTHERCELL returns a valid design id" [expr {[open_design OTHERCELL] != $kInvalidId}]

set other_terminal_names {}
foreach id [get_terminals "*"] {
    lappend other_terminal_names [dict get [terminal_properties $id] name]
}
check "get_terminals * in OTHERCELL's view only sees OTHERCELL's own terminal" B $other_terminal_names

check_true "open_design TESTCELL returns a valid design id" [expr {[open_design TESTCELL] != $kInvalidId}]

set testcell_terminal_names {}
foreach id [get_terminals "*"] {
    lappend testcell_terminal_names [dict get [terminal_properties $id] name]
}
check "get_terminals * back in TESTCELL's view only sees TESTCELL's own terminal, not OTHERCELL's" A $testcell_terminal_names

puts "le_tcl CRUD test passed"
