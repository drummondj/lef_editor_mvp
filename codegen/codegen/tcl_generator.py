"""Generates the TCL/SWIG property-reading surface (backend/src/api's
generated_tcl/ fragments, backend/src/tcl's generated/ files) from a
schema - a separate generation target from generator.py's database
codegen, invoked via `cmg --target tcl`.
"""

from logging import Logger
from pathlib import Path
import shutil

import jinja2

from codegen.templates.tcl import (
    api_declarations_inc_j2,
    api_handle_fields_inc_j2,
    api_property_accessors_internal_inc_j2,
    api_property_accessors_public_inc_j2,
    le_api_generated_i_j2,
    le_tcl_procs_generated_tcl_j2,
    le_tcl_shim_generated_hpp_j2,
    le_tcl_shim_generated_inc_j2,
)
from codegen.schema import Schema
from codegen.validation import SchemaRuleSet

# Classes with a hand-written TCL CRUD surface today (le_api.i,
# le_tcl_shim.hpp/.cpp, api.hpp/api.cpp) - the property-reading generator
# skips these entirely to avoid duplicate-symbol collisions with the
# hand-written le_X_property_count/_at/_path etc. these classes already
# have. Drop a class from this set (and delete its hand-written property
# accessors) if/when its CRUD surface itself gets generated in a later
# round - see the plan's own "write scope unchanged" decision for why
# that isn't done yet.
HAND_WRITTEN_CLASSES = {
    "Library",
    "Design",
    "Abstract",
    "Terminal",
    "TerminalPort",
    "Obstruction",
    "Shape",
}


def get_generated_classes(schema: Schema):
    """
    Every class the TCL generator owns: pool-backed, tcl_readable
    (defaults to has_pool - see Klass.is_tcl_readable()), and not already
    covered by hand-written code (HAND_WRITTEN_CLASSES above).
    """
    return [
        klass
        for klass in schema.classes
        if not klass.is_enum
        and klass.has_pool
        and klass.is_tcl_readable()
        and klass.name not in HAND_WRITTEN_CLASSES
    ]


def get_readable_classes(schema: Schema):
    """
    Every TCL-readable pool-backed class, hand-written or generated -
    used by le_tcl_procs_generated_tcl_j2's property_accessors_for_token,
    which dispatches by friendly-id prefix across the whole surface, not
    just the classes this generator itself owns.
    """
    return [
        klass
        for klass in schema.classes
        if not klass.is_enum and klass.has_pool and klass.is_tcl_readable()
    ]


def generate(schema: Schema, output_dir: str, logger: Logger) -> int:
    """Generate the TCL property-reading surface. `output_dir` is the
    backend's src/ directory - files land under api/generated_tcl/ and
    tcl/generated/ beneath it."""

    logger.info("Validating schema ...")
    errors = SchemaRuleSet().validate(schema)
    if len(errors) > 0:
        for error in errors:
            logger.error(error.message)
        return 1

    schema.link()

    classes = get_generated_classes(schema)
    readable_classes = get_readable_classes(schema)
    logger.info(
        f"Generating TCL property-reading surface for {len(classes)} classes: "
        f"{', '.join(k.name for k in classes)}"
    )

    api_dir = Path(output_dir) / "api" / "generated_tcl"
    tcl_dir = Path(output_dir) / "tcl" / "generated"
    shutil.rmtree(api_dir, ignore_errors=True)
    shutil.rmtree(tcl_dir, ignore_errors=True)
    api_dir.mkdir(parents=True, exist_ok=True)
    tcl_dir.mkdir(parents=True, exist_ok=True)

    files = [
        (api_dir / "declarations.inc", api_declarations_inc_j2.TEMPLATE),
        (api_dir / "handle_fields.inc", api_handle_fields_inc_j2.TEMPLATE),
        (api_dir / "property_accessors_internal.inc", api_property_accessors_internal_inc_j2.TEMPLATE),
        (api_dir / "property_accessors_public.inc", api_property_accessors_public_inc_j2.TEMPLATE),
        (tcl_dir / "le_tcl_shim_generated.hpp", le_tcl_shim_generated_hpp_j2.TEMPLATE),
        (tcl_dir / "le_tcl_shim_generated.inc", le_tcl_shim_generated_inc_j2.TEMPLATE),
        (tcl_dir / "le_api_generated.i", le_api_generated_i_j2.TEMPLATE),
        (tcl_dir / "le_tcl_procs_generated.tcl", le_tcl_procs_generated_tcl_j2.TEMPLATE),
    ]
    for path, template_str in files:
        content = jinja2.Template(template_str).render(
            schema=schema, classes=classes, readable_classes=readable_classes
        )
        with open(path, "w") as f:
            f.write(content)

    return 0
