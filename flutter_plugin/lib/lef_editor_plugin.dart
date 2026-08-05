import 'dart:ffi' as ffi;
import 'dart:io';

import 'package:ffi/ffi.dart' as pkg_ffi;
import 'package:flutter/services.dart';

import 'lef_editor_plugin_bindings_generated.dart';

const String _libName = 'lef_editor_plugin';

/// Method channel used only to hand a texture id back and forth with
/// native platform code (see [LeEditor.createTexture]/[LeTexture]) - Dart
/// FFI can't reach the Flutter engine's texture registry itself, only
/// platform embedder code can. Must match the channel name registered by
/// `LefEditorPlugin` (macOS: `macos/Classes/LefEditorPlugin.swift`; Linux:
/// `linux/lef_editor_plugin.cc`).
const MethodChannel _channel = MethodChannel('lef_editor_plugin');

/// The dynamic library in which the symbols for [LefEditorPluginBindings]
/// can be found.
final ffi.DynamicLibrary _dylib = () {
  if (Platform.isMacOS) {
    return ffi.DynamicLibrary.open('$_libName.framework/$_libName');
  }
  if (Platform.isLinux) {
    return ffi.DynamicLibrary.open('lib$_libName.so');
  }
  throw UnsupportedError(
    'lef_editor_plugin only supports macOS and Linux, not '
    '${Platform.operatingSystem} (see this plugin\'s CLAUDE.md).',
  );
}();

final LefEditorPluginBindings _bindings = LefEditorPluginBindings(_dylib);

/// A rendered frame from [LeEditor.renderPixelBuffer]: a copy of the
/// backend's RGBA8888 pixel buffer (see api.hpp's `LePixelBuffer`), safe to
/// keep around after the next render call unlike the native buffer it was
/// copied from.
class LeFrame {
  const LeFrame({
    required this.pixels,
    required this.width,
    required this.height,
    required this.rowBytes,
  });

  /// Premultiplied RGBA8888, row-major, top-to-bottom. [rowBytes] may
  /// exceed `width * 4` - always index by it, never assume a tight stride.
  final Uint8List pixels;
  final int width;
  final int height;
  final int rowBytes;
}

/// One editor instance: owns a native `LeHandle*` (a Root/ViewLayerSet/
/// Scene/Pipeline/Renderer, see api.hpp) that's reused across calls,
/// matching the backend's own "one instance per Scene-equivalent lifetime"
/// design - don't create a fresh [LeEditor] per frame.
class LeEditor {
  LeEditor() : _handle = _bindings.le_create() {
    // le_create() is documented to never return null; this is a sanity
    // check against that contract changing, not an expected runtime path.
    if (_handle == ffi.nullptr) {
      throw StateError('le_create() returned null');
    }
  }

  final ffi.Pointer<LeHandle> _handle;
  bool _disposed = false;

  /// The raw handle address, for handing off to native platform texture
  /// code - see this plugin's CLAUDE.md: the texture path calls
  /// `le_render_pixel_buffer` directly from native code on every frame
  /// pull, not through this Dart wrapper.
  int get nativeHandleAddress => _handle.address;

  void _checkNotDisposed() {
    if (_disposed) {
      throw StateError('LeEditor used after dispose()');
    }
  }

  /// Reads a LEF file into this handle's shared Root, deriving a library
  /// name from the file's stem. Safe to call multiple times on the same
  /// handle - e.g. a tech file followed by macro file(s) that reference its
  /// layers by name (pass the tech file first). Returns true on success.
  bool readLef(String path) {
    _checkNotDisposed();
    final pathPtr = path.toNativeUtf8();
    try {
      return _bindings.le_read_lef(_handle, pathPtr.cast()) == 0;
    } finally {
      pkg_ffi.calloc.free(pathPtr);
    }
  }

  /// Number of Designs loaded across every LEF file read into this handle
  /// so far.
  int get designCount {
    _checkNotDisposed();
    return _bindings.le_design_count(_handle);
  }

  /// Name of the Design at [index] (0..[designCount] - 1), or null if
  /// [index] is out of range.
  String? designName(int index) {
    _checkNotDisposed();
    final namePtr = _bindings.le_design_name(_handle, index);
    if (namePtr == ffi.nullptr) return null;
    return namePtr.cast<pkg_ffi.Utf8>().toDartString();
  }

  /// Selects the Design at [index] as the one [renderPixelBuffer] renders
  /// (its Abstract view). Returns true on success; the current selection is
  /// left unchanged on failure.
  bool setCurrentDesign(int index) {
    _checkNotDisposed();
    return _bindings.le_set_current_design(_handle, index) == 0;
  }

  /// Zooms the viewport, keeping the dbu point under screen pixel (x, y)
  /// fixed on screen. [factor] is a signed fractional step applied to the
  /// current scale (new_scale = scale * (1 + factor)) - positive zooms in,
  /// negative zooms out (e.g. 0.1 zooms in 10%, -0.1 zooms out 10%); a
  /// factor <= -1.0 is ignored. [x]/[y] are in the same pixel space as
  /// [renderPixelBuffer]'s output (top-left origin, y increasing downward)
  /// - safe to feed straight from a pointer/tap event on the rendered
  /// image. The backend owns pan/scale entirely - there is no direct
  /// setter; use [fitScene] to reset to a known view.
  void zoom(double factor, int x, int y) {
    _checkNotDisposed();
    _bindings.le_zoom(_handle, factor, x, y);
  }

  /// Pans the viewport by a fraction of its own size, in dbu-space
  /// directions (positive [xFactor]/[yFactor] move the view toward
  /// increasing dbu x/y, not screen space). E.g. `xFactor: 1.0` pans right
  /// by exactly one full viewport width of content.
  void pan(double xFactor, double yFactor) {
    _checkNotDisposed();
    _bindings.le_pan(_handle, xFactor, yFactor);
  }

