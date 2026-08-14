---
name: regen-tcl
description: Regenerate the TCL/SWIG property-reading surface (backend/src/api/generated_tcl/, backend/src/tcl/generated/) from src/database/schema.py using the local codegen fork's `tcl` target. Use whenever schema.py changes a TCL-readable class, or when the generated TCL surface looks out of sync (missing class, stale field, stale friendly-id lookup).
user-invocable: true
allowed-tools:
  - Bash
  - Read
---

# Regenerate the TCL property-reading surface from schema.py

`src/database/schema.py` is also the source of truth for this surface -
`Klass.tcl_readable`/`Klass.tcl_id_field` (codegen/codegen/schema.py)
control which classes get a generated TCL property table and how their
friendly id is built. Never edit `generated_tcl/`/`tcl/generated/`
directly — re-run codegen instead.

This is a **separate generation target** from `regen-database` — it
covers property *reading* only (property tables, friendly-id resolution,
`is_child` enumeration) for every TCL-readable class not already covered
by hand-written CRUD code (`Library`/`Design`/`Abstract`/`Terminal`/
`TerminalPort`/`Obstruction`/`Shape` — see
`codegen/codegen/tcl_generator.py`'s `HAND_WRITTEN_CLASSES`). `read_lef`,
session/viewport/design-selection, Shape's rect/polygon/path CRUD, the
coordinate-list SWIG typemap, `update_abstract_boundary`, and the
filter-expression allowlist (`filter.hpp`) all stay hand-written — none
of that is per-class CRUD, so it doesn't belong in a generator.

## Steps

1. **Ensure the local `codegen` fork is installed**:

   ```
   cd /Volumes/Docking/Projects/synthosilicon/lef_editor_mvp/codegen
   poetry install
   ```

2. **Run the generator with `--target tcl`**, pointing `--output` at the
   backend's `src/` directory (not `src/database/generated` — this
   target writes to two different subdirectories beneath `--output`,
   `api/generated_tcl/` and `tcl/generated/`):

   ```
   poetry run cmg --schema /Volumes/Docking/Projects/synthosilicon/lef_editor_mvp/backend/src/database/schema.py \
                   --output /Volumes/Docking/Projects/synthosilicon/lef_editor_mvp/backend/src \
                   --target tcl
   ```

3. **Diff the output.** Both output directories are `.gitignore`d, so
   `git diff`/`git status` won't show anything — copy them aside before
   regenerating if you need a real diff baseline. The generator deletes
   and fully recreates both directories on every run, so a stale/orphaned
   file (e.g. from a since-renamed class) can't survive a run.

4. **Rebuild and run tests** (see the `build-test` skill) to confirm the
   regenerated code still compiles and passes — `le_tcl_smoke`,
   `le_tcl_crud`, and `le_tcl_shell` exercise this surface directly.

## Adding a new TCL-readable class

Every pool-backed class (`has_pool=True`) is TCL-readable by default
(`Klass.tcl_readable` defaults to `has_pool` — see
`codegen/codegen/schema.py`'s `Klass.is_tcl_readable()`), so a brand new
`Klass` in `schema.py` needs no extra step to show up here. To opt a
class *out*, pass `tcl_readable=False` to its `Klass(...)` call. Its
friendly id auto-derives to the field with `index=True` if one exists
(name-based, `"type:NAME"`), else a numeric packed id (`"type:N"`) — pass
`tcl_id_field="<field>"` to override (e.g. `Terminal`'s own override,
since its name uniqueness is hand-enforced per-Abstract, not a real
`index=True`).

## The six generated-code injection points

Each of these hand-written files gains exactly **one** `#include`/
`%include`/`source` line pointing at generated output — added once, never
touched again on subsequent regenerations:

- `api.hpp` — `#include "generated_tcl/declarations.inc"` (Id typedefs,
  plain-C function declarations).
- `api.cpp` — two injection points: `#include "generated_tcl/handle_fields.inc"`
  inside `struct LeHandle`'s body (per-class property-table cache
  members), and two more further down for the generated functions
  themselves, split by required linkage - `#include "generated_tcl/property_accessors_internal.inc"`
  *inside* the file's anonymous namespace (internal helpers -
  `build_X_properties`, `to_c`/`from_c` overloads - never called from
  another translation unit) and `#include "generated_tcl/property_accessors_public.inc"`
  *inside* `extern "C" { ... }` (the real `le_X_property_count/_at/_path`
  etc. - these need external C linkage since `le_tcl_shim.cpp` calls
  them; a function defined inside an anonymous namespace has internal
  linkage regardless of `extern "C"`, so putting these there produces
  unresolved-symbol link errors in `le_tcl.so` - don't merge these two
  fragments back into one).
- `le_tcl_shim.hpp` — `#include "generated/le_tcl_shim_generated.hpp"`.
- `le_tcl_shim.cpp` — `#include "generated/le_tcl_shim_generated.inc"`,
  placed right after the file's own anonymous namespace closes (so
  `session()`/`pack`/`unpack`/`return_string`/`format_property_value`/
  `resolve_numeric_friendly_id`/`format_numeric_friendly_id` are already
  in scope).
- `le_api.i` — `%include "generated/le_api_generated.i"`.
- `le_tcl_procs.tcl` — `source [file join [file dirname [info script]] generated le_tcl_procs_generated.tcl]`,
  replacing the old hand-written `property_accessors_for_token`.

If a future round generates a class's CRUD surface too (removing it from
`HAND_WRITTEN_CLASSES`), delete that class's hand-written property
accessors from all of the files above first, to avoid duplicate-symbol
link errors.
