import 'package:flutter/material.dart';
import 'package:lef_editor_plugin/lef_editor_plugin.dart';

class LeProvider extends ChangeNotifier {
  final LeEditor _editor = LeEditor();

  LeTexture? _texture;
  LeTexture? get texture => _texture;

  final List<String> _openLefFiles = [];

  final List<String> _openDesigns = [];
  List<String> get openDesigns => _openDesigns;

  final List<String> _errors = [];
  List<String> get errors => _errors;

  Future<void> init() async {
    _texture = await _editor.createTexture();
    notifyListeners();
  }

  Future<void> refreshTexture() async {
    await _texture?.markFrameAvailable();
  }

  Future<void> resize(Size size) async {
    debugPrint("Resizing viewport: $size");
    _editor.setViewportSize(size.width.toInt(), size.height.toInt());
    refreshTexture();
    notifyListeners();
  }

  Future<void> readLef(String path) async {
    if (_openLefFiles.contains(path)) {
      _errors.add("$path already open");
      notifyListeners();
      return;
    }

    if (_editor.readLef(path)) {
      _openLefFiles.add(path);
      updateOpenDesigns();
    } else {
      _errors.add("Unable to open $path");
    }
    refreshTexture();
    notifyListeners();
  }

  void updateOpenDesigns() {
    _openDesigns.clear();
    for (int i = 0; i < _editor.designCount; i++) {
      var designName = _editor.designName(i);
      if (designName != null) {
        _openDesigns.add(designName);
      }
    }
    refreshTexture();
    notifyListeners();
  }

  Future<void> openDesign(int index) async {
    if (!_editor.setCurrentDesign(index)) {
      _errors.add("Unable to select design at index $index");
      notifyListeners();
      return;
    }
    _editor.fitScene(10);
    refreshTexture();
    notifyListeners();
  }
}
