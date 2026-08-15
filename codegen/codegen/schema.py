"""A schema used to define the structure of the data in a model."""

from dataclasses import dataclass, field
import os
from typing import Any, List, Optional

"""
Define the type map for C++ types: https://en.cppreference.com/w/cpp/language/types
The key is the type in the schema, the value is a tuple with the C++ type and the Python type
"""
TYPEMAP = {
    "str": ("std::string", str),
    "signed char*": ("std::string", str),
    "unsigned char*": ("std::string", str),
    "char*": ("std::string", str),
    "signed char": ("signed char", int),
    "unsigned char": ("unsigned char", int),
    "char": ("char", int),
    "short": ("short", int),
    "short int": ("short int", int),
    "signed short": ("signed short", int),
    "signed short int": ("signed short int", int),
    "unsigned short": ("unsigned short", int),
    "unsigned short int": ("unsigned short int", int),
    "int": ("int", int),
    "signed": ("signed", int),
    "signed int": ("signed int", int),
    "unsigned": ("unsigned", int),
    "unsigned int": ("unsigned int", int),
    "long": ("long", int),
    "long int": ("long int", int),
    "signed long": ("signed long", int),
    "signed long int": ("signed long int", int),
    "unsigned long": ("unsigned long", int),
    "unsigned long int": ("unsigned long int", int),
    "long long": ("long long", int),
    "long long int": ("long long int", int),
    "signed long long": ("signed long long", int),
    "signed long long int": ("signed long long int", int),
    "unsigned long long": ("unsigned long long", int),
    "unsigned long long int": ("unsigned long long int", int),
    "float": ("float", float),
    "double": ("double", float),
    "long double": ("long double", float),
    "bool": ("bool", bool),
    # A raw value in database units (this project's LEF/DEF integer
    # coordinate/length convention) - a plain int64_t in the generated
    # struct, but a distinct schema-level type so property-formatting code
    # can tell "this needs micron conversion for display" from a plain
    # count/int without guessing from the C++ type name.
    "dbu": ("int64_t", int),
}


def to_snake_case(name: str) -> str:
    """
    Convert a camelCase or PascalCase string to snake_case.

    Args:
        name (str): The name to convert.

    Returns:
        str: The name in snake_case.
    """
    name = "".join(["_" + c.lower() if c.isupper() else c for c in name]).lstrip("_")
    return name


def to_camel_case(name: str | None, upper_first=False) -> str | None:
    """
    Convert a snake_case string to camelCase.

    Args:
        name (str): The name to convert.
        upper_first (bool): Whether to capitalize the first letter.

    Returns:
        str: The name in camelCase.
    """

    if name is None:
        return None
    name = name.replace("_", " ").title().replace(" ", "")
    if upper_first:
        return name
    return name[0].lower() + name[1:]


