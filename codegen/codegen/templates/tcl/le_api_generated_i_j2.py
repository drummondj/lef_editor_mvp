TEMPLATE = """// GENERATED - do not edit by hand. Regenerate via the regen-tcl skill
// (codegen --target tcl). %include'd once from le_api.i.
//
// SWIG declarations for the generated property-reading surface - plain
// positional forms, same as every hand-written declaration in le_api.i
// (SWIG-wrapped C++ functions are always positional; no `-flag value`
// parsing happens here, that's le_tcl_procs.tcl's job).

{% for klass in classes %}
int {{klass.to_snake_case()}}_property_count(const char *id);
const char *{{klass.to_snake_case()}}_property_name(const char *id, int index);
const char *{{klass.to_snake_case()}}_property_value(const char *id, int index);
const char *{{klass.to_snake_case()}}_property_path(const char *id, const char *path);
{% endfor %}
{% for klass in classes %}
{%- for child_field in klass.tcl_child_list_fields() %}
const char *{{klass.to_snake_case()}}_{{child_field.name}}(const char *id);
{% endfor -%}
{% endfor %}
"""
