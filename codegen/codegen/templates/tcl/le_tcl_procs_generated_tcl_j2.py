TEMPLATE = """# GENERATED - do not edit by hand. Regenerate via the regen-tcl skill
# (codegen --target tcl). Sourced once from le_tcl_procs.tcl.
#
# property_accessors_for_token (dispatches a friendly-id token to its
# {count name value path} shim-function quadruplet by prefix, across
# every TCL-readable class) and get_<type>/set_current_<type> - all
# generated - see backend/CLAUDE.md's TCL codegen section. Never edit
# this file directly, regenerate via the regen-tcl skill instead.
# parse_get_args/check_of_prefixes/default_to_unset (le_tcl_procs.tcl)
# are shared, class-agnostic helpers this file calls into, not generated
# here.

# Maps a friendly-id token to its {count name value path} shim-function
# quadruplet - see le_tcl_procs.tcl's own property_accessors_for_token
# comment (properties_for_token/get_properties/report_properties) for
# the full contract this backs.
proc property_accessors_for_token {token} {
{%- for klass in readable_classes %}
    if {[string match "{{klass.to_snake_case()}}:*" $token]} {
        return {{'{'}}{{klass.to_snake_case()}}_property_count {{klass.to_snake_case()}}_property_name {{klass.to_snake_case()}}_property_value {{klass.to_snake_case()}}_property_path{{'}'}}
    }
{%- endfor %}
    error "get_properties: unrecognized token \\"$token\\" - expected a friendly id ({% for klass in readable_classes %}{{klass.to_snake_case()}}:{% if not loop.last %}/{% endif %}{% endfor %})"
}

# --- Current-instance access ---
{% for klass in current_access_classes %}
# Sets the current {{klass.name}} (see current_{{klass.to_snake_case()}} to read it back) - every
# other readable class's get_<type> default (-of omitted) scope derives
# from this, see codegen/codegen/tcl_scope.py's own module docstring.
proc set_current_{{klass.to_snake_case()}} {id} {
    if {[set_current_{{klass.to_snake_case()}}_cmd $id] != 0} {
        error "set_current_{{klass.to_snake_case()}}: failed to select \\"$id\\""
    }
    return {}
}
{% endfor %}

# --- get_<type> search ---
{% for klass in classes %}
{%- set scope = search_scopes[klass.name] %}
{%- set id_field = klass.tcl_friendly_id_field() %}
{%- set plural = klass.tcl_plural_snake_case() %}
proc get_{{plural}} {args} {
    set parsed [parse_get_args get_{{plural}} $args {{ 1 if id_field else 0 }}]
    if {[dict get $parsed help]} {
        return "get_{{plural}} {% if id_field %}\\[<name-expr>...\\] {% endif %}{% if scope.of_params %}\\[-of <token>...\\] {% endif %}\\[-filter <expr>\\] \\[-help\\] - {{klass.description}}"
    }
    {%- if scope.of_params %}
    check_of_prefixes get_{{plural}} [dict get $parsed of_tokens] {{'{'}}{% for op in scope.of_params %}{{op.parent_klass.to_snake_case()}}{% if not loop.last %} {% endif %}{% endfor %}{{'}'}}
    {%- else %}
    if {[llength [dict get $parsed of_tokens]] > 0} {
        error "get_{{plural}}: -of is not valid here - {{klass.name}} has no parent object type"
    }
    {%- endif %}
    set filter [dict get $parsed filter]

    set result {}
    foreach of_token [default_to_unset [dict get $parsed of_tokens]] {
        {%- for op in scope.of_params %}
        set of_{{op.parent_field.name}} {}
        {%- endfor %}
        {%- for op in scope.of_params %}
        if {[string match "{{op.parent_klass.to_snake_case()}}:*" $of_token]} {
            set of_{{op.parent_field.name}} $of_token
        }
        {%- endfor %}
        {%- if id_field %}
        foreach name_expr [default_to_unset [dict get $parsed name_exprs]] {
            set count [get_{{plural}}_cmd {% for op in scope.of_params %}$of_{{op.parent_field.name}} {% endfor %}$name_expr $filter]
            for {set i 0} {$i < $count} {incr i} {
                lappend result [get_{{plural}}_at $i]
            }
        }
        {%- else %}
        set count [get_{{plural}}_cmd {% for op in scope.of_params %}$of_{{op.parent_field.name}} {% endfor %}$filter]
        for {set i 0} {$i < $count} {incr i} {
            lappend result [get_{{plural}}_at $i]
        }
        {%- endif %}
    }
    return [lsort -unique $result]
}
{% endfor %}

# --- create_<type> - `create_<type> [-flag value...]`, one flag per
# scalar field plus one per parent (a friendly-id token, e.g. `-abstract
# abstract:0` - see api_declarations_inc_j2's own comment for the field-
# scope/optionality conventions this follows uniformly). A parent flag
# whose own Klass has has_current_access=True (e.g. -abstract, since
# Abstract does) defaults to current_<parent_klass> when omitted (see
# Klass.create_tcl_current_defaults() - only when every other parent
# flag was also left empty, for a multi-parent class), the same way
# get_<type>'s own default (-of omitted) scope already derives from
# current_abstract - so create_terminal needs no explicit -abstract once
# set_current_abstract/open_design has been called. A multi-parent class
# (e.g. Shape's -terminal_port/-obstruction) requires exactly one of its
# parent flags to resolve (after that default is applied), checked here
# before calling down - same "exactly one" rule create_<type>_cmd's own
# C++ body re-checks (defense in depth, not redundant: a caller could
# reach the *_cmd form directly). ---
{% for klass in classes %}
{%- set snake = klass.to_snake_case() %}
{%- set parent_fields = klass.get_parent_fields() %}
proc create_{{snake}} {args} {
    array set opts {{'{'}}{{klass.create_tcl_flag_defaults()}}{{'}'}}
    foreach {flag value} $args {
        if {![info exists opts($flag)]} {
            error "create_{{snake}}: unknown flag $flag"
        }
        set opts($flag) $value
    }
{{klass.create_tcl_current_defaults()}}
    {%- if parent_fields|length > 1 %}
    set provided_parents 0
    {%- for pf in parent_fields %}
    if {$opts(-{{pf.name}}) ne {}} { incr provided_parents }
    {%- endfor %}
    if {$provided_parents != 1} {
        error "create_{{snake}}: exactly one of {% for pf in parent_fields %}-{{pf.name}}{% if not loop.last %}/{% endif %}{% endfor %} is required"
    }
    {%- endif %}
    foreach required {{'{'}}{{klass.create_tcl_required_flags()}}{{'}'}} {
        if {$opts($required) eq {}} {
            error "create_{{snake}}: $required is required"
        }
    }
{{klass.cmd_tcl_preamble('create')}}
    return [create_{{snake}}_cmd {{klass.create_tcl_call_args()}}]
}
{% endfor %}

# --- update_<type> - `update_<type> <id> -flag value...` - mutates an
# *existing* object in place; the *only* way any field is ever updated
# (no per-field setter, generated or hand-written, exists anymore - see
# this round's own plan/commit message). Same flag set as create_<type>
# (same compound/enum/dbu conventions - see api_declarations_inc_j2's
# own comment), but every flag here means "leave unchanged" when
# omitted, the opposite of create_<type>'s "omitted means unset" - so,
# unlike create_<type>, nothing is ever required *except* that at least
# one flag must be given (an update with zero flags is a no-op call,
# rejected here rather than silently doing nothing). A single-parent
# class also accepts its own parent flag (e.g. -abstract) to reassign
# it; a multi-parent class (e.g. Shape) accepts none at all - passing
# -terminal_port/-obstruction to update_shape is an "unknown flag"
# error, same as any other flag this class doesn't have. Returns the
# (possibly-changed, e.g. after a rename) friendly id token. ---
{% for klass in classes %}
{%- set snake = klass.to_snake_case() %}
proc update_{{snake}} {id args} {
    if {[llength $args] == 0} {
        error "update_{{snake}}: at least one -flag is required"
    }
    array set opts {{'{'}}{{klass.update_tcl_flag_defaults()}}{{'}'}}
    foreach {flag value} $args {
        if {![info exists opts($flag)]} {
            error "update_{{snake}}: unknown flag $flag"
        }
        set opts($flag) $value
    }
{{klass.cmd_tcl_preamble('update')}}
    set new_id [update_{{snake}}_cmd $id {{klass.update_tcl_call_args()}}]
    if {$new_id eq {}} {
        error "update_{{snake}}: failed to update \\"$id\\""
    }
    return $new_id
}
{% endfor %}
"""