@dataclass
class Schema:
    """
    A schema used to define the structure of the data in a model.

    Attributes:
        name (str): The name of the schema.

        description (str): The description of the schema.

        namespace (str): The namespace of the schema.

        classes (List[Klass]): The classes in the schema.
    """

    name: str
    description: str
    namespace: str
    version: str
    classes: List["Klass"] = field(default_factory=list)
    _output_dir: str = field(default=".", repr=False, init=False)

    def set_output_dir(self, output_dir: str) -> None:
        """
        Set the output directory for the schema.

        Args:
            output_dir (str): The output directory.
        """
        self._output_dir = os.path.abspath(output_dir)

    def link(self) -> None:
        """
        Link the schema to its classes and fields.
        """
        for klass in self.classes:
            klass._schema = self
            klass.link()

    def get_klass(self, name: str) -> "Klass":
        """
        Get a klass by name.

        Args:
            name (str): The name of the klass.

        Returns:
            Klass: The klass.

        Raises:
            ValueError: If the klass is not found.
        """
        for klass in self.classes:
            if klass.name == name:
                return klass
        raise ValueError(f"Klass not found: {name}")

    def get_field(self, klass_name: str, field_name: str) -> "Field":
        """
        Get a field by klass and field name.

        Args:
            klass_name (str): The name of the klass.
            field_name (str): The name of the field.

        Returns:
            Field: The field.

        Raises:
            ValueError: If the field is not found.
        """
        klass = self.get_klass(klass_name)
        for field in klass.fields:
            if field.name == field_name:
                return field
        raise ValueError(f"Field not found: {field_name} in klass: {klass_name}")

    def get_cmakelists_src(self) -> str:
        """
        Get the source files for the CMakeLists.txt file.

        Returns:
            str: The source files.
        """
        return " ".join([f'"{klass.to_snake_case()}.cpp"' for klass in self.classes])

    def get_lcov_src(self) -> str:
        """
        Get the source files for the lcov command.

        Returns:
            str: The source files.
        """
        sources = [
            "/".join(
                [
                    self._output_dir.replace(os.path.sep, "/").replace("c:", "", 1),
                    f"{klass.to_snake_case()}.cpp",
                ]
            )
            for klass in self.classes
        ]

        sources.extend(
            [
                "/".join(
                    [
                        self._output_dir.replace(os.path.sep, "/").replace("c:", "", 1),
                        f"{klass}.cpp",
                    ]
                )
                for klass in self.get_supplementary_klasses()
            ]
        )
        return " ".join(sources)

    def get_test_includes(self) -> List[str]:
        """
        Get the include statements for the test file.

        Returns:
            List[str]: A list of include statements for each class in the test file.
        """
        return [f'#include "{klass.to_snake_case()}.hpp"' for klass in self.classes]

    def get_supplementary_klasses(self):
        """
        Get the supplementary sources for the schema.

        Returns:
            List[str]: The supplementary sources.
        """
        return ["identifiable", "index"]

    def get_root_klass(self) -> "Klass":
        """
        Get the root klass for the schema.

        Returns:
            Klass: The root klass.

        Raises:
            ValueError: If no root klass is found.
        """
        for klass in self.classes:
            if not klass.has_parent():
                return klass
        raise ValueError("No root klass found")

    def get_pool_classes(self) -> List["Klass"]:
        """
        Return a list of klasses that need pools.
        """
        return [klass for klass in self.classes if klass.has_pool and not klass.is_enum]

    def get_non_pool_classes(self) -> List["Klass"]:
        """
        Return a list of klasses that do not need pools.
        """
        return [
            klass for klass in self.classes if not klass.has_pool and not klass.is_enum
        ]

    def get_classes_without_enums(self) -> list["Klass"]:
        return [klass for klass in self.classes if not klass.is_enum]

    def get_enums(self) -> list["Klass"]:
        return [klass for klass in self.classes if klass.is_enum]


