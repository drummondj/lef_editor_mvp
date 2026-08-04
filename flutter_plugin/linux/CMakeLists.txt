# The Flutter tooling requires that developers have CMake 3.10 or later
# installed. You should not increase this version, as doing so will cause
# the plugin to fail to compile for some customers of the plugin.
cmake_minimum_required(VERSION 3.10)

# Project-level configuration.
set(PROJECT_NAME "lef_editor_plugin")
project(${PROJECT_NAME} LANGUAGES CXX)

# Invoke the build for native code shared with the other target platforms.
# This can be changed to accommodate different builds.
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../src" "${CMAKE_CURRENT_BINARY_DIR}/shared")

# --- lef_editor_plugin_plugin: the method-channel/texture side (see
# pubspec.yaml's `pluginClass: LefEditorPlugin`), separate from the
# `lef_editor_plugin` FFI shared library above - registers the
# "lef_editor_plugin" method channel and an FlPixelBufferTexture (see
# lef_editor_plugin.cc/lef_texture.cc), mirroring
# ../macos/Classes/LefEditorPlugin.swift/LeTexture.swift.
#
# UNVERIFIED: written against the real vendored Flutter Linux embedder
# headers (this repo has no Linux toolchain/GTK - never built here). This
# target must still link successfully with LE_LINK_BACKEND OFF (the
# default, see ../src/CMakeLists.txt's own comment on why) - Flutter's
# generated_plugins.cmake unconditionally does
# `target_link_libraries(${BINARY_NAME} PRIVATE lef_editor_plugin_plugin)`
# once pluginClass is declared, so unlike the FFI shared library (where
# missing backend symbols are only a problem once Dart actually looks them
# up via dlsym), this target has no equivalent "fine to be incomplete"
# state - lef_texture.cc's copy_pixels therefore has a real implementation
# gated behind LE_LINK_BACKEND_ENABLED and a stub (always fails cleanly)
# when it's off, rather than the whole target being conditional.
set(PLUGIN_NAME "${PROJECT_NAME}_plugin")
add_library(${PLUGIN_NAME} SHARED
  "lef_editor_plugin.cc"
  "lef_texture.cc"
)
apply_standard_settings(${PLUGIN_NAME})
set_target_properties(${PLUGIN_NAME} PROPERTIES CXX_VISIBILITY_PRESET hidden)
target_compile_definitions(${PLUGIN_NAME} PRIVATE FLUTTER_PLUGIN_IMPL)
target_include_directories(${PLUGIN_NAME} INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(${PLUGIN_NAME} PRIVATE flutter)
target_link_libraries(${PLUGIN_NAME} PRIVATE PkgConfig::GTK)

if(LE_LINK_BACKEND)
  # LE_LINK_BACKEND/LE_BACKEND_DIR/LE_BACKEND_BUILD/LE_SKIA_DIR are all
  # defined in ../src/CMakeLists.txt, already processed by the
  # add_subdirectory(../src) call above - CMake cache variables are
  # global, not scoped to the directory that defined them.
  target_compile_definitions(${PLUGIN_NAME} PRIVATE LE_LINK_BACKEND_ENABLED)
  target_link_libraries(${PLUGIN_NAME} PRIVATE
    "-Wl,--whole-archive" "${LE_BACKEND_BUILD}/libapi.a" "-Wl,--no-whole-archive"
    "${LE_BACKEND_BUILD}/librender.a"
    "${LE_BACKEND_BUILD}/libio.a"
    "${LE_BACKEND_DIR}/src/lefdef/lef/lib/liblef.a"
    "${LE_SKIA_DIR}/out/Linux/libskia.a"
    ${LE_HARFBUZZ_LIBRARIES} ${LE_ICU_LIBRARIES} ${LE_JPEG_LIBRARIES} ${LE_PNG_LIBRARIES} ${LE_WEBP_LIBRARIES}
    spdlog::spdlog fmt::fmt
    z
  )
endif()

# List of absolute paths to libraries that should be bundled with the plugin.
# This list could contain prebuilt libraries, or libraries created by an
# external build triggered from this build file.
#
# lef_editor_plugin_plugin itself (the target above) is bundled
# automatically by Flutter's own generated_plugins.cmake, which expects
# exactly this variable name for anything *else* that needs bundling - the
# separate FFI shared library, in this plugin's case.
set(lef_editor_plugin_bundled_libraries
  # Defined in ../src/CMakeLists.txt.
  # This can be changed to accommodate different builds.
  $<TARGET_FILE:lef_editor_plugin>
  PARENT_SCOPE
)
