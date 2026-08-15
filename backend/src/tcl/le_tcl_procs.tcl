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
# aggregation (properties_for_token, shape_rects, ...) loop over the
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
# subsequent get_terminals/get_obstructions/get_terminal_ports/get_shapes
# call (whose default scope, absent an explicit -of, derives from
# current_abstract - see codegen/codegen/tcl_scope.py's own module
# docstring) is scoped to its Abstract, since a script's "give me the
# terminals" means "in the view I have open", not "across every open
# Library/Design". Sets the generated current_abstract independently of
# Scene::current_abstract() (which drives GUI rendering and is untouched
# here) - see backend/CLAUDE.md's TCL codegen section for why those two
# are deliberately separate. `-view` is accepted but currently only
# "abstract" is meaningful - every Design read via read_lef() has exactly
# one Abstract view and no DEF/placement-driven Design exists in this
# project yet (see le_tcl_shim.hpp's design_abstract_id comment for the
# same caveat).
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
    set design_token "design:$name"
    set abstracts [get_abstracts -of $design_token]
    if {[llength $abstracts] > 0} {
        set_current_abstract [lindex $abstracts 0]
    }
    return $design_token
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

# --- get_properties/report_properties (UPDATES.md item 19.2) ---
#
# property_accessors_for_token (dispatches a friendly-id token to its
# {count name value path} shim-function quadruplet by prefix, across
# every TCL-readable class - not just library:/design:/abstract:/
# terminal:/terminal_port:/obstruction:/shape:) is generated - see
# generated/le_tcl_procs_generated.tcl and backend/CLAUDE.md's TCL
# section. Never edit that file directly, regenerate via the regen-tcl
# skill instead.
source [file join [file dirname [info script]] generated le_tcl_procs_generated.tcl]

# All properties for one token, as a dict - the shared building block
# behind both get_properties and report_properties.
proc properties_for_token {token} {
    lassign [property_accessors_for_token $token] count_cmd name_cmd value_cmd
    set result {}
    set n [$count_cmd $token]
    for {set i 0} {$i < $n} {incr i} {
        dict set result [$name_cmd $token $i] [$value_cmd $token $i]
    }
    return $result
}

# `tokens`/`property_names` each independently collapse from "a list" to
# "one value" when they hold exactly one element - Tcl can't otherwise
# distinguish a single bare token/name from a one-element list of them
# (`terminal:IN0` literal and a one-match [get_terminals] result are
# structurally identical), so this is the only rule that can match every
# one of UPDATES.md item 19.2's own worked examples:
#   get_properties [get_terminals]              -> list of dicts (many tokens)
#   get_properties terminal:IN0 .name           -> scalar (one token, one name)
#   get_properties terminal:IN0 {.name .direction} -> flat list (one token, many names)
#   get_properties [get_terminals] {.name .direction} -> list of flat lists
#
# Each requested property name is a dotted path (`.name`, or chained
# through a hop like `.terminal.name` - backend/src/database/filter.hpp's
# parse_property_path/resolve_property_path grammar, the same one -filter
# expressions already use for their own field paths) resolved via the
# token's own *_property_path shim function - always through this path
# mechanism, even for a plain single-segment name, rather than a separate
# dict-lookup fast path, so chained and unchained lookups behave
# identically. A path that fails to parse or references an unrecognized
# field/hop pushes a message (see le_message_*) that this detects via a
# message_count before/after diff and re-raises as a Tcl error, naming
# the specific problem - a structurally valid path that simply has no
# data for this object (e.g. a list hop with zero elements) pushes no
# message and just resolves to "".
proc get_properties {tokens {property_names {}}} {
    set single_token [expr {[llength $tokens] == 1}]
    set token_list [expr {$single_token ? [list $tokens] : $tokens}]

    set results {}
    foreach token $token_list {
        if {[llength $property_names] == 0} {
            lappend results [properties_for_token $token]
        } else {
            lassign [property_accessors_for_token $token] count_cmd name_cmd value_cmd path_cmd
            set values {}
            foreach path $property_names {
                set messages_before [message_count]
                set value [$path_cmd $token $path]
                if {[message_count] > $messages_before} {
                    error "get_properties: [message_at [expr {[message_count] - 1}]]"
                }
                lappend values $value
            }
            if {[llength $property_names] == 1} {
                lappend results [lindex $values 0]
            } else {
                lappend results $values
            }
        }
    }

    if {$single_token} {
        return [lindex $results 0]
    }
    return $results
}

# Pretty-prints every property of every token to stdout, one block per
# token, names padded (within that token's own block) to align values.
proc report_properties {tokens} {
    foreach token $tokens {
        puts $token
        set props [properties_for_token $token]
        set max_len 0
        foreach name [dict keys $props] {
            if {[string length $name] > $max_len} {
                set max_len [string length $name]
            }
        }
        dict for {name value} $props {
            puts [format "  %-*s %s" [expr {$max_len + 1}] "${name}:" $value]
        }
        puts ""
    }
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
