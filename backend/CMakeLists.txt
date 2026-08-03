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
    BYPRODUCTS "${CMAKE_CURRENT_SOURCE_DIR}/src/lefdef/lef/lib/liblef.a"
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

# --- pipeline: header-only shape generation + viewport/size/layer filtering ---
add_library(pipeline INTERFACE)
target_include_directories(pipeline INTERFACE src)
target_link_libraries(pipeline INTERFACE database geometry scene view_style)

# --- io: LEF/DEF file readers ---
add_library(io STATIC src/io/lef_reader.cpp)
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

# --- tests ---
add_executable(backend_tests
    src/database/tests/database_test.cpp
    src/geometry/tests/geometry_test.cpp
    src/view_style/tests/view_style_test.cpp
    src/scene/tests/scene_test.cpp
    src/pipeline/tests/pipeline_test.cpp
    src/pipeline/tests/pipeline_cache_test.cpp
    src/io/tests/lef_reader_test.cpp
    src/io/tests/lef_reader_utils_test.cpp
)
target_link_libraries(backend_tests PRIVATE database geometry view_style scene pipeline io GTest::gtest_main)
target_compile_definitions(backend_tests PRIVATE
    LEFDEF_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src/lefdef/lef/TEST"
    IO_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src/io/tests/fixtures"
)
le_enable_coverage_instrumentation(backend_tests)
gtest_discover_tests(backend_tests)

# --- benchmarks ---
add_executable(pipeline_benchmarks src/pipeline/benchmarks/pipeline_benchmark.cpp)
target_link_libraries(pipeline_benchmarks PRIVATE database geometry scene pipeline io benchmark::benchmark)
target_compile_definitions(pipeline_benchmarks PRIVATE
    BENCHMARK_DATA_DIR="${CMAKE_BINARY_DIR}/benchmark_data"
)
# Not instrumented for its own coverage (benchmarks aren't run under the
# `coverage` target) - but it links `io`, which IS instrumented when
# ENABLE_COVERAGE is on, and that instrumentation pulls in a profiling
# runtime symbol that only gets linked into the final executable if this
# target also passes -fprofile-instr-generate, or the link fails.
le_enable_coverage_instrumentation(pipeline_benchmarks)

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
