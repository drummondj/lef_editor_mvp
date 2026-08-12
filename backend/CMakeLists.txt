cmake_minimum_required(VERSION 3.20)
project(lef_editor_mvp_backend VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
cmake_policy(SET CMP0135 NEW)

option(ENABLE_COVERAGE "Instrument io/backend_tests with Clang source-based line+branch coverage" OFF)

if(ENABLE_COVERAGE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "ENABLE_COVERAGE uses -fprofile-instr-generate, which requires Clang; found ${CMAKE_CXX_COMPILER_ID}. On GCC/Linux use --coverage + gcov/lcov instead.")
    endif()

    find_program(LLVM_PROFDATA_EXE llvm-profdata)
    find_program(LLVM_COV_EXE llvm-cov)
    if(APPLE AND (NOT LLVM_PROFDATA_EXE OR NOT LLVM_COV_EXE))
        # Xcode's toolchain doesn't put these on PATH directly.
        execute_process(COMMAND xcrun -f llvm-profdata OUTPUT_VARIABLE LLVM_PROFDATA_EXE OUTPUT_STRIP_TRAILING_WHITESPACE)
        execute_process(COMMAND xcrun -f llvm-cov OUTPUT_VARIABLE LLVM_COV_EXE OUTPUT_STRIP_TRAILING_WHITESPACE)
    endif()
    if(NOT LLVM_PROFDATA_EXE OR NOT LLVM_COV_EXE)
        message(FATAL_ERROR "ENABLE_COVERAGE requires llvm-profdata and llvm-cov (on PATH, or via xcrun on macOS)")
    endif()
endif()

function(le_enable_coverage_instrumentation target)
    if(ENABLE_COVERAGE)
        target_compile_options(${target} PRIVATE -fprofile-instr-generate -fcoverage-mapping -g -O0)
        target_link_options(${target} PRIVATE -fprofile-instr-generate)
    endif()
endfunction()

include(ExternalProject)
include(FetchContent)

# --- Vendored LEF parser (src/lefdef/lef) ---
# Built via its own Makefile; produces liblef.a and generates lefdef/lef/include/*.h.
# The DEF parser (src/lefdef/def) is vendored alongside it but not built yet -
# wire it up here once a DEF reader module needs it.
ExternalProject_Add(
    lef_lib
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/lefdef/lef
    CONFIGURE_COMMAND ""
    # -j1 and a cleared MAKEFLAGS are required: this vendored recursive
    # Makefile's `all: install release` traverses the same subdirectories
    # twice and isn't safe under an inherited parallel jobserver (races on
    # shared bison-generated lef.tab.c and on liblef.a itself).
    BUILD_COMMAND ${CMAKE_COMMAND} -E env --unset=MAKEFLAGS --unset=MFLAGS make -j1
    INSTALL_COMMAND ""
    BUILD_IN_SOURCE 1
    # lefdiff (UPDATES.md 12 Phase 1's round-trip fidelity check) is also a
    # side effect of this same build - the vendored top-level Makefile's
    # own BUILD_ORDER already includes it (and lefwrite) alongside lef/
    # clef/lefrw, so no separate build step is needed, just tracking it as
    # a real byproduct.
    BYPRODUCTS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/lefdef/lef/lib/liblef.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/lefdef/lef/bin/lefdiff"
)

# --- Third-party dependencies ---
find_package(spdlog REQUIRED)
find_package(fmt REQUIRED)
find_package(Boost REQUIRED)

FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

FetchContent_Declare(
    googlebenchmark
    URL https://github.com/google/benchmark/archive/refs/tags/v1.9.1.zip
)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googlebenchmark)

enable_testing()
include(GoogleTest)

# --- database: header-only, generated from src/database/schema.py via cmg ---
add_library(database INTERFACE)
target_include_directories(database INTERFACE src/database src/database/generated)

