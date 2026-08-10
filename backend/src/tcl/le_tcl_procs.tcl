# Thin -flag parsing layer over the SWIG-wrapped *_cmd shim functions
# (le_tcl_shim.hpp/.cpp) - Tcl's own `-flag value` calling convention has
# to be parsed here, since SWIG-wrapped C++ functions are always
# positional. See TCL_EXPLORATION.md's "Tcl ergonomics layer" section for
# why this split exists (C++ shim owns session state + command logic,
# Tcl owns flag parsing) rather than putting everything in one place.
# Sourced by anything that loads the le_tcl module and wants the
# ergonomic (item 15 -shaped) command surface rather than the raw *_cmd
# forms.
#
# Also owns every piece of Phase 5's CRUD/search surface that's better
# built in Tcl than in C++: property tables and search-result/shape-list
# aggregation (terminal_properties, shape_rects, ...) loop over the
# shim's plain count+by-index accessors and build a real Tcl dict/list
# with `dict set`/`lappend` - correct quoting by construction, unlike
# hand-rolled string-building in C++ (see le_tcl_shim.hpp's own
# "property tables and search results" comment for why that split was
# made). Coordinate lists themselves (`-points {x y x y ...}`) need no
# such treatment here - le_api.i's typemap already turns a plain Tcl list
# into the shim's (const double*, int32_t count) pair directly.

set kInvalidId 4294967295

# Mirrors LeSignalDirection's declaration order (api.hpp) - kept in sync
# by hand since this is Tcl, not generated code; extend if
# LeSignalDirection ever gains a member.
array set direction_codes {
    INPUT 0
    OUTPUT 1
    INOUT 2
    NONE 3
    OUTPUT_TRISTATE 4
    FEEDTHRU 5
}

proc direction_code {name} {
    global direction_codes
    if {![info exists direction_codes($name)]} {
        error "unknown direction $name - expected one of: [array names direction_codes]"
    }
    return $direction_codes($name)
}

proc set_viewport_size {args} {
    array set opts {-width {} -height {}}
    foreach {flag value} $args {
        if {![info exists opts($flag)]} {
            error "set_viewport_size: unknown flag $flag"
        }
        set opts($flag) $value
    }
    if {$opts(-width) eq {} || $opts(-height) eq {}} {
        error "set_viewport_size: -width and -height are required"
    }
    return [set_viewport_size_cmd $opts(-width) $opts(-height)]
}

# --- Terminal ---

proc create_terminal {args} {
    array set opts {-abstract {} -name {} -direction {}}
    foreach {flag value} $args {
        if {![info exists opts($flag)]} {
            error "create_terminal: unknown flag $flag"
        }
        set opts($flag) $value
    }
    foreach required {-abstract -name -direction} {
        if {$opts($required) eq {}} {
            error "create_terminal: $required is required"
        }
    }
    return [create_terminal_cmd $opts(-abstract) $opts(-name) [direction_code $opts(-direction)]]
}

proc set_terminal_direction {id direction} {
    return [set_terminal_direction_cmd $id [direction_code $direction]]
}

proc terminal_properties {id} {
    set result {}
    set n [terminal_property_count $id]
    for {set i 0} {$i < $n} {incr i} {
        dict set result [terminal_property_name $id $i] [terminal_property_value $id $i]
    }
    return $result
}

# --- TerminalPort ---

proc create_terminal_port {args} {
    array set opts {-terminal {}}
    foreach {flag value} $args {
        if {![info exists opts($flag)]} {
            error "create_terminal_port: unknown flag $flag"
        }
        set opts($flag) $value
    }
    if {$opts(-terminal) eq {}} {
        error "create_terminal_port: -terminal is required"
    }
    return [create_terminal_port_cmd $opts(-terminal)]
}

proc terminal_port_properties {id} {
    set result {}
    set n [terminal_port_property_count $id]
    for {set i 0} {$i < $n} {incr i} {
        dict set result [terminal_port_property_name $id $i] [terminal_port_property_value $id $i]
    }
    return $result
}

# --- Obstruction ---

proc create_obstruction {args} {
    array set opts {-abstract {}}
    foreach {flag value} $args {
        if {![info exists opts($flag)]} {
            error "create_obstruction: unknown flag $flag"
        }
        set opts($flag) $value
    }
    if {$opts(-abstract) eq {}} {
        error "create_obstruction: -abstract is required"
    }
    return [create_obstruction_cmd $opts(-abstract)]
}

proc obstruction_properties {id} {
    set result {}
    set n [obstruction_property_count $id]
    for {set i 0} {$i < $n} {incr i} {
        dict set result [obstruction_property_name $id $i] [obstruction_property_value $id $i]
    }
    return $result
}

# --- Abstract boundary ---

