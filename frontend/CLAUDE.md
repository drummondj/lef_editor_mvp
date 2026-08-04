# LEF Layout Editor MVP — Frontend

The real end-user desktop app — distinct from `../flutter_plugin/example`,
which exists only to prove the plugin's texture path works. Loads a
user-chosen LEF file and renders it via `lef_editor_plugin`'s
`LeEditor`/`LeTexture`, which binds to the backend's C API
(`../backend/src/api/api.hpp`) through Dart FFI. See `../backend/CLAUDE.md`
for the backend architecture and `../flutter_plugin/CLAUDE.md` for the
plugin's Dart FFI / native texture wiring — neither is duplicated here.

## Requirements (inherited from backend/README.md)

- Target platforms: macOS (dev) and Linux (deploy, blocked upstream — see
  `../flutter_plugin/CLAUDE.md`'s Open design questions). No Windows/iOS/
  Android — those platform directories were never generated.
- Keep responses and docs concise.

## Current state

Early and minimal — not yet wired to the plugin:

- `lib/main.dart` — bare `MaterialApp` shell; `Home` is its only page.
- `lib/pages/home.dart` — one "Open LEF ..." button. `openFilePicker()`
  uses `file_selector` to open a native, `.lef`-filtered dialog and
  currently only `debugPrint`s the chosen path — it does not yet call into
  `lef_editor_plugin`'s `LeEditor` to read the file or display a `Texture`.
  `../flutter_plugin/example/lib/main.dart` is the reference for that
  wiring (load → `readLef`/`setCurrentDesign` → `createTexture` →
  `Texture` widget) once this page grows past the picker.
- `pubspec.yaml` already declares `lef_editor_plugin: path: ../flutter_plugin`,
  so the backend's native code must be built (see the `build-test` skill)
  before `flutter run`/`build macos` will link successfully.
- macOS is sandboxed (`macos/Runner/*.entitlements`,
  `com.apple.security.app-sandbox`); `com.apple.security.files.user-selected.read-only`
  is required for the native open panel and is already set in both Debug
  and Release entitlements.

## Skills

- `build-test` — build the backend native code the plugin links against,
  then `flutter pub get`/`analyze`/`test` and run/build this app.
