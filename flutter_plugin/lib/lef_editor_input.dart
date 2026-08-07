import 'package:flutter/gestures.dart';
import 'package:flutter/services.dart';

import 'lef_editor_plugin.dart';

/// LogicalKeyboardKey -> LeKeyCode. Extend alongside api.hpp's own
/// LeKeyCode enum as more interaction modes need shortcuts (see
/// LeEditor.keyDown's doc) - this is the one place a new key needs wiring
/// on the Dart side.
final Map<LogicalKeyboardKey, LeKeyCode> _keyCodeMap = {
  LogicalKeyboardKey.shiftLeft: LeKeyCode.LE_KEY_SHIFT,
  LogicalKeyboardKey.shiftRight: LeKeyCode.LE_KEY_SHIFT,
  LogicalKeyboardKey.keyZ: LeKeyCode.LE_KEY_ZOOM,
  LogicalKeyboardKey.keyF: LeKeyCode.LE_KEY_FIT,
  LogicalKeyboardKey.arrowLeft: LeKeyCode.LE_KEY_PAN_LEFT,
  LogicalKeyboardKey.arrowRight: LeKeyCode.LE_KEY_PAN_RIGHT,
  LogicalKeyboardKey.arrowUp: LeKeyCode.LE_KEY_PAN_UP,
  LogicalKeyboardKey.arrowDown: LeKeyCode.LE_KEY_PAN_DOWN,
};

/// Converts standard Flutter `PointerEvent`/`KeyEvent` input straight into
/// [LeEditor]'s low-level selection API (`mouseDown`/`mouseUp`/
/// `setMousePosition`/`clearMousePosition`/`keyDown`/`keyUp`), so a caller
/// doesn't have to hand-roll a `LogicalKeyboardKey` -> `LeKeyCode` mapping
/// or pixel-space bookkeeping itself (see `example/lib/main.dart` for a
/// working `Listener`/`MouseRegion`/`Focus` wiring).
extension LeEditorInput on LeEditor {
  /// Converts a standard Flutter pointer event straight into the right
  /// low-level call - [x]/[y] come from [PointerEvent.localPosition],
  /// `.round()`'d, in the same raw-logical-pixel convention
  /// [LeEditor.setViewportSize]/[LeEditor.zoom]/[LeEditor.setMousePosition]
  /// already use everywhere in this codebase (no devicePixelRatio scaling -
  /// see CLAUDE.md). Every `PointerEvent` subtype is a plain, non-
  /// `covariant` Dart function parameter, so this method (typed `void
  /// Function(PointerEvent)`) is directly tear-off-assignable to a
  /// `Listener`'s onPointerDown/onPointerMove/onPointerHover/onPointerUp/
  /// onPointerCancel AND a `MouseRegion`'s onEnter/onHover/onExit - no
  /// wrapper closure needed for the conversion itself (though a real call
  /// site still wants one, to also call `LeTexture.markFrameAvailable()`
  /// after - this plugin's texture is pull-based, nothing repaints on its
  /// own).
  void handlePointerEvent(PointerEvent event) {
    final x = event.localPosition.dx.round();
    final y = event.localPosition.dy.round();
    switch (event) {
      case PointerDownEvent():
        setMousePosition(x, y);
        // Only the primary (left) button drives selection - a right-click
        // is left for a future context-menu mode, not a click/drag-select
        // gesture. `buttons` at down-time is the button(s) that just went
        // down, so this check is reliable here (unlike on the matching
        // PointerUpEvent below, where `buttons` reflects what's *still*
        // held post-release and can't identify which button was lifted -
        // so mouseUp is forwarded unconditionally instead; le_mouse_up is
        // already a no-op without a preceding le_mouse_down, so a right-
        // click's up event is harmless).
        if (event.buttons & kPrimaryButton != 0) mouseDown(x, y);
      case PointerUpEvent():
        setMousePosition(x, y);
        mouseUp(x, y);
      case PointerMoveEvent():
      case PointerHoverEvent():
      case PointerEnterEvent():
        setMousePosition(x, y);
      case PointerCancelEvent():
      case PointerExitEvent():
        // No le_mouse_cancel exists - an unmatched mouseDown anchor is
        // inert until the next real mouseDown overwrites it (see
        // mouseUp's own doc), so only the hover/grid-snap visual needs
        // clearing here; self-correcting via the next hover/move anyway.
        clearMousePosition();
      default:
        break; // e.g. scroll/pan/zoom signal events - not this API's job.
    }
  }

  /// Converts a standard Flutter key event straight into [LeEditor.keyDown]/
  /// [LeEditor.keyUp] for whichever [LeKeyCode] it maps to (see
  /// `_keyCodeMap`), or does nothing and returns false for any other key. A
  /// [KeyRepeatEvent] is just an extra, harmless `keyDown` call (it only
  /// marks a key "held").
  ///
  /// Returns whether the key was recognized, for a caller's own
  /// `Focus.onKeyEvent` (a different signature - `(FocusNode, KeyEvent) ->
  /// KeyEventResult` - so it always needs its own small wrapper, and often
  /// has other shortcuts of its own to interleave):
  /// ```dart
  /// onKeyEvent: (node, event) =>
  ///     editor.handleKeyEvent(event) ? KeyEventResult.handled : myOtherHandling(event),
  /// ```
  bool handleKeyEvent(KeyEvent event) {
    final keyCode = _keyCodeMap[event.logicalKey];
    if (keyCode == null) return false;
    switch (event) {
      case KeyDownEvent():
      case KeyRepeatEvent():
        keyDown(keyCode);
      case KeyUpEvent():
        keyUp(keyCode);
      default:
        return false;
    }
    return true;
  }

  /// Converts a `Focus` widget's `onFocusChange` straight into
  /// [LeEditor.clearAllKeys] on a focus *loss* - a no-op on gaining focus.
  /// A key's matching key-up event is not guaranteed to still reach a
  /// widget that no longer has focus by the time the physical key is
  /// released, so without this, a modifier (e.g. shift) held at the
  /// moment focus is lost would stay "held" indefinitely from the
  /// backend's point of view - silently turning every later plain click
  /// into a shift-click. Wire directly into `Focus.onFocusChange` (see
  /// `example/lib/main.dart`):
  /// ```dart
  /// onFocusChange: editor.handleFocusChange,
  /// ```
  void handleFocusChange(bool hasFocus) {
    if (!hasFocus) clearAllKeys();
  }
}
