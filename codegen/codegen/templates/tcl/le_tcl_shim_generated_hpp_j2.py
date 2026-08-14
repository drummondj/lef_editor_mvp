TEMPLATE = """// GENERATED - do not edit by hand. Regenerate via the regen-tcl skill
// (codegen --target tcl). #include'd once from le_tcl_shim.hpp.
//
// Bare, handle-free TCL-facing declarations for the generated property-
// reading surface - same shape as the hand-written
// terminal_property_count/_at/_path etc. (le_tcl_shim.hpp's own
// "Property tables and search results" comment) for every class
// codegen/codegen/tcl_generator.py's HAND_WRITTEN_CLASSES doesn't
// already cover by hand.

{% for klass in classes %}
int {{klass.to_snake_case()}}_property_count(const char *id);
const char *{{klass.to_snake_case()}}_property_name(const char *id, int index);
const char *{{klass.to_snake_case()}}_property_value(const char *id, int index);
const char *{{klass.to_snake_case()}}_property_path(const char *id, const char *path);
{% endfor %}
// --- is_child list field enumeration - space-separated friendly-id
// lists, same shape as the hand-written terminal_port_shapes/
// obstruction_shapes. ---
{% for klass in classes %}
{%- for child_field in klass.tcl_child_list_fields() %}
const char *{{klass.to_snake_case()}}_{{child_field.name}}(const char *id);
{% endfor -%}
{% endfor %}
"""
