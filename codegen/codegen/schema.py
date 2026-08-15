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
        backed by a real *global* Root::get_<klass>_by_<field>(value)
        lookup (index=True and not unique_per_parent - a unique_per_parent
        field is also index=True, but its generated Root lookup takes an
        extra parent-id parameter and only enforces/finds uniqueness
        within one parent's sibling group, e.g. Terminal's `name`, unique
        only per-Abstract - see Field.unique_per_parent's own docstring).
        Such a field is still the right one for glob-based
        name_expression search (tcl_friendly_id_field() covers that), but
        there's no *global* Root-level by-name lookup to generate a
        friendly-id resolve/format pair or a le_X_by_<field>/le_X_<field>
        accessor pair from - those stay hand-written for a
        unique_per_parent class (e.g. Terminal's own resolve_terminal_id),
        so this returns None for it instead.
        """
        f = self.tcl_friendly_id_field()
        return f if f is not None and f.index and not f.unique_per_parent else None

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

    def get_create_fields(self) -> List["Field"]:
        """
        Every field that gets its own flag on this class's generated
        create_<type> command (see Field.is_create_field()), in schema
        (declaration) order.
        """
        return [f for f in self.fields if f.is_create_field()]

    def get_unique_per_parent_fields(self) -> List["Field"]:
        """
        Every field on this class with unique_per_parent=True - used by
        create_<type> generation to know whether Root::create_<type>() is
        fallible (a sibling collision under the same parent) and needs an
        error message distinct from the ordinary "unknown parent" case.
        """
        return [f for f in self.fields if f.unique_per_parent]

    def create_api_params(self) -> str:
        """
        Full comma-joined parameter list (excluding the leading
        LeHandle*) for this class's generated le_create_<type> - one
        Le<Parent>Id per parent field (get_parent_fields(), usually one,
        more for a multi-parent class like Shape/ViaLayer/Foreign/
        LayerDensityEntry), then each create field's own value-slot
        parameter (plus a has_<field> companion for the optional numeric
        ones - see Field.create_needs_has_flag()).
        """
        parts = [f"Le{pf.type}Id {pf.name}_id" for pf in self.get_parent_fields()]
        for f in self.get_create_fields():
            if f.create_needs_has_flag():
                parts.append(f"int32_t has_{f.name}")
            parts.append(f.create_c_param_decl())
        return ", ".join(parts)

    def create_shim_params(self) -> str:
        """
        Same shape as create_api_params(), but each parent field is a
        `const char *` friendly-id token instead of its real Le<Parent>Id
        - this is the signature create_<type>_cmd (the SWIG/Tcl-facing
        shim) actually takes, resolving each token via
        resolve_<parent_klass>_id() in its own body before forwarding to
        le_create_<type> (mirrors create_terminal_port_cmd's existing
        `const char *terminal_id` precedent, not the older packed-longlong
        convention create_terminal_cmd/create_obstruction_cmd used before
        Root-level friendly-id resolution existed for every class - see
        the create_<type> generation round's own notes for why those two
        are being replaced, not extended).
        """
        parts = [f"const char *{pf.name}_id" for pf in self.get_parent_fields()]
        for f in self.get_create_fields():
            if f.create_needs_has_flag():
                parts.append(f"int32_t has_{f.name}")
            parts.append(f.create_c_param_decl())
        return ", ".join(parts)

    def create_shim_forward_args(self) -> str:
        """
        Comma-joined argument list forwarding a create_<type>_cmd call
        (create_shim_params()'s own parameter names) to le_create_<type>
        (create_api_params()) - each parent token resolved via
        resolve_<parent_klass>_id(), each create field forwarded via its
        own Field.create_forward_expr() (identity for numeric fields,
        empty-to-nullptr for an optional str/enum field).
        """
        parts = [f"resolve_{pf._parent_klass.to_snake_case()}_id({pf.name}_id)" for pf in self.get_parent_fields()]
        for f in self.get_create_fields():
            if f.create_needs_has_flag():
                parts.append(f"has_{f.name}")
            parts.append(f.create_forward_expr())
        return ", ".join(parts)

    def create_api_body(self) -> str:
        """
        The full statement-list body of le_create_<type>(LeHandle *handle,
        <create_api_params()>) - built as one Python string rather than
        deeply nested Jinja {%- if %} chains, since the per-field-type/
        per-optionality branching (required-null checks, enum parsing,
        the exactly-one-parent check for a multi-parent class, the shared
        dbu_per_um lookup) is easier to get right and keep readable as
        real Python control flow than as template logic. Mirrors the
        hand-written le_create_terminal_port/le_create_obstruction shape
        for the common case (one parent, no optional/enum/dbu fields) and
        le_create_terminal's own shape (str/enum fields, is_optional
        handling) for the richer ones - generated instead of duplicated
        per class.
        """
        indent = "        "
        lines: List[str] = []

        def add(text: str = "") -> None:
            lines.append(f"{indent}{text}" if text else "")

        snake = self.to_snake_case()
        parent_fields = self.get_parent_fields()
        create_fields = self.get_create_fields()
        enum_fields = [f for f in create_fields if f.is_enum_type()]
        dbu_fields = [f for f in create_fields if f.type == "dbu"]
        unique_fields = self.get_unique_per_parent_fields()

        add(f"const Le{self.name}Id invalid{{.index = UINT32_MAX, .generation = 0}};")
        add("if (!handle)")
        add("    return invalid;")
        for f in create_fields:
            if (f.type == "str" or f.is_enum_type()) and f.create_required():
                add(f"if (!{f.create_c_param_name()})")
                add("    return invalid;")
        add("std::lock_guard<std::mutex> lock(handle->mutex_);")

        if parent_fields:
            add()
            for pf in parent_fields:
                add(f"const le::{pf.type}Id {pf.name} = from_c({pf.name}_id);")
            if len(parent_fields) == 1:
                pf = parent_fields[0]
                add(f"if (!handle->root.get_{pf._parent_klass.to_snake_case()}({pf.name}))")
                add("    return invalid;")
            else:
                add("// Exactly one parent must resolve - a {} belongs to exactly one of these, never zero or several (see its own schema.py comment).".format(self.name))
                add("int32_t provided_parent_count = 0;")
                for pf in parent_fields:
                    add(f"if (handle->root.get_{pf._parent_klass.to_snake_case()}({pf.name}))")
                    add("    ++provided_parent_count;")
                add("if (provided_parent_count != 1)")
                add("    return invalid;")

        if enum_fields:
            add()
            for f in enum_fields:
                enum_snake = f._type_klass.to_snake_case()
                if f.create_required():
                    add(f"const std::optional<le::{f.type}> parsed_{f.name} = le::{enum_snake}_from_string({f.name});")
                    add(f"if (!parsed_{f.name})")
                    add("{")
                    add(f'    handle->messages.push_back(fmt::format("ERROR: le_create_{snake}: unrecognized {f.name} \'{{}}\'", {f.name}));')
                    add("    return invalid;")
                    add("}")
                else:
                    add(f"const std::optional<le::{f.type}> parsed_{f.name} = ({f.name} && {f.name}[0]) ? le::{enum_snake}_from_string({f.name}) : std::nullopt;")
                    add(f"if ({f.name} && {f.name}[0] && !parsed_{f.name})")
                    add("{")
                    add(f'    handle->messages.push_back(fmt::format("ERROR: le_create_{snake}: unrecognized {f.name} \'{{}}\'", {f.name}));')
                    add("    return invalid;")
                    add("}")

        if dbu_fields:
            add()
            add("const std::optional<double> dbu_per_um = database_units_microns(handle->root);")
            add("if (!dbu_per_um)")
            add("    return invalid;")

        add()
        add(f"const le::{self.name}Id created = handle->root.create_{snake}(le::{self.name}Data{{")
        for pf in parent_fields:
            add(f"    .{pf.name} = {pf.name},")
        for f in create_fields:
            add(f"    .{f.name} = {f.create_struct_init_expr()},")
        add("});")

        if unique_fields:
            field = unique_fields[0]
            add("if (!created.valid())")
            add("{")
            add(
                f'    handle->messages.push_back(fmt::format("ERROR: le_create_{snake}: a sibling {self.name} with this '
                f"{field.name} (\'{{}}\') already exists\", {field.create_c_param_name()}));"
            )
            add("    return invalid;")
            add("}")

        add("handle->root.bump_mutation_version();")
        add("return to_c(created);")

        return "\n".join(lines)

    def create_tcl_flag_defaults(self) -> str:
        """
        The `array set opts {...}` initializer body (space-joined
        `-flag {}` pairs) for this class's generated `create_<type>
        {args}` proc - one entry per parent field, then per create field,
        in create_shim_params() order.
        """
        parts = [f"-{pf.name} {{}}" for pf in self.get_parent_fields()]
        parts += [f"-{f.name} {{}}" for f in self.get_create_fields()]
        return " ".join(parts)

    def create_tcl_required_flags(self) -> str:
        """
        Space-joined `-flag` names the generated `create_<type> {args}`
        proc rejects as missing before calling create_<type>_cmd - each
        parent flag when there's only one (get_parent_fields() length > 1
        means "exactly one of these", a distinct check - see
        create_api_body()'s own parent-count validation, mirrored here),
        plus every create_required() create field.
        """
        parts = []
        if len(self.get_parent_fields()) == 1:
            parts.append(f"-{self.get_parent_fields()[0].name}")
        parts += [f"-{f.name}" for f in self.get_create_fields() if f.create_required()]
        return " ".join(parts)

    def create_tcl_call_args(self) -> str:
        """
        Space-joined Tcl argument list for calling create_<type>_cmd from
        this class's own create_<type> {args} proc, in create_shim_params()
        order - `$opts(-<parent_field>)` per parent token, then each
        create field's own Field.create_tcl_call_args().
        """
        parts = [f"$opts(-{pf.name})" for pf in self.get_parent_fields()]
        for f in self.get_create_fields():
            parts.extend(f.create_tcl_call_args())
        return " ".join(parts)

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

        unique_per_parent (bool): Requires index=True. Instead of a single
            flat index shared by every instance of this Klass, uniqueness
            (and the generated lookup) is scoped per sibling group sharing
            the same parent - e.g. Terminal.name is unique only within one
            Abstract, not globally, since real LEF libraries legitimately
            reuse pin names like VDD/IN0 across different Abstracts. The
            owning Klass must have exactly one parent field (get_parent_fields()
            == 1) - the scope is ambiguous otherwise. create_<klass> becomes
            fallible for such a Klass: it returns an invalid Id (rather than
            inserting) when a sibling under the same parent already has this
            field's value, mirroring this codebase's existing Id::valid()
            sentinel convention instead of introducing a new failure signal.

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
    unique_per_parent: bool = False
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

    def is_enum_type(self) -> bool:
        """
        Whether this field's type is an enum Klass (RoutingDirection,
        SignalDirection, Orientation, ...) rather than a primitive
        (str/int/double/dbu/bool) or an embedded (has_pool=False) struct
        like Point/Rect/Symmetry.
        """
        return self._type_klass is not None and self._type_klass.is_enum

    def is_create_field(self) -> bool:
        """
        Whether this field gets its own flag on the generated
        create_<type> TCL/API command - a scalar leaf value (str/int/
        double/dbu/bool/enum), not a parent reference, not a child
        relationship (is_child, set via a future add_X/set_X round, not
        at creation), not a plain list (same reasoning - an empty vector
        already conveys "no items" at creation, appended to afterward),
        and not an embedded-struct-typed field (Point/Rect/Symmetry/...) -
        those don't have a single scalar flag representation and are also
        deferred to a future add_X/set_X round.
        """
        if self.has_parent() or self.is_child or self.is_list:
            return False
        return self._type_klass is None or self._type_klass.is_enum

    def create_required(self) -> bool:
        """
        Whether create_<type> makes this an unconditionally-required flag.
        Mirrors is_optional, with one deliberate exception: `bool` fields
        are never is_optional=True anywhere in this schema (false is
        already a zero-cost "not specified" default - see is_optional's
        own convention, established across the whole is_optional audit),
        so treating a bool create-flag as "required" would force every
        caller to spell out every boolean flag on every create call for
        no real benefit - always optional here too, regardless of the
        field's own is_optional value (which is always False for a bool).
        """
        if self.type == "bool":
            return False
        return not self.is_optional

    def create_needs_has_flag(self) -> bool:
        """
        Whether this optional create-flag needs a companion `has_<field>`
        int32_t parameter to distinguish "omitted" from "explicitly set to
        the type's zero value" (0/0.0) - true nullopt, not a zero-value
        default (see Field.unique_per_parent-adjacent design note in
        Phase 3/5 planning). Only numeric types need this: str/enum
        already have an unambiguous "omitted" signal at the C layer
        (nullptr, distinct from a real, even empty, string/enum name) that
        a companion flag would be redundant with.
        """
        return self.create_required() is False and self.type in ("int", "double", "dbu")

    def create_c_param_name(self) -> str:
        """
        The C-level parameter name for this create-field's value slot -
        `<name>_um` for a dbu field (its value crosses the C API in
        microns, converted via database_units_microns()/to_dbu(), same
        convention as le_add_shape_rect's own ll_x_um/.../ur_y_um), the
        bare field name for everything else.
        """
        return f"{self.name}_um" if self.type == "dbu" else self.name

    def create_c_type(self) -> str:
        """
        The C type of this create-field's own value slot (not counting
        any companion has_<field> parameter - see create_needs_has_flag).
        str and enum fields both cross the C API as `const char *` -
        enum-typed flags take the same spelling to_string()/from_string()
        use (e.g. "INPUT"), parsed via the matching generated
        <enum>_from_string() inside the generated create_<type> body,
        rather than a raw numeric code a caller would have to already
        know the encoding of.
        """
        if self.type == "str" or self.is_enum_type():
            return "const char *"
        if self.type == "dbu":
            return "double"
        if self.type == "bool":
            return "int32_t"
        if self.type == "int":
            return "int32_t"
        if self.type == "double":
            return "double"
        raise ValueError(f"create_c_type: unsupported create-field type {self.type!r} on field {self.name!r}")

    def create_c_param_decl(self) -> str:
        """
        The full "<type> <name>" declaration fragment for this create-
        field's own value-slot parameter (e.g. "const char *shape",
        "double width_um") - identical text used in le_create_<type>'s
        declaration (api.hpp) and definition (api.cpp) and the matching
        create_<type>_cmd shim declaration/definition, so those four
        always stay in sync by construction rather than by hand-copying.
        """
        ctype = self.create_c_type()
        name = self.create_c_param_name()
        return f"{ctype}{name}" if ctype.endswith("*") else f"{ctype} {name}"

    def create_forward_expr(self) -> str:
        """
        The expression forwarding this create-field's own value from a
        create_<type>_cmd shim call to le_create_<type> - identical to the
        raw parameter for a numeric field, but converts an empty/null
        string to a real nullptr for an optional str/enum field. Tcl/SWIG
        can't produce a null `const char *` argument directly (a Tcl
        string is never itself "null", only empty), so "empty means
        omitted" has to be applied here, at the first point value is
        actually plain C++ - matches the same "empty flag value means
        omitted" convention this codebase's hand-written -flag parsing
        already uses everywhere else (e.g. create_terminal_port's own
        `if {$opts(-terminal) eq {}}` check).
        """
        name = self.create_c_param_name()
        if (self.type == "str" or self.is_enum_type()) and not self.create_required():
            return f"({name} && {name}[0]) ? {name} : nullptr"
        return name

    def create_struct_init_expr(self) -> str:
        """
        The `.field = <expr>` initializer value for this create-field
        inside le_create_<type>'s own `le::<Klass>Data{...}` construction.
        Assumes (built by Klass.create_api_body(), which emits them in
        this order): a local `parsed_<name>` variable already exists for
        an enum field (declared/validated earlier in the function body),
        and `dbu_per_um` already exists for a dbu field.
        """
        name = self.create_c_param_name()
        if self.is_enum_type():
            return f"*parsed_{self.name}" if self.create_required() else f"parsed_{self.name}"
        if self.type == "str":
            if self.create_required():
                return name
            return f"{name} ? std::optional<std::string>({name}) : std::nullopt"
        if self.type == "bool":
            return f"{name} != 0"
        if self.type == "dbu":
            expr = f"to_dbu({name}, *dbu_per_um)"
            return expr if self.create_required() else f"has_{self.name} ? std::optional<int64_t>({expr}) : std::nullopt"
        if self.type in ("int", "double"):
            cpp_type = "int" if self.type == "int" else "double"
            return name if self.create_required() else f"has_{self.name} ? std::optional<{cpp_type}>({name}) : std::nullopt"
        raise ValueError(f"create_struct_init_expr: unsupported create-field type {self.type!r} on field {self.name!r}")

    def create_tcl_call_args(self) -> List[str]:
        """
        The Tcl expression(s) passing this create-field's own value from a
        generated `create_<type> {args}` proc's own `opts` array to
        create_<type>_cmd - one expression for most fields (Tcl doesn't
        distinguish omitted from empty any more precisely than the shim
        layer already does for str/enum), two (has_<field>, value-or-0)
        for the has-flag numeric case (create_needs_has_flag()) so an
        omitted flag can't reach SWIG's own int/double typemap as an
        unparsable empty string, one boolean-coerced expression for a
        `bool` field (always optional here - see create_required()).
        """
        opt = f"$opts(-{self.name})"
        if self.create_needs_has_flag():
            return [f"[expr {{{opt} ne {{}} ? 1 : 0}}]", f"[expr {{{opt} ne {{}} ? {opt} : 0}}]"]
        if self.type == "bool":
            return [f"[expr {{{opt} ne {{}} && {opt}}}]"]
        return [opt]

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