@dataclass
class Klass:
    """
    A class used to define the structure of the data in a model.

    Attributes:
        name (str): The name of the class.

        description (str): The description of the class.

        fields (List[Field]): The fields in the class.

        has_pool (bool): Store this kclass in a pool. Defaults to true.

    """

    name: str
    description: str
    fields: List["Field"] = field(default_factory=list)
    has_pool: bool = True
    is_enum: bool = False
    enum_values: List["EnumValue"] = field(default_factory=list)

    # TCL codegen (backend/src/tcl) - not database codegen. `None` means
    # "default to has_pool": every pool-backed class is TCL-readable
    # (property tables + friendly-id resolution) unless explicitly opted
    # out with tcl_readable=False. See backend/CLAUDE.md's TCL section.
    tcl_readable: Optional[bool] = None

    # Explicit override for the field backing this class's TCL friendly
    # id (e.g. "type:NAME" instead of "type:N"). Auto-derives to the
    # field with index=True if one exists (see tcl_friendly_id_field()) -
    # only needed when a class's real uniqueness isn't captured by
    # index=True (e.g. Terminal, whose name uniqueness is enforced
    # per-Abstract by hand, not a global cmg index).
    tcl_id_field: Optional[str] = None

    # Whether this class has a generated "current instance" concept
    # (a settable Le{Klass}Id on LeHandle, plus generated current_X/
    # set_current_X_cmd TCL commands) - independent of any hand-written
    # "current view" state elsewhere (e.g. Scene::current_abstract(),
    # which drives GUI rendering and is deliberately not bridged to this).
    # Every readable class's generated get_<type> search command derives
    # its default (-of omitted) scope from whichever has_current_access
    # class governs it, purely from schema parent/is_child structure - see
    # codegen/codegen/tcl_scope.py's own module docstring for the full
    # algorithm. Set on the classes that actually have an "in-progress
    # editing view" concept (Abstract, Schematic) plus any natural
    # session-singleton (Technology) - not every class needs this, only
    # ones other classes' defaults should anchor to.
    has_current_access: bool = False

    _schema: Optional[Schema] = field(default=None, repr=False, init=False)

    def is_tcl_readable(self) -> bool:
        """
        Whether this class gets a generated TCL property-reading surface
        (le_X_property_count/_at/_path, friendly-id resolution). Defaults
        to has_pool - a non-pooled (embedded value) class has no
        independent identity to hand out a friendly id for.
        """
        return self.tcl_readable if self.tcl_readable is not None else self.has_pool

    def tcl_friendly_id_field(self) -> Optional["Field"]:
        """
        The field backing this class's TCL friendly id ("type:<value>"),
        or None if this class uses a numeric ("type:<packed index>")
        friendly id instead. tcl_id_field wins if set; otherwise the
        first field with index=True (Root already generates
        get_<klass>_by_<field>() for it - see root_hpp_j2.py - so name-
        based resolution needs no new database-side lookup).
        """
        if self.tcl_id_field is not None:
            for f in self.fields:
                if f.name == self.tcl_id_field:
                    return f
            raise ValueError(
                f"Klass {self.name}: tcl_id_field={self.tcl_id_field!r} does not name a real field"
            )
        for f in self.fields:
            if f.index:
                return f
        return None

    def tcl_indexed_id_field(self) -> Optional["Field"]:
        """
        Like tcl_friendly_id_field(), but only returns a field actually
        backed by a real global Root::get_<klass>_by_<field>() lookup
        (index=True). A tcl_id_field override can name a field that isn't
        globally unique (e.g. Terminal's `name`, unique only per-Abstract,
        enforced by hand - see its own comment in backend/src/database/
        schema.py) - such a field is still the right one for glob-based
        name_expression search (tcl_friendly_id_field() covers that), but
        there's no Root-level by-name lookup to generate a friendly-id
        resolve/format pair or a le_X_by_<field>/le_X_<field> accessor
        pair from, so this returns None for it instead.
        """
        f = self.tcl_friendly_id_field()
        return f if f is not None and f.index else None

    def tcl_child_list_fields(self) -> List["Field"]:
        """
        This class's own is_child list fields (e.g. Technology.layers) -
        the ones the TCL generator emits a friendly-id enumeration
        pair (le_X_<field>_count/_at) and a derived "<field>_count"
        property row for, since to_properties() itself never includes
        is_child fields (they're structural, not struct fields at all).
        """
        return [f for f in self.fields if f.is_child and f.is_list]

    def tcl_plural_snake_case(self) -> str:
        """
        snake_case, pluralized - matches the existing hand-written search
        naming convention (le_get_terminals/get_terminals_cmd/
        get_terminals_at/the "get_terminals" Tcl proc itself are all
        plural; le_search_result_terminal_at and the
        terminal_search_results cache member stay singular - see
        to_snake_case() for those). Simple English pluralization (+s, or
        -y -> -ies after a consonant) - covers every class name in this
        schema; extend here if a future class name needs an irregular
        plural cmg/codegen doesn't already know how to form.
        """
        snake = self.to_snake_case()
        if len(snake) >= 2 and snake[-1] == "y" and snake[-2] not in "aeiou":
            return snake[:-1] + "ies"
        return snake + "s"

    def tcl_child_fields(self) -> List["Field"]:
        """
        Every one of this class's own is_child fields, list or scalar
        (e.g. both Technology.layers - a list - and Design.abstract - a
        single field, since a Design has at most one Abstract). Used by
        codegen/codegen/tcl_scope.py's default-scope graph walk, which
        needs to traverse scalar is_child edges too (Design->Abstract,
        Design->Schematic) - tcl_child_list_fields() alone (list-only)
        would miss them.
        """
        return [f for f in self.fields if f.is_child]

    def get_parent_fields(self) -> List["Field"]:
        """
        Every field on this class that names a parent (has_parent()) -
        usually exactly one, but a class can have more than one distinct
        parent type (e.g. Shape.terminal_port/Shape.obstruction - a Shape
        belongs to exactly one of the two, never both, per schema.py's own
        comment on Shape). Used by tcl_scope.py to walk "up" from a
        has_current_access class's current instance toward an ancestor
        class whose own default get_<type> scope it governs.
        """
        return [f for f in self.fields if f.has_parent()]

    def get_include_define(self) -> str:
        """
        Get the include define for the class.

        Returns:
            str: The include define.
        """
        return f"_{self.name.upper()}_HPP_"

    def get_forward_declarations(self, has_pool: bool | None = None) -> List[str]:
        """
        Get the forward declarations for the class.

        Returns:
            List[str]: The forward declarations.
        """
        forward_declarations = set()
        for field in self.fields:
            if field.is_reference() and field._type_klass is not None:
                if has_pool is not None:
                    if field._type_klass.has_pool != has_pool:
                        continue
                forward_declarations.add(field.type)
        return sorted(forward_declarations)

    def get_forward_includes(self, has_pool: bool | None = None) -> List[str]:
        """
        Get the forward includes for the class.

        Returns:
            List[str]: The forward includes.
        """
        return [
            to_snake_case(dec)
            for dec in self.get_forward_declarations(has_pool=has_pool)
        ]

    def to_snake_case(self) -> str:
        """
        Convert the class name to snake_case.

        Returns:
            str: The class name in snake_case.
        """
        return to_snake_case(self.name)

    def init_fields(self, parents=True) -> List["Field"]:
        """
        Initialize the fields for the class.

        Args:
            parents (bool): Whether to include parent fields.

        Returns:
            List[Field]: The initialized fields.
        """
        init_fields = []
        for field in self.fields:
            if field.has_parent() and not parents:
                continue
            if not field.has_default() and not field.is_child and not field.is_optional:
                init_fields.append(field)
        return init_fields

    def get_create_arguments(self) -> str:
        """
        Get the create arguments for the class.

        Returns:
            str: The create arguments.
        """
        arguments = []
        for field in self.init_fields():
            arguments.append(f"{field.get_cpp_type()} {field.to_camel_case()}")
        return ", ".join(arguments)

    def get_example_arguments(self) -> str:
        """
        Get the example arguments for the class.

        Returns:
            str: The example arguments.
        """
        arguments = []
        for field in self.init_fields():
            arguments.append(str(field.get_example()))
        return ", ".join(arguments)

    def get_example_update_arguments(self) -> str:
        """
        Get the example update arguments for the class.

        Returns:
            str: The example update arguments.
        """
        arguments = []
        for field in self.get_updatable_fields():
            if field.get_example() != "nullptr" and not field.has_parent():
                tuple_arg = f'{{"{field.to_camel_case()}", {field.get_example()}}}'
                arguments.append(tuple_arg)
        argument_str = ", ".join(arguments)

        return f"{{ {argument_str} }}"

    def get_ordered_fields(self):
        """
        Get the ordered fields for the class.

        Returns:
            List[Field]: The ordered fields.
        """
        # Returns a list of fields with the parent fields first, then other ordered alphabetically by name
        parent_fields = []
        other_fields = []
        for field in self.fields:
            if field.has_parent():
                parent_fields.append(field)
            else:
                other_fields.append(field)
        return sorted(parent_fields, key=lambda x: x.name) + sorted(
            other_fields, key=lambda x: x.name
        )

    def get_struct_fields(self):
        """
        Get the struct fields when using INDEXED_POOL export style

        Returns:
            List[Field]: The struct fields.
        """
        struct_fields = []
        for field in self.fields:
            ## Ignore child references
            if field.is_child and field.is_reference():
                continue

            struct_fields.append(field)
        return struct_fields

    def get_property_fields(self):
        """
        Fields to expose as a to_properties() row (INDEXED_POOLS only) -
        get_struct_fields() minus parent back-references, which are
        structural (an Id into another pool), not user-facing data.
        """
        return [f for f in self.get_struct_fields() if not f.has_parent()]

    def get_filterable_scalar_fields(self):
        """
        Leaf/scalar fields usable directly in a filter-expression
        comparison (INDEXED_POOLS only) - see get_filterable_hop_fields()
        for the traversal counterpart. get_struct_fields() minus parent
        back-references (structural, not user-facing data - same
        exclusion get_property_fields() already makes) and minus
        anything list or a reference to another (non-enum) klass, since
        those are hops rather than leaves. An enum field stays a leaf -
        it's a small closed set, compared as a string, same as
        to_properties() already treats it via wrap_with_to_property().
        """
        return [
            f
            for f in self.get_struct_fields()
            if not f.is_list
            and not f.has_parent()
            and (not f.is_reference() or f._type_klass.is_enum)
        ]

    def get_filterable_hop_fields(self):
        """
        Fields that are a traversal hop to another class, for
        filter-expression path resolution (INDEXED_POOLS only) - see
        get_filterable_scalar_fields() for the leaf counterpart. Includes
        is_child fields, which are never struct fields themselves (see
        get_struct_fields() - reachable only through Root's own
        child-list/child accessor) alongside ordinary reference struct
        fields, so this uses the full field list rather than
        get_struct_fields().
        """
        return [
            f
            for f in self.get_ordered_fields()
            if f.is_reference() and not f._type_klass.is_enum
        ]

    def get_updatable_fields(self):
        """
        Get the updatable fields for the class.

        Returns:
            List[Field]: The updatable fields.
        """
        return [field for field in self.get_ordered_fields() if not field.is_list]

    def link(self) -> None:
        """
        Link the class to its fields and schema.
        """
        if not self._schema:
            raise ValueError("Schema is not linked")
        for field in self.fields:
            field._klass = self
            if field.is_reference():
                field._type_klass = self._schema.get_klass(field.type)

            if field.has_parent():
                field._parent_klass = self._schema.get_klass(field.type)
                field._parent_field = self._schema.get_field(field.type, field.parent)  # type: ignore
            elif field.is_child:
                # Look for opposite field in child klass that has a matching parent
                child_klass = self._schema.get_klass(field.type)
                for child_field in child_klass.fields:
                    if child_field.has_parent() and child_field.type == self.name:
                        field._child_klass = child_klass
                        field._child_field = child_field
                        break

    def get_var_name(self) -> str:
        """
        Get the variable name for the class.

        Returns:
            str: The variable name.
        """
        # Return name with first letter lowercase
        return self.name[0].lower() + self.name[1:]

    def has_parent(self) -> bool:
        """
        Check if the class has a parent.

        Returns:
            bool: True if the class has a parent, False otherwise.
        """
        for field in self.fields:
            if field.has_parent():
                return True
        return False

    def get_create_ptr_type(self) -> str:
        """
        Get the create pointer type for the class.

        Returns:
            str: The create pointer type.
        """
        if self.has_parent():
            return f"std::weak_ptr<{self.name}>"
        else:
            return f"std::shared_ptr<{self.name}>"

    def get_parent_field(self) -> "Field":
        """
        Get the parent field for the class.

        Returns:
            Field: The parent field.

        Raises:
            ValueError: If no parent field is found.
        """
        for field in self.fields:
            if field.has_parent():
                return field
        raise ValueError("No parent field found")

    def is_root(self) -> bool:
        """
        Check if the class is the root klass.

        Returns:
            bool: True if the class is the root klass, False otherwise.
        """
        return not self.has_parent()

    def has_indecies(self) -> bool:
        for field in self.fields:
            if field.parent:
                return True
            if field.index:
                return True
        return False


