import 'dart:ffi' as ffi;
import 'dart:io';

import 'package:ffi/ffi.dart' as pkg_ffi;
import 'package:flutter/services.dart';

import 'lef_editor_plugin_bindings_generated.dart';

/// Re-exported so callers of [LeEditor.keyDown]/[LeEditor.keyUp]/
/// [LeEditor.isKeyHeld] can reference key codes (e.g.
/// `LeKeyCode.LE_KEY_SHIFT`) without importing the generated bindings
/// file themselves - it's a plain generated enum (no ffi.Struct/native
/// pointer involved), so unlike `LePixelBuffer`/`LeLibraryId`/etc. there's
/// no need to wrap it in a hand-written type first.
export 'lef_editor_plugin_bindings_generated.dart' show LeKeyCode;

/// Re-exported for the same reason as [LeKeyCode] - see
/// [LeEditor.selectedObjectKind]/[LeSelectedProperty.type].
export 'lef_editor_plugin_bindings_generated.dart'
    show LeSelectionKind, LePropertyType;

/// Re-exported so callers get the pointer/key convenience layer
/// (handlePointerEvent/handleKeyEvent) from the same import used for
/// LeEditor itself - see lef_editor_input.dart.
export 'lef_editor_input.dart' show LeEditorInput;

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

/// A stable identity for one Library, Design, or Abstract (mirrors the
/// database's own `Id<Tag>` handles at the FFI boundary - see api.hpp's
/// `LeLibraryId`/`LeDesignId`/`LeAbstractId`). Unlike [LeFrame]'s pixel
/// data, these carry no native pointer and no validity window - safe to
/// hold onto indefinitely and pass back into e.g. [LeEditor.setCurrentDesignById]
/// later, but only ever a value read from this API, never hand-built.
abstract class _LeRef {
  const _LeRef(this.index, this.generation);

  final int index;
  final int generation;

  /// False for a ref read off an invalid/out-of-range row (`index` is the
  /// database's own sentinel, `Id<Tag>::valid()`'s complement).
  bool get isValid => index != 0xFFFFFFFF;

  @override
  bool operator ==(Object other) =>
      other is _LeRef &&
      other.runtimeType == runtimeType &&
      other.index == index &&
      other.generation == generation;

  @override
  int get hashCode => Object.hash(runtimeType, index, generation);
}

/// Identifies one Library (one [LeEditor.readLef] call's worth of Designs).
class LeLibraryRef extends _LeRef {
  const LeLibraryRef(super.index, super.generation);
}

/// Identifies one Design within a Library.
class LeDesignRef extends _LeRef {
  const LeDesignRef(super.index, super.generation);
}

/// Identifies one Design's Abstract (placement) view.
class LeAbstractRef extends _LeRef {
  const LeAbstractRef(super.index, super.generation);
}

/// One row of [LeEditor.library]: a Library's identity and name.
class LeLibrary {
  const LeLibrary({required this.id, required this.name});

  final LeLibraryRef id;
  final String name;
}

/// One row of [LeEditor.libraryDesign]: a Design's identity (plus its
/// parent Library's) and name.
class LeDesignEntry {
  const LeDesignEntry({
    required this.libraryId,
    required this.id,
    required this.abstractId,
    required this.name,
  });

  final LeLibraryRef libraryId;
  final LeDesignRef id;

  /// Null if this Design has no Abstract view yet (see api.hpp's
  /// `LeDesignInfo::abstract_id`).
  final LeAbstractRef? abstractId;
  final String name;
}

/// Mirrors `le::ViewLayerPurpose`'s declaration order (api.hpp's
/// `le_purpose_at`/`le_is_purpose_visible`/etc.) - the layer widget's
/// column axis, independent of any row (see [LeEditor.purposeAt]).
enum LeLayerPurpose {
  terminal,
  obstruction,
  boundary;

  static LeLayerPurpose? fromValue(int value) => switch (value) {
    0 => LeLayerPurpose.terminal,
    1 => LeLayerPurpose.obstruction,
    2 => LeLayerPurpose.boundary,
    _ => null,
  };
}

/// One row of [LeEditor.layer]: a layer-widget row's name and swatch
/// color - not tied to a physical Layer existing (BOUNDARY is a row like
/// any other). Visibility/selectability are set by [LeLayer.name] directly
/// ([LeEditor.setLayerNameVisible]/[setLayerNameSelectable]) - a whole row
/// together, not addressed by any id; see [LeLayerPurpose] for the other,
/// row-independent axis.
class LeLayer {
  const LeLayer({
    required this.name,
    required this.colorR,
    required this.colorG,
    required this.colorB,
  });

