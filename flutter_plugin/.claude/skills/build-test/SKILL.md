---
name: build-test
description: Build the backend native code this plugin links against, then run flutter pub get/analyze/test and build the macOS example app. Use when asked to build, compile, test, or run the flutter plugin.
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
the vendored `liblef.a`. Build them first so the plugin's native side has
something to link against:

```
cmake -S ../backend -B ../backend/build -DCMAKE_BUILD_TYPE=Debug
cmake --build ../backend/build --target api render io -j
```

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

## 3. macOS example app (verified working end-to-end)

```
cd example
flutter build macos --debug   # or: flutter run -d macos
```

Requires backend/build's `libapi.a`/`librender.a`/`libio.a` (step 1) and a
Skia checkout (`$SKIA_DIR` or backend's own default,
`/Users/john/Projects/synthosilicon/skia/skia`) — the podspec raises a
clear error naming the missing file/command if these aren't there yet.

**If CocoaPods doesn't pick up a podspec/Classes change** (e.g. a new file
added to `macos/Classes/`): `pod install` only reruns when Flutter thinks
the Podfile/plugin list changed, not on every glob match. Force it:
```
rm -rf macos/Pods macos/Podfile.lock
flutter build macos --debug
```

Verify the native link actually exposes the API, not just that it
compiled:
```
nm -gU example/build/macos/Build/Products/Debug/lef_editor_plugin_example.app/Contents/Frameworks/lef_editor_plugin.framework/lef_editor_plugin | grep le_
```
should list all nine `le_*` symbols.

**Verify the texture actually renders, not just that the app launches** —
a blank white `Texture` widget with no crash and no error is the single
most likely failure mode here (found this way once already: a missing
`kCVPixelBufferIOSurfacePropertiesKey` produced exactly this, with the
real error buried in the system log, not the build output). Launch the
built binary directly (not `open`, which detaches stdout) and grep for the
platform log lines:
```
example/build/macos/Build/Products/Debug/lef_editor_plugin_example.app/Contents/MacOS/lef_editor_plugin_example > /tmp/lef_app_log.txt 2>&1 &
sleep 3
grep -i "Metal texture\|CVReturn" /tmp/lef_app_log.txt   # should be empty
```
Then screenshot (`screencapture -x <path>` after `osascript -e 'tell
application "lef_editor_plugin_example" to activate'`) and look for the
testcell's boundary + pink M1 pin labeled "A", not just a grey-bordered
blank square.

## 4. Linux example app — cannot be built or run here

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
