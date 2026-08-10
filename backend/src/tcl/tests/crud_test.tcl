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
#       <path to testcell.lef fixture>

if {[llength $argv] != 3} {
    puts stderr "usage: crud_test.tcl <le_tcl.so> <le_tcl_procs.tcl> <testcell.lef>"
    exit 2
}
lassign $argv module_path procs_path lef_path

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

set matches [search_terminal ".name =~ IN*"]
check "search_terminal glob match" $in0 $matches

set both [search_terminal ".direction == INPUT || .direction == OUTPUT"]
check "search_terminal or-expression match count" 3 [llength $both]

check "search_terminal no match" {} [search_terminal ".name == DOES_NOT_EXIST"]

set messages_before_parse_error [message_count]
if {[catch {search_terminal "not a filter expression"} bad_result]} {
    puts "unexpected: search_terminal on a bad expression raised a Tcl error ($bad_result)"
} else {
    check "search_terminal parse-error result" {} $bad_result
    check_true "search_terminal parse error was logged" [expr {[message_count] > $messages_before_parse_error}]
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

set port_matches [search_terminal_port ".terminal.name =~ IN* && .shapes.layer_name == M1"]
check "search_terminal_port relational + list-hop match" $port $port_matches

check "remove_shape_rect return code" 0 [remove_shape_rect $shape 0]
check "shape_rects after remove" {} [shape_rects $shape]

# --- Obstruction ---

set obstruction [create_obstruction -abstract $abstract_id]
check_true "create_obstruction returned a valid id" [expr {$obstruction != $kInvalidId}]

set obstruction_shape [create_obstruction_shape -obstruction $obstruction -layer M1]
check "add rect to obstruction shape" 0 [add_shape_rect -shape $obstruction_shape -rect {0 0 1 1}]
check "obstruction_shapes lists the created shape" $obstruction_shape [obstruction_shapes $obstruction]

set obstruction_matches [search_obstruction ".shapes.layer_name == M1"]
check "search_obstruction list-hop match" $obstruction $obstruction_matches

check "delete_obstruction return code" 0 [delete_obstruction $obstruction]
check "search_obstruction after delete" {} [search_obstruction ".shapes.layer_name == M1"]

# --- Abstract boundary ---

check "update_abstract_boundary return code" 0 [update_abstract_boundary -abstract $abstract_id -points {0 0 10 0 10 10 0 10}]

# --- Cascade delete: deleting the Terminal must take its TerminalPort
# (and that port's Shape) with it, since neither is reachable any other
# way (see le_delete_terminal's own doc comment in api.hpp). ---

check "delete_terminal return code" 0 [delete_terminal $in0]
check "search_terminal_port after cascade delete" {} [search_terminal_port ".terminal.name == IN0"]

check "delete_terminal (out0) return code" 0 [delete_terminal $out0]

# The fixture's own LEF-authored PIN A is untouched by any of the above -
# it's the only Terminal left once both terminals this test created are
# gone.
check "search_terminal after deleting both created terminals" A [dict get [terminal_properties [search_terminal ".name =~ *"]] name]

puts "le_tcl CRUD test passed"
