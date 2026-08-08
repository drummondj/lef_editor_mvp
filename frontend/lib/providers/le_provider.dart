import 'package:flutter/material.dart';
import 'package:lef_editor_plugin/lef_editor_plugin.dart';

class LeLayerInfo {
  int index;
  LeLayer layer;
  bool isSelectable;
  bool isVisible;

  LeLayerInfo({
    required this.index,
    required this.layer,
    required this.isSelectable,
    required this.isVisible,
  });
}

class LePurposeInfo {
  LeLayerPurpose purpose;
  bool isSelectable;
  bool isVisible;
  LePurposeInfo({
    required this.purpose,
    required this.isSelectable,
    required this.isVisible,
  });
}

class LeSelectedObjectInfo {
  LeSelectionKind kind;
  List<LeSelectedProperty> properties;
  LeSelectedObjectInfo({required this.kind, required this.properties});
}

class LeProvider extends ChangeNotifier {
  final double panFactor = 0.25;
  final LeEditor _editor = LeEditor();

  LeTexture? _texture;
  LeTexture? get texture => _texture;

  final List<String> _openLefFiles = [];

  final List<String> _messages = ["INFO: Welcome to LayoutEngine"];
  List<String> get messages => _messages;

  final List<LeLayerInfo> _layers = [];
  List<LeLayerInfo> get layers => _layers;

  final List<LePurposeInfo> _layerPurposes = [];
  List<LePurposeInfo> get layerPurposes => _layerPurposes;

  bool _allVisible = true;
  bool get allVisible => _allVisible;

  bool _allSelectable = true;
  bool get allSelectable => _allSelectable;

  bool _allLayersVisible = true;
  bool get allLayersVisible => _allLayersVisible;

  bool _allLayersSelectable = true;
  bool get allLayersSelectable => _allLayersSelectable;

  bool _allPurposesVisible = true;
  bool get allPurposesVisible => _allPurposesVisible;

  bool _allPurposesSelectable = true;
  bool get allPurposesSelectable => _allPurposesSelectable;

  Offset _snappedMousePosition = Offset.zero;
  Offset get snappedMousePosition => _snappedMousePosition;

  int _selectedCount = 0;
  int get selectedCount => _selectedCount;

  String _tooltipMessage = '';
  String get tooltipMessage => _tooltipMessage;

  final List<LeSelectedObjectInfo> _selectedObjects = [];
  List<LeSelectedObjectInfo> get selectedObjects => _selectedObjects;

  // -1 so the first refreshSelection() call (selectionVersion is always
  // >= 0) always does a real refresh.
  int _lastSelectionVersion = -1;

  // Cursor into the backend's own message queue (le_message_count/
  // le_message_at) - messages there are never removed/reordered, so
  // this just needs to remember how many we've already pulled in.
  // Starts at 0 (not -1 like _lastSelectionVersion): messageCount is
  // also 0 before any backend operation has run, so "drain nothing yet"
  // is already the correct behavior with no special sentinel needed.
  int _lastMessageCount = 0;

  Future<void> refreshTexture() async {
    await _texture?.markFrameAvailable();
  }

  Future<void> refreshSnappedMousePosition() async {
    if (_editor.snappedMousePosition != null) {
      _snappedMousePosition = Offset(
        _editor.snappedMousePosition!.xUm,
        _editor.snappedMousePosition!.yUm,
      );
    }
  }

  // A cheap direct read (no version-gating needed, unlike
  // refreshSelection/refreshMessages - there's no list to accumulate
  // and nothing costly to skip).
  void refreshTooltipMessage() {
    _tooltipMessage = _editor.tooltipMessage;
  }

  // Rebuilding _selectedObjects makes several FFI calls per selected
  // object (selectedObjectKind + selectedObjectProperties, itself one
  // call per property) - proportional to selection size, not free. Gated
  // on selectionVersion (bumped only on an actual select/deselect/clear,
  // not on every pointer event) so a mouse move that doesn't change the
  // selection skips this entirely instead of paying that cost on every
  // frame - see backend/BENCHMARKS.md for the measured cost this fixes.
  Future<void> refreshSelection() async {
    final version = _editor.selectionVersion;
    if (version == _lastSelectionVersion) {
      return;
    }
    _lastSelectionVersion = version;

    _selectedCount = _editor.selectionCount;
    _selectedObjects.clear();
    for (int i = 0; i < _selectedCount; i++) {
      final kind = _editor.selectedObjectKind(i);
      if (kind != null) {
        _selectedObjects.add(
          LeSelectedObjectInfo(
            kind: kind,
            properties: _editor.selectedObjectProperties(i),
          ),
        );
      }
    }
  }

  // Pulls in any backend messages (LEF read errors/warnings/info - see
  // backend's le_message_count/le_message_at, UPDATES.md item 3) added
  // since the last check, in order, without re-adding ones already
  // seen - same incremental-fetch shape as refreshSelection's own
  // selectionVersion gate above, just for a queue instead of a
  // change-counter.
  Future<void> refreshMessages() async {
    final count = _editor.messageCount;
    for (int i = _lastMessageCount; i < count; i++) {
      final message = _editor.messageAt(i);
      if (message != null) {
        _messages.add(message);
      }
    }
    _lastMessageCount = count;
  }

  // TODO: this currently refreshed everything all the time (except
  // selection, see refreshSelection), choose what to refresh more
  // carefully.
  void refreshAndNotify() {
    refreshSnappedMousePosition();
    refreshSelection();
    refreshLayers();
    refreshMessages();
    refreshTooltipMessage();
    refreshTexture();
    notifyListeners();
  }

  Future<void> init() async {
    _texture = await _editor.createTexture();
    refreshAndNotify();
  }

