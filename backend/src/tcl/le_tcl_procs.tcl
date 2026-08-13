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

# --- Current view (UPDATES.md item 17) ---

# Selects `name`'s Design as this session's current view - every
# subsequent get_terminals/get_obstructions/get_terminal_ports call is
# scoped to its Abstract (see le_tcl_shim.hpp's own comment on
# get_terminals for why: a script's "give me the terminals" means "in the
# view I have open", not "across every open Library/Design"). `-view` is
# accepted but currently only "abstract" is meaningful - every Design
# read via read_lef() has exactly one Abstract view and no
# DEF/placement-driven Design exists in this project yet (see
# le_tcl_shim.hpp's design_abstract_id comment for the same caveat).
proc open_design {name args} {
    array set opts {-view abstract}
    foreach {flag value} $args {
        if {![info exists opts($flag)]} {
            error "open_design: unknown flag $flag"
        }
        set opts($flag) $value
    }
    if {$opts(-view) ne "abstract"} {
        error "open_design: -view $opts(-view) is not supported - only \"abstract\" is currently meaningful"
    }
    set design_id [design_by_name $name]
    if {$design_id == $::kInvalidId} {
        error "open_design: no such design \"$name\""
    }
    if {[set_current_design_cmd $design_id] != 0} {
        error "open_design: failed to select design \"$name\""
    }
    # design:<name>, not the raw design_id, for consistency with UPDATES.md
    # item 19.1's own friendly-id convention - the caller already has
    # `name` literally, so this costs nothing to derive.
    return "design:$name"
}

# --- get_<type> (UPDATES.md item 19.1) ---
#
# `get_<type> [<name-expr>...] [-of <parent-token>...] [-filter <expr>]
# [-help]` - one shared shape across every object type. parse_get_args
# tokenizes a proc's own `args` into that shape; each has_name_expr=0
# type (Abstract/TerminalPort/Obstruction/Shape - none have a name field,
# see UPDATES.md item 19.1's own NOTE) rejects a bare positional token
# instead of silently ignoring it. `-of`'s own value is itself a Tcl list
# (same idiom as `-rect {...}`/`-points {...}` elsewhere in this file) -
# `-of design:A` and `-of {design:A design:B}` both work, the latter OR'd
# (UPDATES.md item 19.1: "-of <parent tokens>" is plural on purpose).
proc parse_get_args {cmd_name args_list has_name_expr} {
    set name_exprs {}
    set of_tokens {}
    set filter {}
    set help 0

    set i 0
    set n [llength $args_list]
    while {$i < $n} {
        set token [lindex $args_list $i]
        if {$token eq "-help"} {
            set help 1
            incr i
        } elseif {$token eq "-of"} {
            if {$i + 1 >= $n} {
                error "$cmd_name: -of requires a value"
            }
            foreach t [lindex $args_list [expr {$i + 1}]] {
                lappend of_tokens $t
            }
            incr i 2
        } elseif {$token eq "-filter"} {
            if {$i + 1 >= $n} {
                error "$cmd_name: -filter requires a value"
            }
            set filter [lindex $args_list [expr {$i + 1}]]
            incr i 2
        } elseif {[string index $token 0] eq "-"} {
            error "$cmd_name: unknown flag $token"
        } else {
            if {!$has_name_expr} {
                error "$cmd_name: this object type has no name - only -of/-filter/-help are valid"
            }
            lappend name_exprs $token
            incr i
        }
    }

    return [dict create name_exprs $name_exprs of_tokens $of_tokens filter $filter help $help]
}

# Every -of token must be validated against `cmd_name`'s own valid
# parent-type prefix set *before* any shim call (UPDATES.md item 19.1's
# error-checking requirement 1) - a wrong-type token is a script bug, not
# an empty-result-shaped "not found".
proc check_of_prefixes {cmd_name of_tokens prefixes} {
    foreach token $of_tokens {
        set matched 0
        foreach prefix $prefixes {
            if {[string match "${prefix}:*" $token]} {
                set matched 1
                break
            }
        }
        if {!$matched} {
            error "$cmd_name: -of only accepts [join $prefixes {: or }]: tokens (got \"$token\") - only [join $prefixes { or }] objects are valid parents for $cmd_name"
        }
    }
}

# {} (a single empty-string element) is the "axis not given" default for
# both name-expressions and -of tokens - each shim *_cmd already treats
# an empty name_expression/of_token as "skip this axis"/"use the default
# scope" (see le_tcl_shim.hpp's own "IDs" comment), so looping a
# one-element list holding that empty string through the same call path
# as a real value needs no special-casing here.
proc default_to_unset {values} {
    if {[llength $values] == 0} {
        return {{}}
    }
    return $values
}

