TEMPLATE = """
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace {{schema.namespace}}
{
    /**
        @brief One name/value row of an object's schema-derived property
        table (see each generated to_properties() function).
    */
    struct PropertyValue
    {
        enum class Type
        {
            STRING,
            INT,
            DOUBLE,
        };

        std::string name;
        Type type;
        std::string string_value;
        int64_t int_value = 0;
        double double_value = 0.0;

        static PropertyValue make_string(std::string name, std::string value)
        {
            return PropertyValue{.name = std::move(name), .type = Type::STRING, .string_value = std::move(value)};
        }

        static PropertyValue make_int(std::string name, int64_t value)
        {
            return PropertyValue{.name = std::move(name), .type = Type::INT, .int_value = value};
        }

        static PropertyValue make_double(std::string name, double value)
        {
            return PropertyValue{.name = std::move(name), .type = Type::DOUBLE, .double_value = value};
        }
    };

    /**
        @brief Joins a list of embedded (non-pooled) Klass values into one
        Tcl-list-friendly string for to_properties() (INDEXED_POOLS export
        style) - each item wrapped in its own "{...}" so a Tcl foreach/
        lindex can walk the list, with that item's own to_property_string()
        (already brace-balanced - every generated to_property_string()
        opens "Name{" and closes with a matching " }", and - unlike
        to_string() - recurses fully into any list/reference fields that
        item itself has, e.g. Polygon's own points list) supplying the
        full contents in place of a bare "<field>_count". Relies on
        argument-dependent lookup to find each element type's own
        to_property_string() overload, so this only compiles at the call
        site in a generated struct header that has already #include'd it
        - never called directly, only from generated to_properties()/
        to_property_string() bodies.
    */
    template <typename T>
    inline std::string to_property_list_string(const std::vector<T> &items)
    {
        std::string result;
        for (std::size_t i = 0; i < items.size(); ++i)
        {
            if (i != 0)
                result += " ";
            result += "{" + to_property_string(items[i]) + "}";
        }
        return result;
    }
}
"""
