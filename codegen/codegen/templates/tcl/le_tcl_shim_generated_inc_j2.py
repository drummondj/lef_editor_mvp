TEMPLATE = """// GENERATED - do not edit by hand. Regenerate via the regen-tcl skill
// (codegen --target tcl). #include'd once in le_tcl_shim.cpp, right
// after the file's own anonymous namespace closes (so session()/pack/
// unpack/return_string/format_property_value/
// resolve_numeric_friendly_id/format_numeric_friendly_id are all already
// in scope - unqualified name lookup finds them regardless of being
// outside that namespace, same translation unit).
//
// Mirrors the hand-written resolve_obstruction_id/format_obstruction_id
// (numeric friendly ids) and resolve_library_id/format_library_id (name-
// based friendly ids) plus terminal_property_count/_at/_path-style bare
// shim functions (le_tcl_shim.cpp) exactly, generated instead of
// duplicated per class.

// --- Friendly-id resolve/format (emitted before anything that calls
// them, e.g. an is_child enumeration function formatting a child's id
// with a different class's own format_X_id). ---
{% for klass in classes %}
{%- set id_field = klass.tcl_friendly_id_field() %}
constexpr std::string_view k{{klass.name}}Prefix = "{{klass.to_snake_case()}}:";
{% if id_field -%}
std::string format_{{klass.to_snake_case()}}_id(const char *{{id_field.name}}) { return std::string(k{{klass.name}}Prefix) + ({{id_field.name}} ? {{id_field.name}} : ""); }

// Overload taking the Id directly (not just its {{id_field.name}}) - needed to
// format a friendly id when only an Id is in hand, e.g. formatting each
// element of a parent's child-enumeration list below.
std::string format_{{klass.to_snake_case()}}_id(Le{{klass.name}}Id id) { return format_{{klass.to_snake_case()}}_id(le_{{klass.to_snake_case()}}_{{id_field.name}}(session(), id)); }

// Fixed-prefix compare, empty/malformed input degrades to the same
// invalid sentinel as "no such object" - see resolve_library_id's own
// comment (le_tcl_shim.cpp) for why.
Le{{klass.name}}Id resolve_{{klass.to_snake_case()}}_id(const char *s)
{
    const Le{{klass.name}}Id invalid{.index = UINT32_MAX, .generation = 0};
    if (!s)
        return invalid;
    std::string_view sv(s);
    if (sv.substr(0, k{{klass.name}}Prefix.size()) != k{{klass.name}}Prefix)
        return invalid;
    return le_{{klass.to_snake_case()}}_by_{{id_field.name}}(session(), std::string(sv.substr(k{{klass.name}}Prefix.size())).c_str());
}
{% else -%}
Le{{klass.name}}Id resolve_{{klass.to_snake_case()}}_id(const char *s) { return resolve_numeric_friendly_id<Le{{klass.name}}Id>(s, k{{klass.name}}Prefix); }
std::string format_{{klass.to_snake_case()}}_id(Le{{klass.name}}Id id) { return format_numeric_friendly_id(id, k{{klass.name}}Prefix); }
{% endif %}
{% endfor %}

// --- Property tables ---
{% for klass in classes %}
int {{klass.to_snake_case()}}_property_count(const char *id)
{
    return le_{{klass.to_snake_case()}}_property_count(session(), resolve_{{klass.to_snake_case()}}_id(id));
}

const char *{{klass.to_snake_case()}}_property_name(const char *id, int index)
{
    return le_{{klass.to_snake_case()}}_property_at(session(), resolve_{{klass.to_snake_case()}}_id(id), index).name;
}

const char *{{klass.to_snake_case()}}_property_value(const char *id, int index)
{
    return return_string(format_property_value(le_{{klass.to_snake_case()}}_property_at(session(), resolve_{{klass.to_snake_case()}}_id(id), index)));
}

const char *{{klass.to_snake_case()}}_property_path(const char *id, const char *path)
{
    return return_string(format_property_value(le_{{klass.to_snake_case()}}_property_path(session(), resolve_{{klass.to_snake_case()}}_id(id), path)));
}
{% endfor %}

// --- is_child list field enumeration - space-separated friendly-id
// lists (same shape as the hand-written terminal_port_shapes/
// obstruction_shapes). ---
{% for klass in classes %}
{%- for child_field in klass.tcl_child_list_fields() %}
const char *{{klass.to_snake_case()}}_{{child_field.name}}(const char *id)
{
    Le{{klass.name}}Id parent_id = resolve_{{klass.to_snake_case()}}_id(id);
    int32_t count = le_{{klass.to_snake_case()}}_{{child_field.name}}_count(session(), parent_id);
    if (count <= 0)
        return return_string("");
    std::ostringstream out;
    for (int32_t i = 0; i < count; ++i)
    {
        if (i > 0)
        {
            out << ' ';
        }
        out << format_{{child_field._type_klass.to_snake_case()}}_id(le_{{klass.to_snake_case()}}_{{child_field.name}}_at(session(), parent_id, i));
    }
    return return_string(out.str());
}
{% endfor -%}
{% endfor %}
"""
