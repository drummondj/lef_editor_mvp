# LEF Layout Editor MVP — Backend

C++23 backend that reads LEF/DEF and SystemVerilog EDA data into an in-memory
database, then renders it through a multi-threaded, layer-based pipeline into
Skia commands consumed by a Flutter plugin. This is an MVP/proof-of-concept:
the goal right now is finding the right architecture for editing hierarchical
designs with millions of objects, not shipping features. See `README.md` for
the full brief and the live plan checklist.

## Requirements (non-negotiable)

- Target: Linux servers, little/no GPU. Optimize for memory and CPU, not GPU.
- Tests are written alongside the code they cover, not after.
- Performance decisions must be backed by a benchmark, not intuition.
- C++23. Keep abstractions minimal and justified by present, not hypothetical, needs.
- Keep responses and docs concise — this repo's own README asks for that explicitly.

## Layout

- `src/database/` — the object-pool database. `schema.py` is the source of
  truth (a `cmg.Schema` of `Klass`/`Field` definitions); `generated/` is
  produced from it and must never be hand-edited (see Database codegen below).
  `database.hpp` is the single public include (`#include "generated/root.hpp"`).
- `src/io/` — format readers. Currently `lef_reader.{hpp,cpp}`, which drives
  the vendored `lefr*` LEF-parser C callbacks and populates `Root` via the
  generated create/get API.

## Database codegen (cmg)

Generated code follows the **INDEXED_POOLS** export style from
[cmg](https://github.com/johndru-astrophysics/cmg) — not cmg's default
(`SMART_POINTERS`). Every `Klass` in `schema.py` becomes:

- `XxxData` — a plain data struct.
- `XxxId` — a `{index, generation}` handle (see `generated/ids.hpp`), not a pointer.
- Storage in a `Pool<XxxData, XxxId>` (`generated/pool.hpp`) — a generational
  slot array, so erased objects can't alias a reused slot.
- `Root` (`generated/root.hpp`) owns every pool plus an `index_` for
  parent→children and lookup-by-field indices, and exposes
  `create_x`/`get_x`/`get_x_ids`/`for_each_x_id`/`clear_x`/`get_x_size` per class.

To change the schema: edit `src/database/schema.py`, bump `Schema.version`,
then regenerate with the `regen-database` skill rather than editing
`generated/` by hand.

`generated/test_le.cpp` is stale — it targets an older `SMART_POINTERS`-style
API (`shared_ptr`, `.lock()->getptr()`) that no longer matches the current
schema output. Don't use it as a reference; `generated/test_layout_engine.cpp`
is the current generated GTest suite.

## Open gaps (tracked in README's Plan checklist)

- No `CMakeLists.txt` yet. `lef_reader.hpp` expects `../lefdef/lef/include`
  and `../lefdef/def/include` (vendored LEF/DEF 6.0 C parser, not yet added
  to this repo) plus `spdlog`.
- `cmg` itself isn't installed in this environment — see the `regen-database`
  skill for setup.

## Conventions observed in existing code

- Everything lives in `namespace le`.
- Doxygen-style `/// @brief` one-liners on generated public methods — match
  this on hand-written public API.
- No exceptions for expected-missing-data paths — pool lookups return
  nullable pointers (`get(id)` → `T*`) or use `std::optional`/`std::expected`.

## Related prior art

`../../layout_engine/backend` (sibling repo, same author) is an earlier, more
complete implementation of the same idea — same `le` namespace, same
pool/schema database pattern, a working `CMakeLists.txt` wiring up the vendored
`lefdef` libs + spdlog + fmt, and a Taskflow/Boost.Geometry/Skia render
pipeline. This MVP deliberately restarts the pipeline/rendering architecture
decisions rather than importing that one — treat it as reference and lessons
learned (its `BACKEND_REVIEW.md` has a real bug/perf review worth skimming),
not as code to copy wholesale.

## Skills

- `regen-database` — regenerate `src/database/generated/` from `schema.py` via `cmg`.
- `build-test` — configure/build/test the CMake project once one exists.