  /// Sets the viewport size in pixels - also the size of the buffer
  /// [renderPixelBuffer] produces.
  void setViewportSize(int widthPx, int heightPx) {
    _checkNotDisposed();
    _bindings.le_set_viewport_size(_handle, widthPx, heightPx);
  }

  /// Fits the viewport's pan/scale to the currently selected Design's
  /// content bbox: uniform scale (no stretch) so the content fills the
  /// viewport set via [setViewportSize] with [paddingPx] of margin on every
  /// side, pan centering it. Degrades to scale 1.0 / pan (0, 0) if no
  /// Design is selected or its Abstract has no shapes, rather than
  /// throwing. Call [setViewportSize] first - this fits to that viewport's
  /// current size, not a size set afterward.
  void fitScene(int paddingPx) {
    _checkNotDisposed();
    _bindings.le_fit_scene(_handle, paddingPx);
  }

  /// Runs the full pipeline+render chain for the currently selected Design
  /// and viewport, returning the resulting frame copied into Dart-owned
  /// memory. The native `LePixelBuffer.data` this copies from is only valid
  /// until the next `le_render_pixel_buffer`/`le_destroy` call on this
  /// handle (see api.hpp) - never cache the native pointer itself, only
  /// this copy.
  ///
  /// Calling this from Dart is for previewing/testing without a platform
  /// texture; the actual on-screen path calls `le_render_pixel_buffer`
  /// directly from native texture code instead (see CLAUDE.md).
  LeFrame renderPixelBuffer() {
    _checkNotDisposed();
    final buffer = _bindings.le_render_pixel_buffer(_handle);
    final byteCount = buffer.row_bytes * buffer.height;
    final pixels = buffer.data == ffi.nullptr || byteCount <= 0
        ? Uint8List(0)
        : Uint8List.fromList(
            buffer.data.cast<ffi.Uint8>().asTypedList(byteCount),
          );
    return LeFrame(
      pixels: pixels,
      width: buffer.width,
      height: buffer.height,
      rowBytes: buffer.row_bytes,
    );
  }

  /// Registers a platform [LeTexture] backed by this handle, for use with
  /// Flutter's `Texture(textureId: ...)` widget - see [LeTexture]'s own doc
  /// for the lifetime and thread-safety constraints this implies. Native
  /// code calls `le_render_pixel_buffer` on this handle directly (not
  /// through this Dart wrapper) whenever the engine pulls a frame, which
  /// only happens after a [LeTexture.markFrameAvailable] call - creating
  /// the texture does not render eagerly on its own.
  Future<LeTexture> createTexture() async {
    _checkNotDisposed();
    final textureId = await _channel.invokeMethod<int>('createTexture', {
      'handleAddress': nativeHandleAddress,
    });
    if (textureId == null) {
      throw StateError('createTexture returned null');
    }
    return LeTexture._(textureId);
  }

  /// Destroys the native handle. Safe to call more than once. Dispose every
  /// [LeTexture] created from this editor first - see [LeTexture]'s own doc.
  void dispose() {
    if (_disposed) return;
    _bindings.le_destroy(_handle);
    _disposed = true;
  }
}

/// A live platform texture backed by one [LeEditor]'s native handle. Obtain
/// via [LeEditor.createTexture]; display with a Flutter
/// `Texture(textureId: texture.textureId)` widget.
///
/// **Lifetime:** the source [LeEditor] must outlive this [LeTexture].
/// Native texture code holds the raw handle address, not a reference that
/// keeps the handle alive, so disposing the [LeEditor] first leaves the
/// texture pointing at freed memory the next time the engine pulls a
/// frame. Always [dispose] every [LeTexture] before disposing the [LeEditor]
/// it came from.
///
/// **Thread safety (unresolved, not just undocumented):** native code pulls
/// a frame - calling `le_render_pixel_buffer` on the shared handle - on the
/// Flutter engine's raster thread, asynchronously relative to whatever
/// thread calls into [LeEditor] from Dart. Backend's Scene/Pipeline/
/// Renderer aren't documented as thread-safe (see backend/CLAUDE.md), so a
/// [LeEditor] setter (`zoom`/`pan`/`setViewportSize`/`fitScene`/
/// `setCurrentDesign`/`readLef`) racing a pending, not-yet-rendered
/// [markFrameAvailable] is a genuine data race, not just a hypothetical
/// one. Not solved at this layer - a real fix needs a lock inside the C
/// API itself, guarding every `le_*` call on a handle regardless of which
/// thread it's called from.
class LeTexture {
  LeTexture._(this.textureId);

  /// Pass to Flutter's `Texture(textureId: ...)` widget.
  final int textureId;

  bool _disposed = false;

  /// Tells the engine to pull a fresh frame on the raster thread - call
  /// after any change on the source [LeEditor] that should become visible.
  /// Cheap to call even when nothing actually changed (see
  /// `le_render_pixel_buffer`'s own caching note in api.hpp).
  Future<void> markFrameAvailable() async {
    if (_disposed) return;
    await _channel.invokeMethod<void>('markTextureFrameAvailable', {
      'textureId': textureId,
    });
  }

  /// Unregisters the texture. Safe to call more than once. Call this
  /// before disposing the source [LeEditor], not after - see this class's
  /// own Lifetime doc.
  Future<void> dispose() async {
    if (_disposed) return;
    _disposed = true;
    await _channel.invokeMethod<void>('disposeTexture', {
      'textureId': textureId,
    });
  }
}
