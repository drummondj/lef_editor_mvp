---
name: build-test
description: Build the backend native code this plugin links against, then run flutter pub get/analyze/test. Use when asked to build, compile, test, or run the flutter plugin.
user-invocable: true
allowed-tools:
  - Bash
  - Read
---

# Build and test the Flutter plugin

## 1. Backend native code

The plugin links against the backend's `api`/`render`/`io` targets
(`backend/src/api/`, `src/render/`, `src/io/`), which transitively pull in
`database`/`geometry`/`scene`/`view_style`/`pipeline` (all header-only) and
the vendored `liblef.a`. This plugin deliberately links the **Release**
build (`backend/build_release`), not Debug - it's what a real running
Flutter app embeds, so it needs actual optimized performance (see the
podspec's own comment). Build it first so the plugin's native side has
something to link against:

```
cmake -S ../backend -B ../backend/build_release -DCMAKE_BUILD_TYPE=Release
cmake --build ../backend/build_release --target api render io -j
```

`../backend/build` (Debug) is a separate tree used for backend's own
`ctest`/development workflow - it's not what this plugin links, but both
trees are expected to exist and be kept up to date together going
forward (rebuild both after a backend source change), not just Release
built on-demand for benchmarking.

See `../backend/CLAUDE.md`'s Build section for Skia/Boost/spdlog/fmt setup
if this fails to configure.

## 2. Dart/Flutter side

```
flutter pub get
dart analyze
flutter test
```

`dart analyze` excludes `lib/lef_editor_plugin_bindings_generated.dart`
(see `analysis_options.yaml`) — that file has ~60 harmless
`unused_element`/`unused_field` warnings from transitively-included
`<stdint.h>`/`<pthread.h>` cruft, not from anything we wrote.

## 3. Verifying the native link end-to-end

This plugin no longer ships its own example app (removed — `../frontend`
is the real end-user app and the live consumer of this plugin now).
Verify the native link/build via `../frontend`'s own build (`frontend`'s
own skill, once one exists) — `flutter build macos` there succeeds,
`nm -gU` on the built `lef_editor_plugin.framework` shows all nine `le_*`
symbols, and a loaded LEF file's geometry actually renders through the
`LeEditor` → `LeTexture` → `Texture` widget chain (not just "didn't
crash" — a blank white `Texture` widget with no error is the single most
likely failure mode; check the platform log for `Metal texture`/
`CVReturn` lines on macOS, same as any Skia/CVPixelBuffer issue would
produce).

**If CocoaPods doesn't pick up a podspec/Classes change** (e.g. a new file
added to `macos/Classes/`): `pod install` only reruns when Flutter thinks
the Podfile/plugin list changed, not on every glob match. Force it (run
from whatever app consumes this plugin, e.g. `../frontend/macos`):
```
rm -rf macos/Pods macos/Podfile.lock
flutter build macos --debug
```

## 4. Linux — cannot be built or run here

`src/CMakeLists.txt`/`linux/CMakeLists.txt`'s `LE_LINK_BACKEND` option is
`OFF` by default and genuinely can't be turned on successfully yet — see
this plugin's CLAUDE.md's "Open design questions" for the real upstream
blockers (backend's CoreText-only font manager, no Linux Skia checkout).
Beyond that, this dev machine has no Linux toolchain/GTK at all, so
`linux/lef_editor_plugin.cc`/`lef_texture.cc` have never been compiled,
only written against the real vendored Flutter Linux embedder headers
(`/Users/john/Projects/flutter/engine/src/flutter/shell/platform/linux/public/flutter_linux/`
on this machine, if that checkout is still present). Don't spend time
debugging a Linux build failure here without first checking whether the
backend gaps have landed *and* you're actually on a Linux machine.

## 5. Reporting

Report failures concisely: file:line and the actual error, not the full
build log (native link failures especially — grep for the actual missing
symbol/file rather than pasting the whole `ld` output). Per the backend's
own convention, don't guess at performance fixes — this skill is build/test
only.
