---
name: regen-tcl
description: Regenerate the TCL/SWIG property-reading and search surface (backend/src/api/generated_tcl/, backend/src/tcl/generated/) from src/database/schema.py using the local codegen fork's `tcl` target. Use whenever schema.py changes a TCL-readable class or a has_current_access flag, or when the generated TCL surface looks out of sync (missing class, stale field, stale friendly-id lookup, wrong get_<type> default scope).
user-invocable: true
allowed-tools:
  - Bash
  - Read
---

# Regenerate the TCL property-reading/search surface from schema.py

`src/database/schema.py` is also the source of truth for this surface -
`Klass.tcl_readable`/`Klass.tcl_id_field`/`Klass.has_current_access`
(codegen/codegen/schema.py) control which classes get a generated TCL
property table, how their friendly id is built, and how every readable
class's `get_<type>` default scope (`-of` omitted) is derived. Never edit
`generated_tcl/`/`tcl/generated/` directly - re-run codegen instead.

This is a **separate generation target** from `regen-database` - it
covers property *reading* and `get_<type>` *search* for every TCL-readable
class, uniformly (all 15 today - `Terminal`/`TerminalPort`/`Obstruction`/
`Shape` also have hand-written CRUD elsewhere - see
`codegen/codegen/tcl_generator.py`'s `HAND_WRITTEN_CRUD_CLASSES`, kept
purely as documentation, not an exclusion set). `read_lef`, session/
viewport/design-selection, Shape's rect/polygon/path CRUD + the
coordinate-list SWIG typemap, `update_abstract_boundary`,
`create_X`/`delete_X`/`set_X_<field>` for the CRUD classes, and the
filter-expression evaluator itself (`filter.hpp`) all stay hand-written -
none of that is per-class CRUD, so it doesn't belong in a generator.

## Steps

1. **Ensure the local `codegen` fork is installed**:

   ```
   cd /Volumes/Docking/Projects/synthosilicon/lef_editor_mvp/codegen
   poetry install
   ```

2. **Run the generator with `--target tcl`**, pointing `--output` at the
   backend's `src/` directory (not `src/database/generated` - this
   target writes to two different subdirectories beneath `--output`,
   `api/generated_tcl/` and `tcl/generated/`):

   ```
   poetry run cmg --schema /Volumes/Docking/Projects/synthosilicon/lef_editor_mvp/backend/src/database/schema.py \
                   --output /Volumes/Docking/Projects/synthosilicon/lef_editor_mvp/backend/src \
                   --target tcl
   ```

3. **Diff the output.** Both output directories are `.gitignore`d, so
   `git diff`/`git status` won't show anything - copy them aside before
   regenerating if you need a real diff baseline. The generator deletes
   and fully recreates both directories on every run, so a stale/orphaned
   file (e.g. from a since-renamed class) can't survive a run.

4. **Rebuild and run tests** (see the `build-test` skill) to confirm the
   regenerated code still compiles and passes - `le_tcl_smoke`,
   `le_tcl_crud`, and `le_tcl_shell` exercise this surface directly.

## Adding a new TCL-readable class

