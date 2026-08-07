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

  final List<String> _errors = [];
  List<String> get errors => _errors;

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

  final List<LeSelectedObjectInfo> _selectedObjects = [];
  List<LeSelectedObjectInfo> get selectedObjects => _selectedObjects;

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

  Future<void> refreshSelectedCount() async {
    _selectedCount = _editor.selectionCount;
  }

  Future<void> refreshSelectedObjects() async {
    _selectedObjects.clear();
    for (int i = 0; i < _editor.selectionCount; i++) {
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

  // TODO: this currently refreshed everything all the time, choose what to refresh more carefully.
  void refreshAndNotify() {
    refreshSnappedMousePosition();
    refreshSelectedCount();
    refreshSelectedObjects();
    refreshLayers();
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
      _errors.add("$path already open");
      refreshAndNotify();
      return;
    }

    if (_editor.readLef(path)) {
      _openLefFiles.add(path);
    } else {
      _errors.add("Unable to open $path");
    }
    refreshAndNotify();
  }

  Future<void> zoomIn(Offset offset) async {
    _editor.zoom(0.3, offset.dx.toInt(), offset.dy.toInt());
    refreshAndNotify();
  }

  Future<void> zoomOut(Offset offset) async {
    _editor.zoom(-0.3, offset.dx.toInt(), offset.dy.toInt());
    refreshAndNotify();
  }

  Future<void> fit({int padding = 10}) async {
    _editor.fitScene(padding);
    refreshAndNotify();
  }

  Future<void> panLeft() async {
    _editor.pan(-panFactor, 0);
    refreshAndNotify();
  }

  Future<void> panRight() async {
    _editor.pan(panFactor, 0);
    refreshAndNotify();
  }

  Future<void> panUp() async {
    _editor.pan(0, panFactor);
    refreshAndNotify();
  }

  Future<void> panDown() async {
    _editor.pan(0, -panFactor);
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

  // Future<void> setMousePosition(Offset offset) async {
  //   _editor.setMousePosition(offset.dx.toInt(), offset.dy.toInt());
  //   refreshAndNotify();
  // }

  // Future<void> setMouseDown(Offset offset) async {
  //   debugPrint("mouseDown: $offset");
  //   _editor.mouseDown(offset.dx.toInt(), offset.dy.toInt());
  //   refreshAndNotify();
  // }

  // Future<void> setMouseUp(Offset offset) async {
  //   debugPrint("mouseUp: $offset");
  //   _editor.mouseUp(offset.dx.toInt(), offset.dy.toInt());
  //   refreshAndNotify();
  // }
}