proc update_abstract_boundary {args} {
    array set opts {-abstract {} -points {}}
    foreach {flag value} $args {
        if {![info exists opts($flag)]} {
            error "update_abstract_boundary: unknown flag $flag"
        }
        set opts($flag) $value
    }
    foreach required {-abstract -points} {
        if {$opts($required) eq {}} {
            error "update_abstract_boundary: $required is required"
        }
    }
    return [update_abstract_boundary_cmd $opts(-abstract) $opts(-points)]
}

# --- Shape ---

proc create_terminal_port_shape {args} {
    array set opts {-port {} -layer {}}
    foreach {flag value} $args {
        if {![info exists opts($flag)]} {
            error "create_terminal_port_shape: unknown flag $flag"
        }
        set opts($flag) $value
    }
    foreach required {-port -layer} {
        if {$opts($required) eq {}} {
            error "create_terminal_port_shape: $required is required"
        }
    }
    return [create_terminal_port_shape_cmd $opts(-port) $opts(-layer)]
}

proc create_obstruction_shape {args} {
    array set opts {-obstruction {} -layer {}}
    foreach {flag value} $args {
        if {![info exists opts($flag)]} {
            error "create_obstruction_shape: unknown flag $flag"
        }
        set opts($flag) $value
    }
    foreach required {-obstruction -layer} {
        if {$opts($required) eq {}} {
            error "create_obstruction_shape: $required is required"
        }
    }
    return [create_obstruction_shape_cmd $opts(-obstruction) $opts(-layer)]
}

proc add_shape_rect {args} {
    array set opts {-shape {} -rect {}}
    foreach {flag value} $args {
        if {![info exists opts($flag)]} {
            error "add_shape_rect: unknown flag $flag"
        }
        set opts($flag) $value
    }
    foreach required {-shape -rect} {
        if {$opts($required) eq {}} {
            error "add_shape_rect: $required is required"
        }
    }
    if {[llength $opts(-rect)] != 4} {
        error "add_shape_rect: -rect must be {ll_x ll_y ur_x ur_y}"
    }
    lassign $opts(-rect) ll_x ll_y ur_x ur_y
    return [add_shape_rect_cmd $opts(-shape) $ll_x $ll_y $ur_x $ur_y]
}

proc add_shape_polygon {args} {
    array set opts {-shape {} -points {}}
    foreach {flag value} $args {
        if {![info exists opts($flag)]} {
            error "add_shape_polygon: unknown flag $flag"
        }
        set opts($flag) $value
    }
    foreach required {-shape -points} {
        if {$opts($required) eq {}} {
            error "add_shape_polygon: $required is required"
        }
    }
    return [add_shape_polygon_cmd $opts(-shape) $opts(-points)]
}

proc add_shape_path {args} {
    array set opts {-shape {} -width {} -points {}}
    foreach {flag value} $args {
        if {![info exists opts($flag)]} {
            error "add_shape_path: unknown flag $flag"
        }
        set opts($flag) $value
    }
    foreach required {-shape -width -points} {
        if {$opts($required) eq {}} {
            error "add_shape_path: $required is required"
        }
    }
    return [add_shape_path_cmd $opts(-shape) $opts(-width) $opts(-points)]
}

proc shape_rects {id} {
    set result {}
    set n [shape_rect_count $id]
    for {set i 0} {$i < $n} {incr i} {
        lappend result [shape_rect_at $id $i]
    }
    return $result
}

proc shape_polygons {id} {
    set result {}
    set polygon_count [shape_polygon_count $id]
    for {set p 0} {$p < $polygon_count} {incr p} {
        set points {}
        set point_count [shape_polygon_point_count $id $p]
        for {set c 0} {$c < $point_count} {incr c} {
            lappend points [shape_polygon_point_at $id $p $c]
        }
        lappend result $points
    }
    return $result
}

# --- GUI (Phase 6 - see TCL_EXPLORATION.md) ---

# Deliberate stub, not a silently missing feature: this project's only
# GUI is the Flutter app, a separate Dart/Flutter runtime consuming
# api/render/io via FFI - this Tcl shell can't "just start rendering" on
# the same in-process state without embedding a full FlutterEngine
# alongside its own Tcl event loop (OpenROAD's actual gui_start model),
# a substantial, separate integration effort TCL_EXPLORATION.md's Phase
# 6 section explicitly defers rather than fakes. Prints instead of
# silently no-op-ing so a caller typing `show_gui` learns why nothing
# happened, not just that nothing did.
proc show_gui {args} {
    puts "show_gui: not implemented - this Tcl shell doesn't embed the Flutter GUI in-process yet. See TCL_EXPLORATION.md's Phase 6 section for why and what real support would take."
}

proc shape_paths {id} {
    set result {}
    set path_count [shape_path_count $id]
    for {set p 0} {$p < $path_count} {incr p} {
        set points {}
        set point_count [shape_path_point_count $id $p]
        for {set c 0} {$c < $point_count} {incr c} {
            lappend points [shape_path_point_at $id $p $c]
        }
        lappend result [dict create width_um [shape_path_width_um $id $p] points $points]
    }
    return $result
}