# --- geometry: header-only Boost.Geometry wrapper over database types ---
add_library(geometry INTERFACE)
target_include_directories(geometry INTERFACE src)
target_link_libraries(geometry INTERFACE database Boost::boost)

# --- view_style: header-only ViewLayer/purpose/color config, distinct from
# the LEF-mirroring database - see CLAUDE.md for why these aren't schema/cmg-generated ---
add_library(view_style INTERFACE)
target_include_directories(view_style INTERFACE src)
target_link_libraries(view_style INTERFACE database)

# --- scene: header-only per-handle mutable view state (pan/zoom, selection, layer visibility) ---
add_library(scene INTERFACE)
target_include_directories(scene INTERFACE src)
target_link_libraries(scene INTERFACE database view_style)

# --- core: header-only generic building blocks shared between pipeline and
# render (RenderedShape/TinyShapeDot, VersionedStage) - neither module
# depends on the other for these, only both on core. See UPDATES.md item 16. ---
add_library(core INTERFACE)
target_include_directories(core INTERFACE src)
target_link_libraries(core INTERFACE database scene view_style)

# --- pipeline: header-only shape generation + viewport/size/layer filtering ---
add_library(pipeline INTERFACE)
target_include_directories(pipeline INTERFACE src)
target_link_libraries(pipeline INTERFACE database geometry scene view_style core)

# --- Skia (not vendored/built by this project - a pre-built checkout on
# this machine). SKIA_DIR must contain out/MacStatic/{libskia.a,...} built
# with is_component_build=false (static). Link set (include path, libskia.a,
# Homebrew harfbuzz/icu4c, CoreText/CoreFoundation/CoreGraphics frameworks)
# was determined empirically by linking a minimal SkPictureRecorder/
# SkSurface program against this exact checkout - see BENCHMARKS.md/
# CLAUDE.md for why no GPU (Ganesh/Metal) frameworks are needed: we only
# use the raster (CPU) surface APIs, and static linking never pulls in the
# unused GPU object files. Machine-specific - not portable/committed;
# override -DSKIA_DIR=... to point at a different checkout.
set(SKIA_DIR "/Users/john/Projects/synthosilicon/skia/skia" CACHE PATH
    "Path to a built Skia checkout (must contain out/MacStatic/libskia.a)")
add_library(skia INTERFACE)
target_include_directories(skia INTERFACE ${SKIA_DIR})
target_link_directories(skia INTERFACE /opt/homebrew/lib /opt/homebrew/opt/icu4c/lib)
target_link_libraries(skia INTERFACE
    ${SKIA_DIR}/out/MacStatic/libskia.a
    harfbuzz
    icuuc
    icudata
    jpeg
    png
    z
    webp
    webpdemux
)
if(APPLE)
    target_link_libraries(skia INTERFACE
        "-framework CoreText"
        "-framework CoreFoundation"
        "-framework CoreGraphics"
        "-framework CoreServices"
    )
endif()

# --- render: dbu->pixel transform + Skia SkPicture generation. Compiled
# (not header-only) so render.cpp alone pulls in the CoreText font manager's
# ApplicationServices.h - see Renderer::default_typeface()'s comment for why
# that can't be a header-only detail. ---
add_library(render STATIC src/render/render.cpp)
target_include_directories(render PUBLIC src)
target_link_libraries(render PUBLIC database geometry scene view_style core skia)

# --- io: LEF/DEF file readers ---
add_library(io STATIC src/io/lef_reader.cpp src/io/lef_writer.cpp)
add_dependencies(io lef_lib)
target_include_directories(io
    PUBLIC src/io
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/lefdef/lef/include
)
target_link_libraries(io
    PUBLIC database geometry
    PRIVATE spdlog::spdlog fmt::fmt
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lefdef/lef/lib/liblef.a
)
le_enable_coverage_instrumentation(io)

