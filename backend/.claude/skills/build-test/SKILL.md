---
name: build-test
description: Configure, build, and run tests for the backend C++ project. Use whenever the user asks to build, compile, or run tests for this repo.
user-invocable: true
allowed-tools:
  - Bash
  - Read
---

# Build and test the backend

1. **Configure and build:**

   ```
   cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
   cmake --build build -j
   ```

2. **Run tests via ctest:**

   ```
   ctest --test-dir build --output-on-failure
   ```

3. **Dependencies**: `spdlog`, `fmt`, `Boost` via `find_package` (installed
   via Homebrew on this dev machine); GoogleTest via CMake `FetchContent`
   (no system install needed); `src/lefdef/lef` (vendored LEF parser C
   source) built via `ExternalProject_Add` + its own `Makefile`.

4. **If `lef_lib` fails to build** with something like
   `ranlib: liblef.a is not writable` or `mv: lef.tab.c: No such file or
   directory`: that vendored Makefile's `install`/`release` targets race
   under a parallel jobserver. `CMakeLists.txt` already forces the
   `ExternalProject_Add(lef_lib ...)` step to run with
   `--unset=MAKEFLAGS make -j1` — if this regresses, check that flag hasn't
   been dropped rather than reaching for `-j1` on the whole outer build.

5. Report build/test failures concisely — file:line and the actual error,
   not the full compiler log. Per this project's requirements, benchmark
   before changing anything for performance reasons rather than guessing.

6. **Coverage** (line + branch, off by default): reconfigure with
   `-DENABLE_COVERAGE=ON`, then `cmake --build build --target coverage`.
   Prints a `llvm-cov report --show-branch-summary` table and writes
   `build/coverage/lcov.info`. Requires Clang + `llvm-profdata`/`llvm-cov`
   (auto-resolved via `xcrun` on macOS). See CLAUDE.md's "Coverage" section
   for the full explanation, including why `io` shows 0% until it has tests.