@dataclass
class Field:
    """
    A field used to define the structure of the data in a model.

    Attributes:
        name (str): The name of the field.

        description (str): The description of the field.

        type (str): The type of the field.

        example (Any): The example value of the field.

        default (Optional[Any]): The default value of the field.

        parent (Optional[str]): The parent of the field.

        is_child (bool): Whether the field is a child.

        is_list (bool): Whether the field is a list.

        is_optional (bool): Whether the field is optional.

        index (bool): Create an index for this field.

        value (int): Optional enum value.
    """

    name: str
    description: str
    type: str
    example: Optional[Any] = None
    default: Optional[Any] = None
    parent: Optional[str] = None
    is_child: bool = False
    is_list: bool = False
    is_optional: bool = False
    index: bool = False
    value: Optional[int] = None
    _parent_klass: Optional[Klass] = field(default=None, repr=False, init=False)
    _parent_field: Optional["Field"] = field(default=None, repr=False, init=False)
    _child_klass: Optional[Klass] = field(default=None, repr=False, init=False)
    _child_field: Optional["Field"] = field(default=None, repr=False, init=False)
    _klass: Optional[Klass] = field(default=None, repr=False, init=False)
    _type_klass: Optional[Klass] = field(default=None, repr=False, init=False)

    def has_parent(self) -> bool:
        """
        Check if the field has a parent.

        Returns:
            bool: True if the field has a parent, False otherwise.
        """
        return self.parent is not None

    def has_default(self) -> bool:
        """
        Check if the field has a default value.

        Returns:
            bool: True if the field has a default value, False otherwise.
        """
        return self.default is not None

    def to_camel_case(self, upper_first=False) -> str | None:
        """
        Convert the field name to camelCase.

        Args:
            upper_first (bool): Whether to capitalize the first letter.

        Returns:
            str: The field name in camelCase.
        """
        return to_camel_case(self.name, upper_first)

    def get_cpp_type(
        self,
        nolist=False,
        cast=False,
    ) -> str:
        """
        Get the C++ type for the field.

        Args:
            nolist (bool): Whether to exclude list types.
            cast (bool): Whether to cast the type.

        Returns:
            str: The C++ type.
        """
        # Handle parent fields
        if self.has_parent():
            return f"{self.type}Id"

        # Handle child fields
        if self.is_reference():
            if self.is_list and not nolist:
                return f"std::vector<{self.type}>"
            if (
                not self.is_child
                and self._type_klass
                and not self._type_klass.is_enum
                and self._type_klass.has_pool
            ):
                return f"{self.type}Id"

        # Handle other (primitive) fields
        if self.type in TYPEMAP:
            type = TYPEMAP[self.type][0]
        else:
            type = self.type

        # A list of a primitive type (e.g. a plain list of str) - the
        # is_child/is_reference branches above already handle is_list for
        # fields that reference another Klass; this covers the remaining
        # case. Takes priority over is_optional (same convention as those
        # branches, which return their vector type without ever consulting
        # is_optional) - an empty vector already conveys "no items".
        if self.is_list and not nolist:
            return f"std::vector<{type}>"

        # Handle optional fields
        if self.is_optional:
            type = f"std::optional<{type}>"

        return type

    def is_reference(self) -> bool:
        """
        Check if the field is a reference.

        Returns:
            bool: True if the field is a reference, False otherwise.

        Raises:
            ValueError: If the class or schema is not linked.
        """
        if self._klass is None:
            raise ValueError("Klass is not linked")

        if self._klass._schema is None:
            raise ValueError("Schema is not linked")

        return self.type in [klass.name for klass in self._klass._schema.classes]

    def get_example(self) -> Any:
        """
        Get the example value for the field.

        Returns:
            Any: The example value.
        """
        example = self.example
        if self._parent_field is not None:
            example = self.to_camel_case()
        elif isinstance(example, str):
            example = f'std::string("{example}")'
        elif isinstance(example, list):
            example = "{}"
        elif example is None:
            example = "nullptr"
        elif isinstance(example, bool):
            example = str(example).lower()
        elif isinstance(example, float):
            example = f"{example}f"
        return example

    def _optional_default_cpp(self) -> str:
        """
        Value-initialized default ("<bare-type>{}") for an is_optional
        field's unset case - 0/0.0 for numeric types, "" for std::string,
        a zero-initialized aggregate for a reference to another (always
        non-pooled, plain-struct) Klass like Point/Foreign. Same bare-type
        resolution get_cpp_type() itself uses before wrapping in
        std::optional<...>, kept in sync with it deliberately (not calling
        get_cpp_type() directly, since that already applies the
        std::optional<> wrapper this method exists to unwrap).
        """
        return f"{TYPEMAP[self.type][0] if self.type in TYPEMAP else self.type}{{}}"

    def wrap_with_to_string(self, code, namespace) -> str:
        if self.is_list:
            return f"std::to_string({code}.size())"

        if self.is_optional:
            # .value_or(...), not .value() - an unset optional field (LEF
            # marks plenty of fields genuinely optional, e.g. Abstract's
            # own POWER statement) must never throw
            # std::bad_optional_access just because to_string()/
            # to_properties() below got called on it - matches this
            # project's "no exceptions for expected-missing-data paths"
            # convention (CLAUDE.md), not merely a style preference here.
            code = f"{code}.value_or({self._optional_default_cpp()})"

        if self.is_reference():
            return f"{namespace}::to_string({code})"

        if self.type == "str":
            return code

        return f"std::to_string({code})"

    def _references_embedded_klass(self) -> bool:
        """
        True only for a reference field whose target has its own
        to_property_string() overload - a plain embedded (non-pooled)
        struct like Point/Rect/Polygon. False for an enum (only
        to_string() is generated for those) and false for a reference to
        a pooled Klass (INDEXED_POOLS export represents those as a bare
        XxxId - e.g. Instance.reference_design - which likewise only has
        a generic to_string() overload in ids.hpp, not a per-class
        to_property_string()).
        """
        return (
            self._type_klass is not None
            and not self._type_klass.is_enum
            and not self._type_klass.has_pool
        )

    def wrap_with_to_property_string(self, code, namespace, dbu_var="dbu_per_um") -> str:
        """
        Fully-expanded string form of this field, used by
        to_property_string() - the get_properties()-facing analog of
        wrap_with_to_string(), which deliberately collapses list fields
        (and, transitively, anything nested inside them) to a bare count
        for compact debug logging. This one recurses all the way down
        instead, so e.g. Shape.polygons's own Polygon.points list - and
        Shape.paths's Path.polygon.points, one level deeper still -
        actually appear, not just their counts. An enum-typed reference,
        or a reference to a pooled Klass (stored as a bare Id, e.g.
        Instance.reference_design), is the exception: neither has a
        to_property_string() overload (see _references_embedded_klass()),
        so those keep going through to_string(), same as
        wrap_with_to_string() already does for them.

        A `dbu`-typed scalar converts to microns via `dbu_var` (in scope
        at every to_property_string()/to_property_list_string() call site
        - see struct_hpp_j2.py) and formats with format_coordinate_um(),
        instead of the raw integer std::to_string() would otherwise give -
        this is the fix for Shape's rects/polygons/paths having been the
        only clean, unit-converted display anywhere (previously hand-
        patched in api.cpp; every `dbu` field anywhere now converts the
        same way, recursively, for free).

        A scalar reference to an embedded klass is wrapped in its own
        "{...}" here - to_property_string() itself returns bare,
        unbracketed content (its own fields space-joined, no enclosing
        brace of its own), so every *splice* site (this one, a scalar
        reference field nested in a parent struct; and
        wrap_with_to_display_property()'s own matching branch, a
        top-level property-table row) is responsible for adding exactly
        one wrapping brace pair around the nested value it embeds - the
        same responsibility to_property_list_string() already carries
        per list item. This reproduces api.cpp's old hand-written
        format_rect_um()'s exact "{{llx lly} {urx ury}}" shape
        generically: Point has no embedded-klass fields of its own so it
        stays bare ("llx lly"), Rect's own "ll"/"ur" fields each wrap
        their Point in one brace pair, and whatever embeds a whole Rect
        (a list, or a scalar field like Abstract.bbox) adds the outer
        pair.
        """
        if self.is_list:
            if self.is_reference() and self._references_embedded_klass():
                return f"{namespace}::to_property_list_string({code}, {dbu_var})"
            return f"std::to_string({code}.size())"

        if self.is_optional:
            code = f"{code}.value_or({self._optional_default_cpp()})"

        if self.is_reference():
            if self._references_embedded_klass():
                return f'"{{" + {namespace}::to_property_string({code}, {dbu_var}) + "}}"'
            return f"{namespace}::to_string({code})"

        if self.type == "str":
            return code

        if self.type == "dbu":
            return f"{namespace}::format_coordinate_um({namespace}::to_um({code}, {dbu_var}))"

        return f"std::to_string({code})"

    def wrap_with_to_property(self, code, namespace) -> str:
        """
        PropertyValue::make_*(name, ...) call for this field - the
        property-table analog of wrap_with_to_string. A list of a
        non-pooled Klass (Rect/Polygon/Path/etc. - get_property_fields()
        already excludes is_child+is_reference fields like
        Abstract.terminals as structural, not user-facing data, so any
        is_list+is_reference field reaching here is always an embedded
        value list) reports its full contents, one brace-delimited
        Tcl-list item per element via that element's own
        to_property_string() (the fully-recursive sibling of to_string()
        - see wrap_with_to_property_string()), rather than just
        "<name>_count" - a bare count was confusing once the real data
        (e.g. every Shape.rects coordinate) was one property lookup away
        anyway. A list of a plain scalar (int/str/double/...) still
        reports only "<name>_count", numeric kind (INT vs DOUBLE)
        preserved rather than collapsed to a string. A scalar
        (non-list) embedded-struct reference field is expanded the same
        fully-recursive way (e.g. Path.polygon's own points list, not
        just its count) - an enum, or a reference to a pooled Klass
        (e.g. Instance.reference_design, stored as a bare Id), is the
        exception, via to_string() same as before - see
        _references_embedded_klass().

        Deliberately leaves `dbu` fields as a raw PropertyValue::INT (not
        micron-converted) - this is also the method backing get_field(),
        the filter-expression leaf lookup (struct_hpp_j2.py), which has
        no Root/dbu_per_um context and whose numeric comparisons
        (`-filter "width > 1000"`) are defined in raw database units. Use
        wrap_with_to_display_property() instead for anything actually
        shown to a user - see its own docstring.
        """
        # Note: get_filterable_scalar_fields() (the only real caller, via
        # get_field()) excludes every reference-to-non-enum-klass field
        # (see its own docstring), so the is_list/is_reference-embedded
        # branches below are unreachable in practice - kept correct
        # (with a literal dbu_per_um=1.0, since no such variable is ever
        # in scope in get_field()) as defense-in-depth rather than left
        # subtly broken if that filter ever changes.
        name = self.name
        if self.is_list:
            if self.is_reference() and self._references_embedded_klass():
                return f'{namespace}::PropertyValue::make_string("{name}", {namespace}::to_property_list_string({code}, 1.0))'
            return f'{namespace}::PropertyValue::make_int("{name}_count", static_cast<int64_t>({code}.size()))'

        # .value_or(...), not .value() - see wrap_with_to_string's own
        # comment for why an unset optional field must degrade to a
        # default instead of throwing.
        value = (
            f"{code}.value_or({self._optional_default_cpp()})"
            if self.is_optional
            else code
        )

        if self.is_reference():
            if not self._references_embedded_klass():
                return f'{namespace}::PropertyValue::make_string("{name}", {namespace}::to_string({value}))'
            return f'{namespace}::PropertyValue::make_string("{name}", "{{" + {namespace}::to_property_string({value}, 1.0) + "}}")'
        if self.type == "str":
            return f'{namespace}::PropertyValue::make_string("{name}", {value})'
        if self.type in ("float", "double", "long double"):
            return f'{namespace}::PropertyValue::make_double("{name}", static_cast<double>({value}))'
        return f'{namespace}::PropertyValue::make_int("{name}", static_cast<int64_t>({value}))'  # int/dbu/bool/etc.

    def wrap_with_to_display_property(self, code, namespace, dbu_var="dbu_per_um") -> str:
        """
        The to_properties()-facing sibling of wrap_with_to_property(),
        used for every user-visible property table (get_properties/
        report_properties, the Property Viewer) - identical except a
        `dbu` scalar becomes a micron-formatted STRING
        (format_coordinate_um(to_um(value, dbu_var))) instead of a raw
        PropertyValue::INT, and every recursive to_property_string()/
        to_property_list_string() call threads `dbu_var` through so a
        `dbu` field nested inside an embedded struct (e.g. Point.x via
        Rect.ll) converts too. This is the fix for every non-Shape type's
        embedded Rect/Point/Polygon/Path fields having shown raw-dbu,
        debug-style `Rect{ll=Point{x=...}}` text while Shape's own
        rects/polygons/paths were hand-patched (in api.cpp) to show
        clean microns - see backend/src/api/api.cpp's now-deleted
        replace_shape_geometry_properties for the override this
        generalizes and replaces.
        """
        name = self.name
        if self.is_list:
            if self.is_reference() and self._references_embedded_klass():
                return f'{namespace}::PropertyValue::make_string("{name}", {namespace}::to_property_list_string({code}, {dbu_var}))'
            return f'{namespace}::PropertyValue::make_int("{name}_count", static_cast<int64_t>({code}.size()))'

        value = (
            f"{code}.value_or({self._optional_default_cpp()})"
            if self.is_optional
            else code
        )

        if self.is_reference():
            if not self._references_embedded_klass():
                return f'{namespace}::PropertyValue::make_string("{name}", {namespace}::to_string({value}))'
            return f'{namespace}::PropertyValue::make_string("{name}", "{{" + {namespace}::to_property_string({value}, {dbu_var}) + "}}")'
        if self.type == "str":
            return f'{namespace}::PropertyValue::make_string("{name}", {value})'
        if self.type == "dbu":
            return f'{namespace}::PropertyValue::make_string("{name}", {namespace}::format_coordinate_um({namespace}::to_um({value}, {dbu_var})))'
        if self.type in ("float", "double", "long double"):
            return f'{namespace}::PropertyValue::make_double("{name}", static_cast<double>({value}))'
        return f'{namespace}::PropertyValue::make_int("{name}", static_cast<int64_t>({value}))'  # int/bool/etc.


@dataclass
class EnumValue:
    name: str
    value: int
