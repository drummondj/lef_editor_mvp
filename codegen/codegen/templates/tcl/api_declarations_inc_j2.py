TEMPLATE = """// GENERATED - do not edit by hand. Regenerate via the regen-tcl skill
// (codegen --target tcl). #include'd once from api.hpp - see this
// project's own "generated code, never hand-edited" convention
// (backend/CLAUDE.md's Database codegen section; this is the TCL-surface
// analog of it).
//
// Plain-C declarations for the generated TCL property-reading surface -
// one Id typedef, friendly-id-by-name lookup (name-indexed classes
// only), property table accessors, and is_child-field enumeration pairs
// per class (see codegen/codegen/tcl_generator.py's HAND_WRITTEN_CLASSES
// for which classes this covers - Library/Design/Abstract/Terminal/
// TerminalPort/Obstruction/Shape already have their own hand-written
// equivalents and are deliberately excluded here).

// --- Id types (typedef'd first, in one pass, so every function below -
// including another class's child-enumeration accessor returning this
// class's Id - can reference any of them regardless of declaration
// order in schema.py). ---
{% for klass in classes %}
/// @brief Mirrors the database's {{klass.name}}Id handle - see
/// LeLibraryId's own comment (api.hpp) for the general contract.
typedef struct Le{{klass.name}}Id
{
    uint32_t index;
    uint32_t generation;
} Le{{klass.name}}Id;
{% endfor %}

// --- Friendly-id-by-name lookups (name-indexed classes only) ---
{% for klass in classes %}
{%- set id_field = klass.tcl_friendly_id_field() %}
{%- if id_field %}
/// @brief The {{klass.name}}Id whose {{id_field.name}} matches `{{id_field.name}}`
/// (Root::get_{{klass.to_snake_case()}}_by_{{id_field.name}}, a real global cmg
/// index=True lookup). Returns an invalid id (index == UINT32_MAX) if
/// handle/{{id_field.name}} is null or nothing matches.
Le{{klass.name}}Id le_{{klass.to_snake_case()}}_by_{{id_field.name}}(LeHandle *handle, const char *{{id_field.name}});

/// @brief The {{id_field.name}} of the {{klass.name}} at `id` - the reverse of
/// le_{{klass.to_snake_case()}}_by_{{id_field.name}} above, needed to format a friendly id
/// from an Id alone (e.g. when enumerating a parent's children). Owned
/// by the handle's Root - valid until the handle is destroyed. Returns
/// nullptr if handle is null or id doesn't name a {{klass.name}} on this handle.
const char *le_{{klass.to_snake_case()}}_{{id_field.name}}(LeHandle *handle, Le{{klass.name}}Id id);
{% endif -%}
{% endfor %}

// --- Property tables (count/at/path - see le_terminal_property_count's
// own api.hpp comment for the general by-id property-table contract
// every one of these mirrors) ---
{% for klass in classes %}
int32_t le_{{klass.to_snake_case()}}_property_count(LeHandle *handle, Le{{klass.name}}Id id);
LeProperty le_{{klass.to_snake_case()}}_property_at(LeHandle *handle, Le{{klass.name}}Id id, int32_t index);
LeProperty le_{{klass.to_snake_case()}}_property_path(LeHandle *handle, Le{{klass.name}}Id id, const char *path);
{% endfor %}

// --- is_child list field enumeration (count/at) - to_properties() never
// includes these (structural, not struct fields), so a script needs a
// dedicated way to reach e.g. Technology's own Layers - same shape as
// the existing hand-written le_terminal_port_shape_count/_at. ---
{% for klass in classes %}
{%- for child_field in klass.tcl_child_list_fields() %}
int32_t le_{{klass.to_snake_case()}}_{{child_field.name}}_count(LeHandle *handle, Le{{klass.name}}Id id);
Le{{child_field.type}}Id le_{{klass.to_snake_case()}}_{{child_field.name}}_at(LeHandle *handle, Le{{klass.name}}Id id, int32_t index);
{% endfor -%}
{% endfor %}
"""
