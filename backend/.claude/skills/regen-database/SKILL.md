---
name: regen-database
description: Regenerate src/database/generated/ from src/database/schema.py using the cmg code generator. Use whenever schema.py changes, or when generated/ looks out of sync with the schema (missing class, stale field, stale test).
user-invocable: true
allowed-tools:
  - Bash
  - Read
---

# Regenerate the database from schema.py

`src/database/schema.py` is the only hand-edited source for everything under
`src/database/generated/`. Never edit files in `generated/` directly — re-run
codegen instead.

## Steps

1. **Bump the schema version.** Every change to a class or field requires
   incrementing `version=` in the `Schema(...)` call at the top of
   `src/database/schema.py` — cmg embeds this so old serialized databases are
   rejected instead of silently misread.

2. **Ensure `cmg` is available.** Check first:

   ```
   python3 -c "import cmg; print(cmg.__version__)"
   ```

   If missing, install the version this project was generated with (1.2.1):

   ```
   pip install cmg==1.2.1
   ```

   A local checkout also exists at `/Users/john/Projects/synthosilicon/cmg`
   (poetry project, same version) if you need to develop against an unreleased
   cmg change — run it there with `poetry run cmg ...` instead of installing.

3. **Run the generator with the INDEXED_POOLS export style** — this repo's
   generated code is *not* in cmg's default `SMART_POINTERS` style:

   ```
   cmg --schema src/database/schema.py --output src/database/generated --export-style INDEXED_POOLS
   ```

   Confirm the flag name/value with `cmg --help` if generation fails — verify
   against the installed cmg version rather than assuming this is current.

4. **Diff the output.** `cmg` deletes and fully recreates the output
   directory on every run. Check `git diff --stat src/database/generated`
   for unexpected churn (e.g. accidental deletions if `--output` was wrong)
   before trusting the result.

5. **Rebuild and run tests** once a build exists (see the `build-test`
   skill) to confirm the regenerated code still compiles and passes.