  final String name;

  /// The row's swatch color (0-255 per channel) - not the fill color or
  /// pattern, which are resolved per shape during rendering.
  final int colorR;
  final int colorG;
  final int colorB;
}

/// The current mouse position's coordinates in microns, snapped to the
/// minor grid (see [LeEditor.snappedMousePosition]) - the same point
/// [LeEditor.setMousePosition]'s grid-snap indicator box is centered on.
class LeSnappedMouse {
  const LeSnappedMouse({required this.xUm, required this.yUm});

  final double xUm;
  final double yUm;
}

/// One name/value row of a selected object's property table (UPDATES.md
/// 7.2) - see [LeEditor.selectedObjectProperties]. [value] is a `String`,
/// `int`, or `double` matching [type] (LE_PROPERTY_TYPE_STRING/_INT/
/// _DOUBLE respectively) - coordinates are always microns (double),
/// converted from the database's own dbu by the backend.
class LeSelectedProperty {
  const LeSelectedProperty({
    required this.name,
    required this.type,
    required this.value,
  });

  final String name;
  final LePropertyType type;
  final Object value;
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

  /// Total number of error/warning/info messages produced by this
  /// handle's backend operations so far (currently just [readLef] -
  /// file-open/parse errors, parser warnings, parser info notes, and a
  /// success summary, each already formatted with its own "ERROR "/
  /// "WARNING "/"INFO " prefix). Monotonically increasing - entries are
  /// never removed, cleared, or reordered - so a caller can poll this
  /// like [selectionVersion] and only fetch [messageAt] for indices at
  /// or past what it last saw.
  int get messageCount {
    _checkNotDisposed();
    return _bindings.le_message_count(_handle);
  }