# --- api: the C API surface a Flutter plugin's Dart FFI binds to (see
# README's `api` module entry). Compiled (not header-only): the opaque
# LeHandle's real definition must stay out of api.hpp, which needs to be a
# plain-C-parseable header for ffigen/Dart FFI - see api.hpp's own comment. ---
add_library(api STATIC src/api/api.cpp)
target_include_directories(api PUBLIC src/api)
target_link_libraries(api PRIVATE database geometry scene view_style pipeline render io)
le_enable_coverage_instrumentation(api)

# --- le_tcl: Phase 0 SWIG toolchain spike (UPDATES.md item 15 /
# TCL_EXPLORATION.md) - SWIG-wraps src/tcl/le_tcl_shim.{hpp,cpp} (a thin
# Tcl-ergonomics layer over api.hpp - hidden session handle, domain-verb
# command names) into a loadable Tcl extension, proving the build/link
# story before any CRUD/filter-search work exists to wrap for real.
# Optional: only built if SWIG and Tcl are both found, so a machine not
# yet provisioned for this (e.g. CI) still configures/builds everything
# else. Pinned to Homebrew's tcl-tk@8 (Tcl 8.6), not the also-installed
# tcl-tk 9.0 - SWIG 4.5's Tcl support is battle-tested against 8.x, and
# Tcl 9's C ABI (released 2025) isn't yet a safe default target for this
# spike. Override TCL_TK_ROOT for a different Tcl 8.x install. ---
set(TCL_TK_ROOT "/opt/homebrew/opt/tcl-tk@8" CACHE PATH
    "Path to a Tcl 8.6 install (Homebrew tcl-tk@8 keg) used for the SWIG Tcl spike")
if(EXISTS "${TCL_TK_ROOT}/include/tcl-tk/tcl.h")
    set(TCL_INCLUDE_PATH "${TCL_TK_ROOT}/include/tcl-tk" CACHE PATH "" FORCE)
    set(TCL_LIBRARY "${TCL_TK_ROOT}/lib/libtcl8.6.dylib" CACHE FILEPATH "" FORCE)
    set(TCL_TCLSH "${TCL_TK_ROOT}/bin/tclsh8.6" CACHE FILEPATH "" FORCE)
endif()

find_package(SWIG 4.0)
find_package(TCL)