  Future<void> resize(Size size) async {
    _editor.setViewportSize(size.width.toInt(), size.height.toInt());
    refreshAndNotify();
  }

  Future<void> readLef(String path) async {
    if (_openLefFiles.contains(path)) {
      _messages.add("ERROR: $path already open");
      refreshAndNotify();
      return;
    }
    _messages.add("INFO: Reading $path");
    if (_editor.readLef(path)) {
      _openLefFiles.add(path);
    }
    // On failure, the backend's own le_message_count/le_message_at
    // queue already has a more specific error (e.g. "ERROR: Could not
    // open LEF file ..." or a real parser diagnostic) - refreshAndNotify
    // below (via refreshMessages) pulls it in, so no generic fallback
    // message is added here.
    refreshAndNotify();
  }

  Future<List<LeLibrary>> getLibraries() async {
    List<LeLibrary> libraries = [];
    for (int i = 0; i < _editor.libraryCount; i++) {
      var library = _editor.library(i);
      if (library != null) {
        libraries.add(library);
      }
    }
    return libraries;
  }

  Future<List<LeDesignEntry>> getDesigns(int libraryIndex) async {
    List<LeDesignEntry> designs = [];
    for (int i = 0; i < _editor.libraryDesignCount(libraryIndex); i++) {
      var design = _editor.libraryDesign(libraryIndex, i);
      if (design != null) {
        designs.add(design);
      }
    }
    return designs;
  }

  Future<void> openDesign(LeDesignRef designRef) async {
    _editor.setCurrentDesignById(designRef);
    _editor.fitScene(10);
    refreshAndNotify();
  }

  Future<void> refreshLayers() async {
    _layers.clear();
    _layerPurposes.clear();
    _allLayersSelectable = true;
    _allLayersVisible = true;
    _allPurposesSelectable = true;
    _allPurposesVisible = true;

    for (int i = 0; i < _editor.layerCount; i++) {
      var layer = _editor.layer(i);
      if (layer != null) {
        bool isSelectable = _editor.isLayerNameSelectable(layer.name);
        bool isVisible = _editor.isLayerNameVisible(layer.name);
        var layerInfo = LeLayerInfo(
          index: i,
          layer: layer,
          isSelectable: isSelectable,
          isVisible: isVisible,
        );
        _layers.add(layerInfo);
        if (!isSelectable) {
          _allLayersSelectable = false;
        }
        if (!isVisible) {
          _allLayersVisible = false;
        }
      }
    }
    for (int i = 0; i < _editor.purposeCount; i++) {
      var purpose = _editor.purposeAt(i);
      if (purpose != null) {
        bool isSelectable = _editor.isPurposeSelectable(purpose);
        bool isVisible = _editor.isPurposeVisible(purpose);
        var purposeInfo = LePurposeInfo(
          purpose: purpose,
          isSelectable: isSelectable,
          isVisible: isVisible,
        );
        _layerPurposes.add(purposeInfo);
        if (!isSelectable) {
          _allPurposesSelectable = false;
        }
        if (!isVisible) {
          _allPurposesVisible = false;
        }
      }
    }
    _allVisible = _allLayersVisible && _allPurposesVisible;
    _allSelectable = _allLayersSelectable && _allLayersSelectable;

    notifyListeners();
  }

  Future<void> setLayerVisibility(LeLayerInfo layerInfo, bool? visible) async {
    _editor.setLayerNameVisible(layerInfo.layer.name, visible ?? false);
    refreshAndNotify();
  }

  Future<void> setLayerSelectable(
    LeLayerInfo layerInfo,
    bool? selectable,
  ) async {
    _editor.setLayerNameSelectable(layerInfo.layer.name, selectable ?? false);
    refreshAndNotify();
  }

  Future<void> setPurposeVisible(
    LePurposeInfo purposeInfo,
    bool? visible,
  ) async {
    _editor.setPurposeVisible(purposeInfo.purpose, visible ?? false);
    refreshAndNotify();
  }

  Future<void> setPurposeSelectable(
    LePurposeInfo purposeInfo,
    bool? visible,
  ) async {
    _editor.setPurposeSelectable(purposeInfo.purpose, visible ?? false);
    refreshAndNotify();
  }

  Future<void> setAllLayersVisible(bool? visible) async {
    for (var layerInfo in _layers) {
      await setLayerVisibility(layerInfo, visible);
    }
  }

  Future<void> setAllLayersSelectable(bool? selectable) async {
    for (var layerInfo in _layers) {
      await setLayerSelectable(layerInfo, selectable);
    }
  }

  Future<void> setAllPurposesVisible(bool? visible) async {
    for (var purposeInfo in _layerPurposes) {
      await setPurposeVisible(purposeInfo, visible);
    }
  }

  Future<void> setAllPurposesSelectable(bool? selectable) async {
    for (var purposeInfo in _layerPurposes) {
      await setPurposeSelectable(purposeInfo, selectable);
    }
  }

  Future<void> setAllVisible(bool? visible) async {
    await setAllLayersVisible(visible);
    await setAllPurposesVisible(visible);
  }

  Future<void> setAllSelectable(bool? selectable) async {
    await setAllLayersSelectable(selectable);
    await setAllPurposesSelectable(selectable);
  }

  void handlePointerEvent(PointerEvent event) {
    _editor.handlePointerEvent(event);
    refreshAndNotify();
  }

  bool handleKeyEvent(KeyEvent event) {
    if (_editor.handleKeyEvent(event)) {
      refreshAndNotify();
      return true;
    } else {
      return false;
    }
  }

  void handleFocusChange(bool hasFocus) {
    _editor.handleFocusChange(hasFocus);
  }
}
