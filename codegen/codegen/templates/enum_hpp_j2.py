TEMPLATE = """
#pragma once

namespace {{schema.namespace}} {
    /// @brief {{klass.description}}
    enum class {{klass.name}} : uint8_t
    {
    {%- for field in klass.fields %}
        /// @brief {{field.description}}
        {{field.name}} = {{field.value}},
    {%- endfor %}
    };

    inline std::string to_string({{schema.namespace}}::{{klass.name}} value)
    {
        switch (value)
        {
        {%- for field in klass.fields %}
        case {{schema.namespace}}::{{klass.name}}::{{field.name}}:
            return "{{field.name}}";
        {%- endfor %}
        }
        return "UNKNOWN";
    }

    inline std::ostream& operator<<(std::ostream& os, {{schema.namespace}}::{{klass.name}} value) {
        os << to_string(value);
        return os;
    }
}

"""