if(SWIG_FOUND AND TCL_FOUND)
    include(UseSWIG)
    set_property(SOURCE src/tcl/le_api.i PROPERTY CPLUSPLUS ON)
    swig_add_library(le_tcl
        LANGUAGE tcl
        SOURCES src/tcl/le_api.i src/tcl/le_tcl_shim.cpp
    )
    set_property(TARGET le_tcl PROPERTY SWIG_INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/src/api ${CMAKE_CURRENT_SOURCE_DIR}/src/tcl)
    target_include_directories(le_tcl PRIVATE src/api src/tcl ${TCL_INCLUDE_PATH})
    target_link_libraries(le_tcl PRIVATE api ${TCL_LIBRARY})
    # Hide this target's own copy of the vendored LEF parser's symbols
    # (everything under the LefDefParser:: namespace -
    # src/tcl/le_tcl_unexported_symbols.txt). Without this, a process
    # that *also* separately links the same static libraries
    # (api/io/render/liblef.a) into another Mach-O image (e.g.
    # lef_editor_plugin.framework, for LeTclBridge's embedded Tcl
    # console - see TCL_EXPLORATION.md's show_gui section) crashes: dyld
    # binds a call inside this module's own copy of the vendored parser
    # (specifically LefDefParser::lefGetKeyword's internal keyword-table
    # std::map, a function-local static) to the *other* image's
    # differently-initialized copy of the same symbol instead of this
    # module's own, reading through a garbage/uninitialized pointer -
    # reproduced headlessly and fixed here after a real crash report,
    # see ReadLefThroughTclFirstDoesNotCrash in
    # src/tcl/tests/session_handle_test.cpp.
    #
    # Deliberately a *linker*-level denylist
    # (-unexported_symbols_list), not CXX_VISIBILITY_PRESET or a
    # blanket -exported_symbols_list allowlist - two things learned by
    # trial, not assumed:
    # - CXX_VISIBILITY_PRESET only sets compile flags for this target's
    #   own two listed sources (le_tcl_shim.cpp, the SWIG wrap file) -
    #   api/io/render/lef_lib are separate CMake targets, already
    #   compiled into .a archives with default (visible) symbols, and a
    #   consuming target's own visibility preset can't retroactively
    #   hide symbols already baked into object code linked in from
    #   elsewhere. Confirmed by trial: with only CXX_VISIBILITY_PRESET
    #   set, the crash reproduced identically.
    # - A blanket -exported_symbols_list allowlist (hide everything
    #   except two named entry points) does stop the original crash, but
    #   breaks something worse: it also hides libc++ typeinfo symbols
    #   that are *supposed* to stay coalescable across dylib boundaries
    #   (weak/vague linkage), so std::system_error thrown deep in the
    #   parser's std::vector/std::map usage stops RTTI-matching a
    #   `catch (const std::exception&)` in the calling image - "unknown
    #   C++ exception" instead of a clean TCL_ERROR. Confirmed by trial
    #   (abi::__cxa_current_exception_type()->name() ==
    #   "NSt3__112system_errorE", uncaught by a std::exception handler).
    #   A denylist scoped to exactly the confirmed culprit
    #   (LefDefParser::) avoids this - everything else, including any
    #   std:: template instantiated with a LefDefParser:: type as a
    #   parameter (e.g. exception guards for vector<lefrOBSSpacing>),
    #   keeps its normal default visibility and stays coalescable.
    target_link_options(le_tcl PRIVATE
        "-Wl,-unexported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/src/tcl/le_tcl_unexported_symbols.txt"
    )

    # Regression check for the spike itself (src/tcl/tests/smoke_test.tcl) -
    # loads the built module under tclsh, sources le_tcl_procs.tcl, and
    # exercises the ergonomic command surface against the same
    # testcell.lef fixture api_test.cpp's C++ tests already use, asserting
    # the same expected values so this doesn't silently bit-rot.
    add_test(
        NAME le_tcl_smoke
        COMMAND ${TCL_TCLSH}
            ${CMAKE_CURRENT_SOURCE_DIR}/src/tcl/tests/smoke_test.tcl
            $<TARGET_FILE:le_tcl>
            ${CMAKE_CURRENT_SOURCE_DIR}/src/tcl/le_tcl_procs.tcl
            ${CMAKE_CURRENT_SOURCE_DIR}/src/api/tests/fixtures/testcell.lef
    )

    # Phase 5 regression check (src/tcl/tests/crud_test.tcl) - exercises
    # the full Terminal/TerminalPort/Obstruction/Shape CRUD+search
    # surface and the coordinate-list typemap, against the same
    # testcell.lef fixture.
    add_test(
        NAME le_tcl_crud
        COMMAND ${TCL_TCLSH}
            ${CMAKE_CURRENT_SOURCE_DIR}/src/tcl/tests/crud_test.tcl
            $<TARGET_FILE:le_tcl>
            ${CMAKE_CURRENT_SOURCE_DIR}/src/tcl/le_tcl_procs.tcl
            ${CMAKE_CURRENT_SOURCE_DIR}/src/api/tests/fixtures/testcell.lef
    )

    # Phase 6 batch shell (src/tcl/le_shell.cpp) - a small Tcl_Main-based
    # binary, not linked against le_tcl (it dynamically `load`s the built
    # shared module at startup, same as tclsh would) - only needs Tcl
    # itself.
    add_executable(le_shell src/tcl/le_shell.cpp)
    target_include_directories(le_shell PRIVATE ${TCL_INCLUDE_PATH})
    target_link_libraries(le_shell PRIVATE ${TCL_LIBRARY})

    # Batch-mode regression check (src/tcl/tests/shell_test.tcl) - runs
    # through the real compiled le_shell binary (not tclsh directly
    # loading le_tcl, which smoke_test.tcl/crud_test.tcl already cover),
    # verifying its own bootstrap (argv flag handling, `load`, sourcing
    # le_tcl_procs.tcl) actually works end to end.
    add_test(
        NAME le_tcl_shell
        COMMAND $<TARGET_FILE:le_shell>
            -module $<TARGET_FILE:le_tcl>
            -procs ${CMAKE_CURRENT_SOURCE_DIR}/src/tcl/le_tcl_procs.tcl
            ${CMAKE_CURRENT_SOURCE_DIR}/src/tcl/tests/shell_test.tcl
            ${CMAKE_CURRENT_SOURCE_DIR}/src/api/tests/fixtures/testcell.lef
    )

    # show_gui design regression check (src/tcl/tests/session_handle_test.cpp) -
    # a plain GTest binary, not a Tcl script: proves set_session_handle()
    # actually redirects le_tcl_shim.cpp's session() to an externally-
    # owned LeHandle*, the mechanism a Flutter-embedded Tcl console
    # relies on (see TCL_EXPLORATION.md's show_gui section). Kept out of
    # backend_tests (unconditional, built even without SWIG/TCL) since
    # this needs both.
    add_executable(le_tcl_session_test src/tcl/tests/session_handle_test.cpp)
    target_include_directories(le_tcl_session_test PRIVATE src/api ${TCL_INCLUDE_PATH})
    target_link_libraries(le_tcl_session_test PRIVATE api ${TCL_LIBRARY} GTest::gtest_main)
    target_compile_definitions(le_tcl_session_test PRIVATE
        API_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src/api/tests/fixtures"
        LE_TCL_MODULE_PATH="$<TARGET_FILE:le_tcl>"
    )
    add_dependencies(le_tcl_session_test le_tcl)
    gtest_discover_tests(le_tcl_session_test)
