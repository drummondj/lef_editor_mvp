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
  final FocusNode _focusNode = FocusNode();
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
    final tempDir = await Directory.systemTemp.createTemp(
      'lef_editor_plugin_example',
    );
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

    _editor
      ..setViewportSize(_viewportPx, _viewportPx)
      ..fitScene(16);

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
    _focusNode.dispose();
    super.dispose();
  }

  // Demonstrates LeEditorInput (see lib/lef_editor_input.dart): converts the
  // Listener/MouseRegion's standard PointerEvent straight into LeEditor's
  // mouseDown/mouseUp/setMousePosition/clearMousePosition calls. Still a
  // closure (not a bare tear-off) because this plugin's texture is
  // pull-based - nothing repaints until markFrameAvailable() is called.
  //
  // Explicitly re-requests focus on every pointer-down: a plain
  // Listener/MouseRegion doesn't request focus for an ancestor Focus
  // widget on its own (unlike e.g. a TextField), so without this, clicking
  // away to some other widget and back would leave _focusNode permanently
  // unfocused - onKeyEvent would stop firing at all (not just shift
  // specifically), and clearAllKeys (see handleFocusChange) would never
  // get a chance to run either.
  void _onPointerEvent(PointerEvent event) {
    if (event is PointerDownEvent) _focusNode.requestFocus();
    _editor.handlePointerEvent(event);
    _texture?.markFrameAvailable();
    setState(() {}); // refresh the "Selected: N" line below
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
              if (texture != null) Text('Selected: ${_editor.selectionCount}'),
              const SizedBox(height: 16),
              if (texture != null)
                Focus(
                  focusNode: _focusNode,
                  autofocus: true,
                  onFocusChange: _editor.handleFocusChange,
                  onKeyEvent: (node, event) {
                    final handled = _editor.handleKeyEvent(event);
                    if (handled) _texture?.markFrameAvailable();
                    return handled
                        ? KeyEventResult.handled
                        : KeyEventResult.ignored;
                  },
                  child: MouseRegion(
                    onExit: _onPointerEvent,
                    child: Listener(
                      onPointerDown: _onPointerEvent,
                      onPointerMove: _onPointerEvent,
                      onPointerHover: _onPointerEvent,
                      onPointerUp: _onPointerEvent,
                      onPointerCancel: _onPointerEvent,
                      child: SizedBox(
                        width: _viewportPx.toDouble(),
                        height: _viewportPx.toDouble(),
                        child: DecoratedBox(
                          decoration: BoxDecoration(
                            border: Border.all(color: Colors.grey),
                          ),
                          child: Texture(textureId: texture.textureId),
                        ),
                      ),
                    ),
                  ),
                ),
            ],
          ),
        ),
      ),
    );
  }
}
