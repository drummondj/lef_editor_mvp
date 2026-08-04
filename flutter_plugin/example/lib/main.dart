import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart' show rootBundle;

import 'package:lef_editor_plugin/lef_editor_plugin.dart';

const int _viewportPx = 400;

void main() {
  runApp(const MyApp());
}

class MyApp extends StatefulWidget {
  const MyApp({super.key});

  @override
  State<MyApp> createState() => _MyAppState();
}

class _MyAppState extends State<MyApp> {
  final LeEditor _editor = LeEditor();
  LeTexture? _texture;
  String _status = 'Loading testcell.lef...';

  @override
  void initState() {
    super.initState();
    _load();
  }

  Future<void> _load() async {
    // le_read_lef() takes a real filesystem path (it drives a C LEF parser
    // that opens the file itself) - a bundled Flutter asset isn't one, so
    // copy it out to a temp file first. Fine for this demo; a real app
    // would more likely load a user-chosen file that's already on disk.
    final bytes = await rootBundle.load('assets/testcell.lef');
    final tempDir = await Directory.systemTemp.createTemp('lef_editor_plugin_example');
    final tempFile = File('${tempDir.path}/testcell.lef');
    await tempFile.writeAsBytes(bytes.buffer.asUint8List());

    if (!_editor.readLef(tempFile.path)) {
      setState(() => _status = 'Failed to read ${tempFile.path}');
      return;
    }
    if (_editor.designCount == 0 || !_editor.setCurrentDesign(0)) {
      setState(() => _status = 'No Design found in testcell.lef');
      return;
    }

    // testcell.lef's TESTCELL macro is 10x10 microns; UNITS DATABASE
    // MICRONS 1000 makes that 10000x10000 dbu. Fixed viewport/pan/scale
    // for this demo (not a real fit-to-content computation, unlike
    // backend/src/pipeline/benchmarks/render_preview.cpp's own fitting -
    // this is close enough to comfortably frame a cell this small).
    _editor
      ..setViewportSize(_viewportPx, _viewportPx)
      ..setScale(0.03)
      ..setPan(-500, -500);

    final texture = await _editor.createTexture();
    await texture.markFrameAvailable();

    setState(() {
      _texture = texture;
      _status = 'Design: ${_editor.designName(0)}';
    });
  }

  @override
  void dispose() {
    // Texture must be disposed before the editor - see LeTexture's own
    // lifetime doc. dispose() itself can't be async, so this is
    // best-effort (fine for an example app's single top-level widget).
    _texture?.dispose();
    _editor.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final texture = _texture;
    return MaterialApp(
      home: Scaffold(
        appBar: AppBar(title: const Text('lef_editor_plugin')),
        body: Center(
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              Text(_status, style: const TextStyle(fontSize: 18)),
              const SizedBox(height: 16),
              if (texture != null)
                SizedBox(
                  width: _viewportPx.toDouble(),
                  height: _viewportPx.toDouble(),
                  child: DecoratedBox(
                    decoration: BoxDecoration(border: Border.all(color: Colors.grey)),
                    child: Texture(textureId: texture.textureId),
                  ),
                ),
            ],
          ),
        ),
      ),
    );
  }
}
