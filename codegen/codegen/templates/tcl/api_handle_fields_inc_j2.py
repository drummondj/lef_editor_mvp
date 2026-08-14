TEMPLATE = """// GENERATED - do not edit by hand. Regenerate via the regen-tcl skill
// (codegen --target tcl). #include'd once inside struct LeHandle's body
// (api.cpp), right before its closing brace.
//
// Single-slot property-table caches, same pattern as the hand-written
// cached_terminal_property_id/cached_terminal_properties pair (see that
// pair's own comment in api.cpp) - one pair per generated class.
{% for klass in classes %}
le::{{klass.name}}Id cached_{{klass.to_snake_case()}}_property_id{};
std::vector<le::PropertyValue> cached_{{klass.to_snake_case()}}_properties;
{% endfor %}
"""