  /// The message at [index] (0..[messageCount] - 1), or null if [index]
  /// is out of range.
  String? messageAt(int index) {
    _checkNotDisposed();
    final msgPtr = _bindings.le_message_at(_handle, index);
    if (msgPtr == ffi.nullptr) return null;
    return msgPtr.cast<pkg_ffi.Utf8>().toDartString();
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

  /// Number of Libraries currently loaded - one per [readLef] call so far.
  /// The top level of a Library -> Design -> Abstract browser widget; see
  /// [libraryDesignCount]/[libraryDesign] for the next level.
  int get libraryCount {
    _checkNotDisposed();
    return _bindings.le_library_count(_handle);
  }

  /// The Library at [index] (0..[libraryCount] - 1), or null if [index] is
  /// out of range.
  LeLibrary? library(int index) {
    _checkNotDisposed();
    final info = _bindings.le_library_at(_handle, index);
    if (info.name == ffi.nullptr) return null;
    return LeLibrary(
      id: LeLibraryRef(info.id.index, info.id.generation),
      name: info.name.cast<pkg_ffi.Utf8>().toDartString(),
    );
  }

  /// Number of Designs belonging to the Library at [libraryIndex] (into the
  /// same enumeration [library] uses).
  int libraryDesignCount(int libraryIndex) {
    _checkNotDisposed();
    return _bindings.le_library_design_count(_handle, libraryIndex);
  }

  /// The Design at [designIndex] within the Library at [libraryIndex]
  /// (0..[libraryDesignCount] - 1), or null if either index is out of
  /// range.
  LeDesignEntry? libraryDesign(int libraryIndex, int designIndex) {
    _checkNotDisposed();
    final info = _bindings.le_library_design_at(
      _handle,
      libraryIndex,
      designIndex,
    );
    if (info.name == ffi.nullptr) return null;
    final abstractId = info.abstract_id.index == 0xFFFFFFFF
        ? null
        : LeAbstractRef(info.abstract_id.index, info.abstract_id.generation);
    return LeDesignEntry(
      libraryId: LeLibraryRef(
        info.library_id.index,
        info.library_id.generation,
      ),
      id: LeDesignRef(info.id.index, info.id.generation),
      abstractId: abstractId,
      name: info.name.cast<pkg_ffi.Utf8>().toDartString(),
    );
  }

  /// Selects a Design by its stable [LeDesignRef] (e.g. one read from
  /// [libraryDesign]'s [LeDesignEntry.id]) as the one [renderPixelBuffer]
  /// renders - same effect as [setCurrentDesign] but addressed by identity
  /// instead of a position in the flat [designCount] list, the natural fit
  /// for a browser widget's row click. Returns true on success; the
  /// current selection is left unchanged on failure.
  bool setCurrentDesignById(LeDesignRef designId) {
    _checkNotDisposed();
    final idPtr = pkg_ffi.calloc<LeDesignId>();
    try {
      idPtr.ref.index = designId.index;
      idPtr.ref.generation = designId.generation;
      return _bindings.le_set_current_design_by_id(_handle, idPtr.ref) == 0;
    } finally {
      pkg_ffi.calloc.free(idPtr);
    }
  }

  /// Number of layer-widget rows currently available (0 before the first
  /// [readLef] call, since no ViewLayerSet has been built yet). Includes
  /// BOUNDARY and any future non-Technology-derived row, not just physical
  /// Layers. See [layer] for each row's contents.
  int get layerCount {
    _checkNotDisposed();
    return _bindings.le_layer_count(_handle);
  }

  /// The row at [rowIndex] (0..[layerCount] - 1), or null if out of range.
  LeLayer? layer(int rowIndex) {
    _checkNotDisposed();
    final row = _bindings.le_layer_at(_handle, rowIndex);
    if (row.name == ffi.nullptr) return null;
    return LeLayer(
      name: row.name.cast<pkg_ffi.Utf8>().toDartString(),
      colorR: row.color_r,
      colorG: row.color_g,
      colorB: row.color_b,
    );
  }

  /// Number of distinct purposes across the currently loaded ViewLayerSet -
  /// the layer widget's column axis, independent of any row. See
  /// [purposeAt] for each one.
  int get purposeCount {
    _checkNotDisposed();
    return _bindings.le_purpose_count(_handle);
  }

  /// The purpose at [index] (0..[purposeCount] - 1), or null if out of
  /// range.
  LeLayerPurpose? purposeAt(int index) {
    _checkNotDisposed();
    return LeLayerPurpose.fromValue(_bindings.le_purpose_at(_handle, index));
  }

  /// Whether every ViewLayer named [layerName] (a whole [LeLayer] row
  /// together, not one purpose-column) is currently visible - true by
  /// default until toggled with [setLayerNameVisible]. See
  /// [isPurposeVisible] for the other, row-independent axis.
  bool isLayerNameVisible(String layerName) {
    _checkNotDisposed();
    final namePtr = layerName.toNativeUtf8();
    try {
      return _bindings.le_is_layer_name_visible(_handle, namePtr.cast()) != 0;
    } finally {
      pkg_ffi.calloc.free(namePtr);
    }
  }

  /// Sets the visibility of every ViewLayer named [layerName] - e.g. a
  /// layer-widget row-header checkbox. Affects rendering.
  void setLayerNameVisible(String layerName, bool visible) {
    _checkNotDisposed();
    final namePtr = layerName.toNativeUtf8();
    try {
      _bindings.le_set_layer_name_visible(
        _handle,
        namePtr.cast(),
        visible ? 1 : 0,
      );
    } finally {
      pkg_ffi.calloc.free(namePtr);
    }
  }

  /// Whether every ViewLayer whose purpose is [purpose] (a whole column,
  /// not one row) is currently visible - true by default until toggled
  /// with [setPurposeVisible].
  bool isPurposeVisible(LeLayerPurpose purpose) {
    _checkNotDisposed();
    return _bindings.le_is_purpose_visible(_handle, purpose.index) != 0;
  }

  /// Sets the visibility of every ViewLayer whose purpose is [purpose] -
  /// e.g. a layer-widget column-header checkbox. Affects rendering.
  void setPurposeVisible(LeLayerPurpose purpose, bool visible) {
    _checkNotDisposed();
    _bindings.le_set_purpose_visible(_handle, purpose.index, visible ? 1 : 0);
  }

  /// Whether every ViewLayer named [layerName] is currently selectable -
  /// true by default until toggled with [setLayerNameSelectable]. Purely
  /// an interaction-layer concern (no hit-testing/click-to-select API
  /// exists yet to consult it) - doesn't affect rendering.
  bool isLayerNameSelectable(String layerName) {
    _checkNotDisposed();
    final namePtr = layerName.toNativeUtf8();
    try {
      return _bindings.le_is_layer_name_selectable(_handle, namePtr.cast()) !=
          0;
    } finally {
      pkg_ffi.calloc.free(namePtr);
    }
  }

  /// Sets the selectability of every ViewLayer named [layerName].
  void setLayerNameSelectable(String layerName, bool selectable) {
    _checkNotDisposed();
    final namePtr = layerName.toNativeUtf8();
    try {
      _bindings.le_set_layer_name_selectable(
        _handle,
        namePtr.cast(),
        selectable ? 1 : 0,
      );
    } finally {
      pkg_ffi.calloc.free(namePtr);
    }
  }

  /// Whether every ViewLayer whose purpose is [purpose] is currently
  /// selectable - true by default until toggled with
  /// [setPurposeSelectable].
  bool isPurposeSelectable(LeLayerPurpose purpose) {
    _checkNotDisposed();
    return _bindings.le_is_purpose_selectable(_handle, purpose.index) != 0;
  }

  /// Sets the selectability of every ViewLayer whose purpose is [purpose].
  void setPurposeSelectable(LeLayerPurpose purpose, bool selectable) {
    _checkNotDisposed();
    _bindings.le_set_purpose_selectable(
      _handle,
      purpose.index,
      selectable ? 1 : 0,
    );
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

  /// Spacing (dbu) between minor grid dots, drawn behind the design by
  /// [renderPixelBuffer]. Defaults to 5 (dbu), matching a 5nm minor grid
  /// under the common "1 dbu = 1nm" Technology convention.
  int get minorGridSpacing {
    _checkNotDisposed();
    return _bindings.le_minor_grid_spacing(_handle);
  }

  /// Sets the minor grid dot spacing (dbu). Affects rendering; values <= 0
  /// are ignored, same guard as [zoom]'s scale.
  set minorGridSpacing(int dbu) {
    _checkNotDisposed();
    _bindings.le_set_minor_grid_spacing(_handle, dbu);
  }

  /// Spacing (dbu) between major grid dots (drawn bolder than minor ones).
  /// Defaults to 50 (dbu), matching a 50nm major grid under the common
  /// "1 dbu = 1nm" Technology convention.
  int get majorGridSpacing {
    _checkNotDisposed();
    return _bindings.le_major_grid_spacing(_handle);
  }

  /// Sets the major grid dot spacing (dbu). Affects rendering; values <= 0
  /// are ignored.
  set majorGridSpacing(int dbu) {
    _checkNotDisposed();
    _bindings.le_set_major_grid_spacing(_handle, dbu);
  }

  /// Sets the current mouse position, in the same pixel space as
  /// [renderPixelBuffer]'s output (top-left origin, y increasing downward)
  /// and [zoom]'s x/y - feed straight from a pointer-move event. Drives
  /// the grid-snap indicator box [renderPixelBuffer] draws, and updates
  /// which selectable shape (if any) is hovered - drawn with a yellow
  /// outline - via a hit-test against the shapes currently on screen.
  /// Never invalidates the (potentially design-sized) rasterized design
  /// cache, only the small overlay picture, but the hit-test itself is
  /// bounded by the number of shapes currently visible (already
  /// viewport-culled), not the whole design - see backend/BENCHMARKS.md
  /// for measured cost on a large design.
  void setMousePosition(int x, int y) {
    _checkNotDisposed();
    _bindings.le_set_mouse_position(_handle, x, y);
  }

  /// Clears the current mouse position (e.g. on a pointer-leave event) so
  /// the grid-snap indicator box and any hover outline stop showing at/for
  /// the last known position.
  void clearMousePosition() {
    _checkNotDisposed();
    _bindings.le_clear_mouse_position(_handle);
  }

  /// The current mouse position's coordinates in microns, snapped to the
  /// minor grid - the same point [setMousePosition]'s grid-snap indicator
  /// box is centered on. Null if no mouse position has been set (or
  /// [clearMousePosition] was called since), or no Technology has been
  /// read yet ([readLef]) to convert dbu to microns with.
  LeSnappedMouse? get snappedMousePosition {
    _checkNotDisposed();
    final result = _bindings.le_snapped_mouse_position(_handle);
    if (result.has_position == 0) return null;
    return LeSnappedMouse(xUm: result.x_um, yUm: result.y_um);
  }

  /// Marks [keyCode] as currently held, e.g. on a key-down event - queried
  /// internally by gesture commands (e.g. [mouseUp]'s shift-click/
  /// shift-drag behavior).
  void keyDown(LeKeyCode keyCode) {
    _checkNotDisposed();
    _bindings.le_key_down(_handle, keyCode.value);
  }

  /// Marks [keyCode] as no longer held, e.g. on a key-up event.
  void keyUp(LeKeyCode keyCode) {
    _checkNotDisposed();
    _bindings.le_key_up(_handle, keyCode.value);
  }

  /// Whether [keyCode] is currently held (see [keyDown]).
  bool isKeyHeld(LeKeyCode keyCode) {
    _checkNotDisposed();
    return _bindings.le_is_key_held(_handle, keyCode.value) != 0;
  }

  /// Clears every currently-held key at once - call when the widget
  /// receiving key events loses focus (e.g. a `Focus` widget's
  /// `onFocusChange(false)`, see `LeEditorInput`/the example app). A
  /// key's matching key-up event is not guaranteed to still reach a
  /// widget that no longer has focus by the time the physical key is
  /// released, so without calling this on a focus loss, a modifier held
  /// at that moment stays "held" indefinitely - silently turning every
  /// later plain click into a shift-click until that same key happens to
  /// be pressed and released again while focused.
  void clearAllKeys() {
    _checkNotDisposed();
    _bindings.le_clear_all_keys(_handle);
  }

  /// Begins a mouse gesture at [x]/[y] (same pixel space as
  /// [setMousePosition]/[zoom]), e.g. on a pointer-down event. Records the
  /// gesture's anchor point; [mouseUp] later decides whether the gesture
  /// was a click or a rubber-band drag-select by comparing the down/up
  /// pixel distance against a small threshold.
  void mouseDown(int x, int y) {
    _checkNotDisposed();
    _bindings.le_mouse_down(_handle, x, y);
  }

  /// Begins a rectangle-zoom gesture at [x]/[y] (same pixel space as
  /// [mouseDown]), e.g. on a right-button pointer-down event. Like
  /// [mouseDown], only records the gesture's anchor point - [mouseUp] is
  /// the shared endpoint for both gesture kinds (see its own doc for why).
  void zoomDragDown(int x, int y) {
    _checkNotDisposed();
    _bindings.le_zoom_drag_down(_handle, x, y);
  }

  /// Ends a mouse gesture at [x]/[y], e.g. on a pointer-up event. A no-op
  /// if there was no matching [mouseDown]/[zoomDragDown] call first.
  ///
  /// - Started by [mouseDown]: click (small down/up distance) hit-tests the
  ///   single point; drag-select (larger distance) hit-tests every
  ///   selectable shape on every layer fully enclosed by the down/up
  ///   rectangle - either way, replaces the current selection unless
  ///   [LeKeyCode.LE_KEY_SHIFT] is currently held ([isKeyHeld]), in which
  ///   case the results are added to it instead.
  /// - Started by [zoomDragDown]: fits the viewport to the down/up
  ///   rectangle (same math as [fitScene] with 0 padding), unless the drag
  ///   was click-sized, in which case nothing happens. Selection is
  ///   untouched either way.
  void mouseUp(int x, int y) {
    _checkNotDisposed();
    _bindings.le_mouse_up(_handle, x, y);
  }

  /// Instructional text describing which mouse gestures and keyboard
  /// modifiers are currently available, for display in the GUI's status
  /// bar below the texture - e.g. "Left click to select. Shift for
  /// multi-select. Left click and drag for rectangle multi-select." for
  /// the current (and, for now, only) interaction mode, Select.
  String get tooltipMessage {
    _checkNotDisposed();
    final msgPtr = _bindings.le_tooltip_message(_handle);
    if (msgPtr == ffi.nullptr) return '';
    return msgPtr.cast<pkg_ffi.Utf8>().toDartString();
  }

  /// Number of currently selected objects. Indexes [selectedObjectKind]/
  /// [selectedObjectProperties]' own `selectionIndex` parameter,
  /// 0..this-1, in the same (insertion) order as the selection itself.
  int get selectionCount {
    _checkNotDisposed();
    return _bindings.le_selection_count(_handle);
  }

  /// Monotonic counter bumped only on an actual selection change (a
  /// select/deselect/clear that wasn't a no-op) - cheap to check on every
  /// pointer event to decide whether [selectionCount]/[selectedObjectKind]/
  /// [selectedObjectProperties] need re-fetching at all, instead of always
  /// re-fetching them (a real, measured cost that scales with how many
  /// objects are selected - see backend/BENCHMARKS.md).
  int get selectionVersion {
    _checkNotDisposed();
    return _bindings.le_selection_version(_handle);
  }

  /// Which database class the selected object at [selectionIndex] is, or
  /// null if [selectionIndex] is out of range (0..[selectionCount] - 1).
  LeSelectionKind? selectedObjectKind(int selectionIndex) {
    _checkNotDisposed();
    final kind = _bindings.le_selected_object_kind(_handle, selectionIndex);
    return kind < 0 ? null : LeSelectionKind.fromValue(kind);
  }

  /// Every property row (UPDATES.md 7.2) for the selected object at
  /// [selectionIndex] - e.g. a Terminal's name/direction/port count or an
  /// Obstruction's shape count, plus a shapes bounding box in microns for
  /// either, when one is available (see api.hpp's
  /// `le_selected_object_property_at` doc comment for the exact row
  /// list). Empty if [selectionIndex] is out of range.
  List<LeSelectedProperty> selectedObjectProperties(int selectionIndex) {
    _checkNotDisposed();
    final count = _bindings.le_selected_object_property_count(
      _handle,
      selectionIndex,
    );
    return [
      for (var propertyIndex = 0; propertyIndex < count; propertyIndex++)
        _selectedObjectProperty(selectionIndex, propertyIndex),
    ];
  }

  LeSelectedProperty _selectedObjectProperty(
    int selectionIndex,
    int propertyIndex,
  ) {
    final row = _bindings.le_selected_object_property_at(
      _handle,
      selectionIndex,
      propertyIndex,
    );
    final type = LePropertyType.fromValue(row.type);
    final Object value = switch (type) {
      LePropertyType.LE_PROPERTY_TYPE_STRING =>
        row.string_value.cast<pkg_ffi.Utf8>().toDartString(),
      LePropertyType.LE_PROPERTY_TYPE_INT => row.int_value,
      LePropertyType.LE_PROPERTY_TYPE_DOUBLE => row.double_value,
    };
    return LeSelectedProperty(
      name: row.name.cast<pkg_ffi.Utf8>().toDartString(),
      type: type,
      value: value,
    );
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

  /// Creates a Tcl console sharing this handle's own database (see
  /// [LeTclConsole] - TCL_EXPLORATION.md's show_gui design): native code
  /// embeds a Tcl interpreter pointed at this same `LeHandle*` via
  /// `set_session_handle`, so every CRUD/search command a caller runs
  /// through the returned [LeTclConsole] mutates the exact database this
  /// [LeEditor] is already rendering - call [LeTexture.markFrameAvailable]
  /// after each command to see the result, same as any other mutation.
  Future<LeTclConsole> createTclConsole() async {
    _checkNotDisposed();
    final consoleId = await _channel.invokeMethod<int>('createTclConsole', {
      'handleAddress': nativeHandleAddress,
    });
    if (consoleId == null) {
      throw StateError('createTclConsole returned null');
    }
    return LeTclConsole._(consoleId);
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
/// `minorGridSpacing`/`majorGridSpacing`/`setMousePosition`/
/// `clearMousePosition`/`keyDown`/`keyUp`/`mouseDown`/`mouseUp`/
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

/// A live Tcl console backed by one [LeEditor]'s native handle - obtain via
/// [LeEditor.createTclConsole]. Every [eval] call runs synchronously
/// (from Tcl's own point of view) on the native platform thread, against
/// an embedded `Tcl_Interp` sharing the source [LeEditor]'s own database
/// (see TCL_EXPLORATION.md's show_gui design) - a command that mutates the
/// database (`create_terminal`, `delete_shape`, ...) doesn't refresh the
/// screen on its own; call [LeTexture.markFrameAvailable] afterward, same
/// as any other mutating action.
///
/// **Lifetime:** the source [LeEditor] must outlive this [LeTclConsole] -
/// same constraint as [LeTexture], and for the same reason (native code
/// holds the raw handle address, not a reference keeping it alive).
class LeTclConsole {
  LeTclConsole._(this._consoleId);

  final int _consoleId;
  bool _disposed = false;

  /// Evaluates one Tcl command, returning the interpreter's string result
  /// whether the command succeeded or failed - Tcl already puts the error
  /// message in the same place on failure, so a caller checks the text
  /// itself rather than a separate success flag, matching how a real
  /// interactive Tcl shell prints either case.
  Future<String> eval(String command) async {
    if (_disposed) {
      throw StateError('eval called on a disposed LeTclConsole');
    }
    final result = await _channel.invokeMethod<String>('evalTclCommand', {
      'consoleId': _consoleId,
      'command': command,
    });
    return result ?? '';
  }

  /// Destroys the native Tcl interpreter. Safe to call more than once.
  Future<void> dispose() async {
    if (_disposed) return;
    _disposed = true;
    await _channel.invokeMethod<void>('disposeTclConsole', {
      'consoleId': _consoleId,
    });
  }
}
