TEMPLATE = """# GENERATED - do not edit by hand. Regenerate via the regen-tcl skill
# (codegen --target tcl). Sourced once from le_tcl_procs.tcl.
#
# Replaces the old hand-written property_accessors_for_token (a 7-entry
# if/elseif chain covering only Library/Design/Abstract/Terminal/
# TerminalPort/Obstruction/Shape) with one covering every TCL-readable
# pool-backed class - see codegen/codegen/schema.py's Klass.tcl_readable.

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

# is_child list field enumeration (e.g. technology_layers $id) needs no
# Tcl-level wrapper here - the SWIG-wrapped bare C function of that exact
# name (le_tcl_shim_generated.hpp/.inc) is already the callable Tcl
# command, same as the hand-written terminal_port_shapes/
# obstruction_shapes (le_tcl_shim.hpp) are called directly with no Tcl
# proc of their own. A same-named `proc` here would shadow the C command
# and recurse into itself instead of calling it.
"""