else()
    message(STATUS "SWIG and/or Tcl not found - skipping le_tcl spike target (see TCL_EXPLORATION.md Phase 0)")
endif()

# --- tests ---
add_executable(backend_tests
    src/database/tests/database_test.cpp
    src/database/tests/filter_test.cpp
    src/geometry/tests/geometry_test.cpp
    src/view_style/tests/view_style_test.cpp
    src/scene/tests/scene_test.cpp
    src/pipeline/tests/pipeline_test.cpp
    src/render/tests/render_test.cpp
    src/io/tests/lef_reader_test.cpp
    src/io/tests/lef_reader_utils_test.cpp
    src/io/tests/lef_writer_test.cpp
    src/api/tests/api_test.cpp
)
target_link_libraries(backend_tests PRIVATE database geometry view_style scene pipeline render io api fmt::fmt GTest::gtest_main)
target_compile_definitions(backend_tests PRIVATE
    LEFDEF_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src/lefdef/lef/TEST"
    IO_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src/io/tests/fixtures"
    API_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src/api/tests/fixtures"
    LEFDIFF_BIN="${CMAKE_CURRENT_SOURCE_DIR}/src/lefdef/lef/bin/lefdiff"
)
add_dependencies(backend_tests lef_lib)
le_enable_coverage_instrumentation(backend_tests)
gtest_discover_tests(backend_tests)

# --- benchmarks ---
add_executable(pipeline_benchmarks src/pipeline/benchmarks/pipeline_benchmark.cpp)
target_link_libraries(pipeline_benchmarks PRIVATE database geometry scene pipeline render io api benchmark::benchmark)
target_compile_definitions(pipeline_benchmarks PRIVATE
    BENCHMARK_DATA_DIR="${CMAKE_BINARY_DIR}/benchmark_data"
)
# Not instrumented for its own coverage (benchmarks aren't run under the
# `coverage` target) - but it links `io`, which IS instrumented when
# ENABLE_COVERAGE is on, and that instrumentation pulls in a profiling
# runtime symbol that only gets linked into the final executable if this
# target also passes -fprofile-instr-generate, or the link fails.
le_enable_coverage_instrumentation(pipeline_benchmarks)