proc get_libraries {args} {
    set parsed [parse_get_args get_libraries $args 1]
    if {[dict get $parsed help]} {
        return "get_libraries \[<name-expr>...\] \[-filter <expr>\] \[-help\] - Libraries loaded this session (no -of: Library has no parent)"
    }
    if {[llength [dict get $parsed of_tokens]] > 0} {
        error "get_libraries: -of is not valid here - Library has no parent object type"
    }
    set filter [dict get $parsed filter]

    set result {}
    foreach name_expr [default_to_unset [dict get $parsed name_exprs]] {
        set count [get_libraries_cmd $name_expr $filter]
        for {set i 0} {$i < $count} {incr i} {
            lappend result [get_libraries_at $i]
        }
    }
    return [lsort -unique $result]
}

proc get_designs {args} {
    set parsed [parse_get_args get_designs $args 1]
    if {[dict get $parsed help]} {
        return "get_designs \[<name-expr>...\] \[-of <library-token>...\] \[-filter <expr>\] \[-help\] - Designs (default: current view's Library, or every Library if none open)"
    }
    check_of_prefixes get_designs [dict get $parsed of_tokens] library
    set filter [dict get $parsed filter]

    set result {}
    foreach of_token [default_to_unset [dict get $parsed of_tokens]] {
        foreach name_expr [default_to_unset [dict get $parsed name_exprs]] {
            set count [get_designs_cmd $of_token $name_expr $filter]
            for {set i 0} {$i < $count} {incr i} {
                lappend result [get_designs_at $i]
            }
        }
    }
    return [lsort -unique $result]
}

proc get_abstracts {args} {
    set parsed [parse_get_args get_abstracts $args 0]
    if {[dict get $parsed help]} {
        return "get_abstracts \[-of <design-token>...\] \[-filter <expr>\] \[-help\] - Abstract views (default: current view's Abstract)"
    }
    check_of_prefixes get_abstracts [dict get $parsed of_tokens] design
    set filter [dict get $parsed filter]

    set result {}
    foreach of_token [default_to_unset [dict get $parsed of_tokens]] {
        foreach id [get_abstracts_cmd $of_token $filter] {
            lappend result $id
        }
    }
    return [lsort -unique $result]
}

proc get_terminals {args} {
    set parsed [parse_get_args get_terminals $args 1]
    if {[dict get $parsed help]} {
        return "get_terminals \[<name-expr>...\] \[-of <abstract-token>...\] \[-filter <expr>\] \[-help\] - Terminals (default: current view's Abstract)"
    }
    check_of_prefixes get_terminals [dict get $parsed of_tokens] abstract
    set filter [dict get $parsed filter]

    set result {}
    foreach of_token [default_to_unset [dict get $parsed of_tokens]] {
        foreach name_expr [default_to_unset [dict get $parsed name_exprs]] {
            set count [get_terminals_cmd $of_token $name_expr $filter]
            for {set i 0} {$i < $count} {incr i} {
                lappend result [get_terminals_at $i]
            }
        }
    }
    return [lsort -unique $result]
}

proc get_terminal_ports {args} {
    set parsed [parse_get_args get_terminal_ports $args 0]
    if {[dict get $parsed help]} {
        return "get_terminal_ports \[-of <terminal-token>...\] \[-filter <expr>\] \[-help\] - TerminalPorts (default: current view's Abstract's Terminals' Ports)"
    }
    check_of_prefixes get_terminal_ports [dict get $parsed of_tokens] terminal
    set filter [dict get $parsed filter]

    set result {}
    foreach of_token [default_to_unset [dict get $parsed of_tokens]] {
        foreach id [get_terminal_ports_cmd $of_token $filter] {
            lappend result $id
        }
    }
    return [lsort -unique $result]
}

proc get_obstructions {args} {
    set parsed [parse_get_args get_obstructions $args 0]
    if {[dict get $parsed help]} {
        return "get_obstructions \[-of <abstract-token>...\] \[-filter <expr>\] \[-help\] - Obstructions (default: current view's Abstract)"
    }
    check_of_prefixes get_obstructions [dict get $parsed of_tokens] abstract
    set filter [dict get $parsed filter]

    set result {}
    foreach of_token [default_to_unset [dict get $parsed of_tokens]] {
        foreach id [get_obstructions_cmd $of_token $filter] {
            lappend result $id
        }
    }
    return [lsort -unique $result]
}

proc get_shapes {args} {
    set parsed [parse_get_args get_shapes $args 0]
    if {[dict get $parsed help]} {
        return "get_shapes \[-of <terminal_port-or-obstruction-token>...\] \[-filter <expr>\] \[-help\] - Shapes (default: current view's Terminals' Ports' shapes, union'd with its Obstructions' shapes)"
    }
    check_of_prefixes get_shapes [dict get $parsed of_tokens] {terminal_port obstruction}
    set filter [dict get $parsed filter]

    set result {}
    foreach of_token [default_to_unset [dict get $parsed of_tokens]] {
        if {[string match "terminal_port:*" $of_token]} {
            foreach id [get_shapes_cmd $of_token {} $filter] { lappend result $id }
        } elseif {[string match "obstruction:*" $of_token]} {
            foreach id [get_shapes_cmd {} $of_token $filter] { lappend result $id }
        } else {
            foreach id [get_shapes_cmd {} {} $filter] { lappend result $id }
        }
    }
    return [lsort -unique $result]
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