Every pool-backed class (`has_pool=True`) is TCL-readable by default
(`Klass.tcl_readable` defaults to `has_pool` - see
`codegen/codegen/schema.py`'s `Klass.is_tcl_readable()`), so a brand new
`Klass` in `schema.py` needs no extra step to show up here. To opt a
class *out*, pass `tcl_readable=False` to its `Klass(...)` call.

Its friendly id auto-derives to the field with `index=True` if one exists
(name-based, `"type:NAME"`), else a numeric packed id (`"type:N"`) - pass
`tcl_id_field="<field>"` to override which field backs the friendly id
(used for glob-based `name_expression` search). If that field is *not*
`index=True` (e.g. `Terminal`'s `name`, unique only per-Abstract, enforced
by hand rather than a real global `index=True`), `Klass.tcl_indexed_id_field()`
returns `None` for it even though `tcl_friendly_id_field()` still does -
the generator then skips the Root-backed by-name lookup pair
(`le_X_by_<field>`/`le_X_<field>_by_id`) and the friendly-id resolve/
format pair (`resolve_X_id`/`format_X_id`) for that class, since there's
no global index to build them from; a class in that situation needs its
own hand-written `resolve_X_id`/`format_X_id`/`le_X_by_<field>` (see
`Terminal`'s in `le_tcl_shim.cpp`/`api.cpp` for the pattern - current-
abstract-scoped name lookup, not a flat Root index).

## `has_current_access` and the `get_<type>` default-scope algorithm

`Klass.has_current_access = True` (today: `Technology`, `Abstract`,
`Schematic`) marks a class with a generated "current instance" concept -
each gets independent `LeHandle` state (`current_X_id`), a
`current_X`/`set_current_X_cmd` TCL command pair, and every *other*
readable class's `get_<type>` default scope (when every `-of` is omitted)
is *derived automatically* from where that class sits in the schema graph
relative to the nearest `has_current_access` anchor - no hand-picked
per-class table. See `codegen/codegen/tcl_scope.py`'s own module
docstring for the full 4-case algorithm (self / descendant-of-anchor,
unioning multiple `is_child` paths / ancestor-of-anchor, falling back to a
flat scan if unset / no relation, flat scan). `-of <parent>` parameters
are independent of this and always generated one-per-parent-field
(`Klass.get_parent_fields()`, matched against a *sibling* `is_child`
field - list or scalar, e.g. `Design.abstract` - on the parent class);
the default-scope case only supplies the fallback used when every `-of`
was omitted or invalid.

This generated `current_X` state is deliberately **independent** of any
other "current view" concept in the codebase (e.g. `Scene::current_abstract()`,
which drives GUI rendering) - a TCL script must call `set_current_abstract`
itself (or rely on a convenience caller like `open_design`, which does
this for the script - see `le_tcl_procs.tcl`) before `get_terminals`/
`get_shapes`/etc.'s default scope will resolve to anything.

## The nine generated-code injection points

Each of these hand-written files gains one or more `#include`/`%include`/
`source` lines pointing at generated output - added once, never touched
again on subsequent regenerations:

- `api.hpp` - **two** injection points: `#include "generated_tcl/ids.inc"`
  immediately after `typedef struct LeHandle LeHandle;` (every `LeXId`
  typedef - has to come before anything else in the file, hand-written or
  generated, that names one of these types), and
  `#include "generated_tcl/declarations.inc"` further down (friendly-id-
  by-name lookups, property-table declarations, `is_child` enumeration,
  current-instance access, `get_<type>` search declarations).
- `api.cpp` - **four** injection points: `#include "generated_tcl/handle_fields.inc"`
  inside `struct LeHandle`'s body (per-class property-table caches,
  search-result caches, `current_X_id` fields); `#include "generated_tcl/property_accessors_internal.inc"`
  *inside* the file's anonymous namespace (internal helpers -
  `build_X_properties`, `to_c`/`from_c` overloads - never called from
  another translation unit); `#include "generated_tcl/property_accessors_public.inc"`
  *inside* `extern "C" { ... }` (the real `le_X_property_count/_at/_path`,
  friendly-id-by-name lookups, `current_X`/`set_current_X` - external C
  linkage required since `le_tcl_shim.cpp` calls them; a function defined
  inside an anonymous namespace has internal linkage regardless of
  `extern "C"`, so putting these there produces unresolved-symbol link
  errors in `le_tcl.so` - don't merge the internal/public fragments back
  into one); `#include "generated_tcl/search.inc"` right after it, also
  inside `extern "C" { ... }` (`le_get_X`/`le_search_result_X_at`, same
  linkage reasoning). `filter_field_tables()`'s body is also generated -
  `= \n#include "generated_tcl/filter_tables.inc"` replaces its old
  hand-written initializer list (the `FilterFieldTable` struct itself and
  the functions that consume the table stay hand-written).
- `le_tcl_shim.hpp` - `#include "generated/le_tcl_shim_generated.hpp"`.
- `le_tcl_shim.cpp` - `#include "generated/le_tcl_shim_generated.inc"`,
  placed right after the file's own anonymous namespace closes (so
  `session()`/`pack`/`unpack`/`return_string`/`format_property_value`/
  `resolve_numeric_friendly_id`/`format_numeric_friendly_id` are already
  in scope).
- `le_api.i` - `%include "generated/le_api_generated.i"`.
- `le_tcl_procs.tcl` - `source [file join [file dirname [info script]] generated le_tcl_procs_generated.tcl]`
  (`property_accessors_for_token`, `set_current_X`, every `get_<type>`
  proc - `parse_get_args`/`check_of_prefixes`/`default_to_unset` stay
  hand-written, shared/class-agnostic helpers the generated procs call
  into).

If a future round generates a class's CRUD surface too (shrinking
`HAND_WRITTEN_CRUD_CLASSES`), delete that class's hand-written CRUD from
all of the files above first, to avoid duplicate-symbol link errors -
property reading/search are already generated uniformly for every
readable class today, so no further deletion is needed on that front.
