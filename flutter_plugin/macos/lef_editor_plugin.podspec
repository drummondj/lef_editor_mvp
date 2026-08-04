#
# To learn more about a Podspec see http://guides.cocoapods.org/syntax/podspec.html.
# Run `pod lib lint lef_editor_plugin.podspec` to validate before publishing.
#

# le_*() (see ../src/lef_editor_plugin.h) is implemented in
# backend/src/api/api.cpp, not in this plugin's own sources - link the
# backend's own CMake build output directly rather than recompiling it
# here. This link recipe is a manual mirror of backend/CMakeLists.txt's
# `api`/`render`/`io`/`skia` targets (there's no automated sharing between
# a CMakeLists.txt and a podspec) - keep the two in sync by hand; see this
# plugin's CLAUDE.md. Verified against the exact command that already
# links backend_tests successfully on this machine (see
# backend/build/CMakeFiles/backend_tests.dir/link.txt).
#
# CocoaPods loads this podspec through a `.symlinks/plugins/...` path, not
# this file's real location - File.realpath resolves that symlink first,
# or the relative walk below lands inside .symlinks instead of the repo.
backend_dir = File.expand_path('../../backend', File.realpath(__dir__))
backend_build = File.join(backend_dir, 'build')
skia_dir = ENV['SKIA_DIR'] || '/Users/john/Projects/synthosilicon/skia/skia'

unless File.exist?(File.join(backend_build, 'libapi.a'))
  raise "backend/build/libapi.a not found - build it first: " \
        "cmake -S #{backend_dir} -B #{backend_build} && " \
        "cmake --build #{backend_build} --target api render io -j"
end

Pod::Spec.new do |s|
  s.name             = 'lef_editor_plugin'
  s.version          = '0.0.1'
  s.summary          = 'Flutter plugin binding the LEF Layout Editor C API and rendering le_render_pixel_buffer as a platform Texture.'
  s.description      = <<-DESC
Flutter plugin binding the LEF Layout Editor C API and rendering le_render_pixel_buffer as a platform Texture.
                       DESC
  s.homepage         = 'http://example.com'
  s.license          = { :file => '../LICENSE' }
  s.author           = { 'Your Company' => 'email@example.com' }

  # This will ensure the source files in Classes/ are included in the native
  # builds of apps using this FFI plugin. Podspec does not support relative
  # paths, so Classes contains a forwarder C file that relatively imports
  # `../src/*` so that the C sources can be shared among all target platforms.
  s.source           = { :path => '.' }
  s.source_files = 'Classes/**/*'

  # If your plugin requires a privacy manifest, for example if it collects user
  # data, update the PrivacyInfo.xcprivacy file to describe your plugin's
  # privacy impact, and then uncomment this line. For more information,
  # see https://developer.apple.com/documentation/bundleresources/privacy_manifest_files
  # s.resource_bundles = {'lef_editor_plugin_privacy' => ['Resources/PrivacyInfo.xcprivacy']}

  s.dependency 'FlutterMacOS'

  s.platform = :osx, '10.11'
  s.pod_target_xcconfig = {
    'DEFINES_MODULE' => 'YES',
    'LIBRARY_SEARCH_PATHS' => "\"/opt/homebrew/lib\" \"/opt/homebrew/opt/icu4c/lib\"",
    'OTHER_LDFLAGS' => [
      # api.cpp's le_*() functions are never referenced by this plugin's
      # own compiled sources (Dart only finds them via dlsym at runtime) -
      # without -force_load, the linker drops libapi.a's object file
      # entirely since nothing in this link appears to need it.
      "-Wl,-force_load,#{File.join(backend_build, 'libapi.a')}",
      File.join(backend_build, 'librender.a'),
      File.join(backend_build, 'libio.a'),
      File.join(backend_dir, 'src/lefdef/lef/lib/liblef.a'),
      File.join(skia_dir, 'out/MacStatic/libskia.a'),
      '-lharfbuzz', '-licuuc', '-licudata', '-ljpeg', '-lpng', '-lz', '-lwebp', '-lwebpdemux',
      '-lspdlog', '-lfmt',
      # Everything above is a C++ archive underneath (operator new/delete,
      # __cxa_*, __gxx_personality_v0 all get pulled in) - linking libc++
      # itself is handled by Classes/LeApiBridge.mm being an Objective-C++
      # source, which makes Xcode select the C++ linker driver for this
      # target, not by a flag here: neither '-lc++' nor an explicit
      # libc++.tbd path survives CocoaPods' OTHER_LDFLAGS merging
      # (confirmed by trial - both are silently dropped, no error), so
      # this has to be solved by the target having a real C++/ObjC++
      # source instead.
      '-framework CoreText', '-framework CoreFoundation', '-framework CoreGraphics', '-framework CoreServices',
      '-Wl,-rpath,/opt/homebrew/lib', '-Wl,-rpath,/opt/homebrew/opt/icu4c/lib',
    ].join(' '),
  }
  s.swift_version = '5.0'
end