# --- render_preview: dev-only tool, rasterizes each given LEF file's Designs
# to PNGs (preview/<lef-stem>__<design-name>.png) so they can be visually
# sanity-checked without waiting for Flutter texture wiring. Not a benchmark
# or a test - not run by ctest or the coverage target. ---
add_executable(render_preview src/pipeline/benchmarks/render_preview.cpp)
target_link_libraries(render_preview PRIVATE database geometry scene pipeline render io)
le_enable_coverage_instrumentation(render_preview)

# --- lef_roundtrip_diff: dev-only progress-tracking tool for UPDATES.md
# item 12 (LEF syntax completion) - reads a LEF file (default:
# complete.5.8.lef), writes every Design + the Technology's layers back out
# via LEFWriter, and reports what the vendored lefdiff tool's own
# normalized dump still shows as missing after the round trip. Not a
# benchmark or a test - not run by ctest or the coverage target, not a hard
# gate (see src/io/tests/lef_writer_test.cpp's own lefdiff-based CTest for
# the actual gated check, scoped to what this phase supports). ---
add_executable(lef_roundtrip_diff src/pipeline/benchmarks/lef_roundtrip_diff.cpp)
add_dependencies(lef_roundtrip_diff lef_lib)
target_link_libraries(lef_roundtrip_diff PRIVATE database geometry io)
target_compile_definitions(lef_roundtrip_diff PRIVATE
    LEFDEF_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src/lefdef/lef/TEST"
    LEFDIFF_BIN="${CMAKE_CURRENT_SOURCE_DIR}/src/lefdef/lef/bin/lefdiff"
)
le_enable_coverage_instrumentation(lef_roundtrip_diff)

# --- coverage: `cmake --build build --target coverage` (requires -DENABLE_COVERAGE=ON at configure time) ---
if(ENABLE_COVERAGE)
    set(COVERAGE_DIR ${CMAKE_BINARY_DIR}/coverage)
    # database/generated is cmg output (mostly boilerplate getters/to_string);
    # excluded for now rather than chasing coverage on generated code.
    set(COVERAGE_IGNORE_REGEX "${CMAKE_BINARY_DIR}/_deps|${CMAKE_SOURCE_DIR}/src/lefdef|${CMAKE_SOURCE_DIR}/src/database/generated")

    add_custom_target(coverage
        COMMAND ${CMAKE_COMMAND} -E make_directory ${COVERAGE_DIR}
        COMMAND ${CMAKE_COMMAND} -E env LLVM_PROFILE_FILE=${COVERAGE_DIR}/backend_tests.profraw $<TARGET_FILE:backend_tests>
        COMMAND ${LLVM_PROFDATA_EXE} merge -sparse ${COVERAGE_DIR}/backend_tests.profraw -o ${COVERAGE_DIR}/backend_tests.profdata
        COMMAND sh -c "${LLVM_COV_EXE} report $<TARGET_FILE:backend_tests> -instr-profile=${COVERAGE_DIR}/backend_tests.profdata --show-branch-summary --ignore-filename-regex='${COVERAGE_IGNORE_REGEX}' | tee ${COVERAGE_DIR}/report.txt"
        COMMAND sh -c "${LLVM_COV_EXE} export $<TARGET_FILE:backend_tests> -instr-profile=${COVERAGE_DIR}/backend_tests.profdata -format=lcov --ignore-filename-regex='${COVERAGE_IGNORE_REGEX}' > ${COVERAGE_DIR}/lcov.info"
        DEPENDS backend_tests
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Running backend_tests with coverage instrumentation, then reporting line+branch coverage"
        USES_TERMINAL
        VERBATIM
    )
endif()
