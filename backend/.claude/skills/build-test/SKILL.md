---
name: build-test
description: Configure, build, and run tests for the backend C++ project. Use whenever the user asks to build, compile, or run tests for this repo.
user-invocable: true
allowed-tools:
  - Bash
  - Read
---

# Build and test the backend

1. **Check a `CMakeLists.txt` exists at the repo root.** As of this writing
   it doesn't yet (see CLAUDE.md "Open gaps") — that's a separate setup task,
   not something to improvise inside this skill. If it's missing, stop and
   tell the user the CMake project needs to be set up first.

2. **Configure and build:**

   ```
   cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
   cmake --build build -j
   ```

3. **Run tests via ctest** (GTest-based, per `generated/test_layout_engine.cpp`):

   ```
   ctest --test-dir build --output-on-failure
   ```

4. **Dependencies to expect on the build machine**, based on this project's
   lineage (`../../layout_engine/backend`): `spdlog`, `fmt`, a vendored
   `lefdef` LEF/DEF C parser under `lefdef/lef` and `lefdef/def` (built via
   `ExternalProject_Add` + `make`, not a system package), and GTest for the
   generated test suite. Confirm against the actual `CMakeLists.txt` once it
   exists rather than assuming this list is exhaustive.

5. Report build/test failures concisely — file:line and the actual error,
   not the full compiler log. Per this project's requirements, benchmark
   before changing anything for performance reasons rather than guessing.
